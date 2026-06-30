// claymore — a hub over sgiandubh spokes (C++; cpp-httplib + nlohmann/json, one small binary).
//
// Fans a query out to N bounded-expert spokes (each an OpenAI-compatible sgiandubh server), drops the ones that
// ABSTAIN (off-domain — the bound IS the router), ranks the survivors by confidence, and answers in one of two modes:
//   deterministic  — return the top cited answer verbatim (keeps every spoke guarantee; no LLM).
//   llm            — synthesize across the cited answers with a hub LLM (LOCAL llama.cpp server OR a remote API;
//                    OpenAI- or Anthropic-shaped). If every spoke abstains, claymore refuses IN CODE.
//
// claymore exposes the SAME API surface as sgiandubh — OpenAI /v1/chat/completions + /v1/completions + /v1/models,
// Anthropic /v1/messages, SSE streaming, and response_format json_object — so clients see one drop-in endpoint.
#include "httplib.h"
#include "json.hpp"
#include "md_render.h"
#include "repl_tui.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <future>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
using json = nlohmann::json;

static bool g_tty = false;   // stdout is an interactive terminal → render markdown/TeX in the REPL (set in main)

// A spoke can front N REDUNDANT replicas of the same expert (identical sgiandubh copies — or another claymore, since
// they share an API) for reliability (failover) + scale (load spread). One `url` → one replica; `urls`/`replicas` → many.
// role: "" = a content expert (fanned out + ask/list/lookup); "librarian" = a catalog expert backing find/catalog tools.
struct Spoke { std::string name, domain, role, key; std::vector<std::string> urls; };
// `key` (optional): a SUBSET key forwarded to the spoke so this entry only sees a slice of a larger expert. Two spoke
// entries can share a url but differ in key → one big sgiandubh fronted as several bounded subset-experts.
struct SAns {
    std::string answer, kind, citation, citation_id, source, spoke, route;  // route: RETRIEVED|SELECTED|COMPOSED tier
    double confidence = -1;                         // citation_id = the spoke's callable handle (→ /lookup)
    double margin = 1e9;                            // thinnest-decision margin (1e9 = absent) — fragile-link signal
    bool ok = false;
};
struct Src { std::string spoke, citation, id; };  // a federated citation: which spoke, the label, the callable handle (id)
struct Result {                                   // the hub's answer + provenance
    std::string body, mode, route;                // body = answer text (no inline tag); mode = deterministic|llm|abstain
    double margin = 1e9;
    std::vector<Src> sources;                      // (spoke, citation, id) — id + spoke make the cite resolvable via /lookup
    std::vector<std::string> suggestions;          // tutor sessions: suggested next prompts for the learner (may be empty)
};

static std::vector<Spoke> g_spokes;
static std::string g_mode = "deterministic";
static int g_top_k = 3;
static std::string g_tool_style = "generic";      // tools mode: "generic" (one consult_experts tool) | "per-expert"
static bool g_verbose = false;                     // --verbose / -v: trace the tools loop (tool calls, spoke results, why it refused)
static double g_min_relevance = 0.10;              // --min-relevance: drop a spoke response sharing < this fraction (Jaccard) with the query
static json g_synth = json::object();             // {url, model, api_key_env, format:"openai"|"anthropic", max_tokens}
static const char* REFUSE = "I don't have anything on that in the available material.";

// split "https://api.openai.com/v1" -> origin "https://api.openai.com", base "/v1"
static void split_url(const std::string& url, std::string& origin, std::string& base) {
    auto p = url.find("://");
    size_t hs = (p == std::string::npos) ? 0 : p + 3;
    auto slash = url.find('/', hs);
    if (slash == std::string::npos) { origin = url; base = ""; }
    else { origin = url.substr(0, slash); base = url.substr(slash); }
    if (!base.empty() && base.back() == '/') base.pop_back();
}

static std::atomic<unsigned> g_rr{0};   // round-robin cursor for replica selection (spreads load across copies)

// Try `fn(origin, base)` against the spoke's replicas: round-robin START (load spread) + FAILOVER (on a replica that's
// down/erroring, move to the next). fn returns true if the replica RESPONDED (stop), false to fail over to the next.
// Returns true if any replica responded. A replica that legitimately abstains has "responded" — we don't fail over
// for that (the replicas are identical copies; only transport/HTTP failures warrant trying another).
template <class F>
static bool call_replica(const Spoke& sp, F&& fn) {
    size_t k = sp.urls.size();
    if (k == 0) return false;
    unsigned start = g_rr.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < k; i++) {
        std::string origin, base;
        split_url(sp.urls[(start + i) % k], origin, base);
        if (fn(origin, base)) return true;
    }
    return false;
}

// The callable handle inside a citation: "norm:id · Section" → "norm:id" (the part before the middle dot). Used when a
// source has no explicit citation_id (e.g. /retrieve matches carry only a section string).
static std::string cite_id(const std::string& c) {
    auto dot = c.find("\xC2\xB7");                                       // UTF-8 middle dot (·)
    std::string s = (dot == std::string::npos) ? c : c.substr(0, dot);
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static bool is_abstain(const std::string& a, const std::string& kind) {
    return kind == "abstain" || a.empty() ||
           a.find("isn't covered") != std::string::npos || a.find("Try rephrasing") != std::string::npos;
}

// Hub-side relevance gate (defense-in-depth, NOT a substitute for spoke abstain): how much of the query's content
// overlaps the response (answer + citation). A near-zero overlap means the spoke returned something off-topic even
// though it didn't abstain (e.g. a RISC-V vector rule for "predicate calculus") — drop it before citing/synthesizing.
// Lexical + conservative by design: it only catches the egregious cases; the spoke + the LLM remain the real filters.
static std::set<std::string> content_words(const std::string& s) {
    static const std::set<std::string> STOP = {
        "the","is","are","was","were","be","been","a","an","of","to","in","on","for","and","or","but","what","which",
        "who","how","why","when","where","do","does","did","you","your","it","its","that","this","these","those",
        "with","as","at","by","from","about","can","could","would","should","i","we","me","my","please","explain",
        "list","give","tell","get","all","any","some","there"};
    std::set<std::string> w; std::string cur;
    auto flush = [&] { if (cur.size() > 1 && !STOP.count(cur)) w.insert(cur); cur.clear(); };
    for (char c : s) { if (std::isalnum((unsigned char)c)) cur += (char)std::tolower((unsigned char)c); else flush(); }
    flush();
    return w;
}
static double relevance(const std::string& query, const std::string& response) {
    auto q = content_words(query), r = content_words(response);
    if (q.empty()) return 1.0;                          // no content words to judge → don't gate
    size_t inter = 0;
    for (const auto& x : q) if (r.count(x)) inter++;
    return (double)inter / (double)(q.size() + r.size() - inter);   // Jaccard
}
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
// Sentinel separating a pedagogy template's system scaffold from its JSON tutor-metadata tail (opener + scope
// restriction), emitted by rosetta's pedagogy adapter. The subject↔domain match below this floor doesn't bind a tutor.
static const std::string TUTOR_META = "[[TUTOR_META]]";
static const double TUTOR_SUBJECT_MIN = 0.05;

// ---- spokes ----------------------------------------------------------------------------------------------------
static SAns ask_spoke(const Spoke& sp, const std::string& query, const std::string& key = "") {
    SAns r;
    const std::string ek = key.empty() ? sp.key : key;            // session/tool key overrides the spoke's configured key
    call_replica(sp, [&](const std::string& origin, const std::string& base) -> bool {
    httplib::Client cli(origin);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(15);
    json req;
    req["messages"] = json::array({json{{"role", "user"}, {"content", query}}});
    req["response_format"] = json{{"type", "json_object"}};
    if (!ek.empty()) req["key"] = ek;                             // confine this call to a content slice of the expert
    auto res = cli.Post(base + "/v1/chat/completions", req.dump(), "application/json");
    if (!res || res->status != 200) return false;             // replica down/erroring → fail over to the next
    try {
        std::string content = json::parse(res->body)["choices"][0]["message"]["content"].get<std::string>();
        json a;
        try { a = json::parse(content); }
        catch (...) { a = json{{"answer", content}, {"kind", "distilled"}}; }
        std::string ans = a.value("answer", ""), kind = a.value("kind", "distilled");
        if (is_abstain(ans, kind)) return true;        // replica abstained — a valid response, not a failure (r.ok stays false)
        r.answer = ans; r.kind = kind; r.spoke = sp.name;
        r.citation = a.value("citation", ""); r.source = a.value("source", "");
        r.citation_id = a.value("citation_id", "");      // the spoke's callable handle (sgiandubh emits it)
        r.route = a.value("route", "");
        if (a.contains("margin") && a["margin"].is_number()) r.margin = a["margin"].get<double>();
        if (a.contains("confidence") && a["confidence"].is_number()) r.confidence = a["confidence"].get<double>();
        r.ok = true;
        // Relevance = best of (query↔response) and (query↔spoke domain). The domain term keeps a legit in-domain query
        // phrased with synonyms the corpus doesn't use ("integer RISC operators" vs the corpus's "instructions") —
        // while a truly off-topic query (matching neither the response nor the domain) is still dropped.
        double rel = std::max(relevance(query, r.answer + " " + r.citation + " " + r.source),
                              relevance(query, sp.domain));
        if (rel < g_min_relevance) {
            if (g_verbose) fprintf(stderr, "[claymore] dropped %s response (relevance %.2f < %.2f — off-topic)\n",
                                   sp.name.c_str(), rel, g_min_relevance);
            r.ok = false;                               // hub relevance gate: backstop for a spoke that didn't abstain
        }
        return true;                                    // replica responded (even if off-topic) — don't fail over
    } catch (...) { return false; }                     // malformed reply → fail over to the next replica
    });
    return r;
}

static double score(const SAns& a) {
    if (a.confidence >= 0) return a.confidence;
    if (a.kind == "retrieved") return 0.9;
    if (a.kind == "distilled") return 0.6;
    if (a.kind == "generated") return 0.3;
    return 0.5;
}

static std::vector<SAns> fan_out(const std::string& query, const std::string& key = "") {
    std::vector<std::future<SAns>> futs;
    for (const auto& sp : g_spokes)                       // every spoke participates — a catalog expert answers catalog
        futs.push_back(std::async(std::launch::async, ask_spoke, std::cref(sp), std::cref(query), key));  // queries, the
    // relevance gate drops its cards from unrelated answers. (No exclusion — librarians must federate, not be hidden.)
    std::vector<SAns> res;
    for (auto& f : futs) { SAns a = f.get(); if (a.ok) res.push_back(a); }
    std::sort(res.begin(), res.end(), [](const SAns& x, const SAns& y) { return score(x) > score(y); });
    return res;
}

// ---- structured retrieval (aggregation) — call the spoke's /retrieve extension for a SET of cited passages -------
// This is the hub's standard-tool wrapper over the spoke's non-/v1 /retrieve primitive: the LLM calls a normal
// tool, claymore turns it into /retrieve calls. For "list / table / all X" queries the single-best path can't serve.
struct Match { std::string spoke, section, passage; double score = 0; };

static std::vector<Match> retrieve_spoke(const Spoke& sp, const std::string& query,
                                         const std::string& section, int max, const std::string& key = "") {
    std::vector<Match> out;
    const std::string ek = key.empty() ? sp.key : key;            // session/tool key overrides the spoke's configured key
    call_replica(sp, [&](const std::string& origin, const std::string& base) -> bool {
        httplib::Client cli(origin);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(15);
        json req{{"query", query}, {"section", section}, {"k", max}};
        if (!ek.empty()) req["key"] = ek;                         // confine retrieval to a content slice of the expert
        auto res = cli.Post(base + "/retrieve", req.dump(), "application/json");
        if (!res || res->status != 200) return false;   // replica down / lacks /retrieve → fail over to the next
        try {
            for (const auto& m : json::parse(res->body).value("matches", json::array()))
                out.push_back({sp.name, m.value("section", ""), m.value("passage", ""), m.value("score", 0.0)});
            return true;
        } catch (...) { return false; }
    });
    return out;
}

static std::vector<Match> retrieve_all(const std::string& query, const std::string& section, int max,
                                       const std::string& key = "") {
    std::vector<std::future<std::vector<Match>>> futs;
    for (const auto& sp : g_spokes)                       // all spokes — a catalog expert returns its document cards
        futs.push_back(std::async(std::launch::async, retrieve_spoke, std::cref(sp),
                                  std::cref(query), std::cref(section), max, key));
    std::vector<Match> all;
    for (auto& f : futs) { auto v = f.get(); all.insert(all.end(), v.begin(), v.end()); }
    std::sort(all.begin(), all.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
    if (max > 0 && (int)all.size() > max) all.resize(max);
    return all;
}

// Ping each spoke's /v1/models; report up/down + latency. Used at startup (visibility) and the /health endpoint.
static json spoke_health() {
    json arr = json::array();
    for (const auto& sp : g_spokes) {
        json reps = json::array();
        int up_n = 0;
        for (const auto& url : sp.urls) {                          // ping EACH replica (redundancy visibility)
            std::string origin, base;
            split_url(url, origin, base);
            httplib::Client cli(origin);
            cli.set_connection_timeout(2);
            cli.set_read_timeout(3);
            auto t0 = std::chrono::steady_clock::now();
            auto res = cli.Get(base + "/v1/models");
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            bool up = res && res->status == 200;
            if (up) up_n++;
            reps.push_back(json{{"url", url}, {"up", up}, {"latency_ms", up ? (int)(ms + 0.5) : -1}});
        }
        json o;
        o["name"] = sp.name; o["domain"] = sp.domain; o["replicas"] = reps;
        o["up"] = up_n > 0;                                        // the spoke is up if ANY replica answers (failover)
        o["replicas_up"] = up_n; o["replicas_total"] = (int)sp.urls.size();
        arr.push_back(o);
    }
    return arr;
}

// ---- hub LLM (llm + tools modes): local llama.cpp server OR remote API; OpenAI- or Anthropic-shaped --------------
// The hub LLM (synthesis + tools routing) can be made REDUNDANT: g_synth["backends"] is an array of endpoint configs
// (each may differ — e.g. a primary API, a fallback provider, a local model). Each inherits the top-level g_synth
// fields and overrides them. With no "backends", g_synth itself is the single backend (back-compat).
static std::vector<json> synth_backends() {
    std::vector<json> v;
    if (g_synth.contains("backends") && g_synth["backends"].is_array() && !g_synth["backends"].empty()) {
        for (const auto& b : g_synth["backends"]) {
            json m = g_synth;
            m.erase("backends");
            m.merge_patch(b);                                 // backend overrides the inherited defaults
            v.push_back(m);
        }
    } else {
        v.push_back(g_synth);
    }
    return v;
}

static std::string call_synth_cfg(const json& cfg, const std::string& prompt) {
    std::string origin, base;
    split_url(cfg.value("url", ""), origin, base);
    if (origin.empty()) return "";
    httplib::Client cli(origin);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(120);
    const char* key = std::getenv(cfg.value("api_key_env", "OPENAI_API_KEY").c_str());
    std::string keystr = key ? key : "";                  // local llama.cpp needs no key; the header is harmless
    if (cfg.value("format", "openai") == "anthropic") {
        httplib::Headers h = {{"x-api-key", keystr}, {"anthropic-version", "2023-06-01"}};
        json req;
        req["model"] = cfg.value("model", "claude-3-5-haiku-latest");
        req["max_tokens"] = cfg.value("max_tokens", 1024);
        req["messages"] = json::array({json{{"role", "user"}, {"content", prompt}}});
        auto res = cli.Post(base + "/v1/messages", h, req.dump(), "application/json");
        if (!res || res->status != 200) return "";
        try { return json::parse(res->body)["content"][0]["text"].get<std::string>(); } catch (...) { return ""; }
    }
    httplib::Headers h = {{"Authorization", std::string("Bearer ") + keystr}};
    json req;
    req["model"] = cfg.value("model", "gpt-4o-mini");
    req["messages"] = json::array({json{{"role", "user"}, {"content", prompt}}});
    auto res = cli.Post(base + "/chat/completions", h, req.dump(), "application/json");  // llama.cpp + OpenAI shape
    if (!res || res->status != 200) return "";
    try { return json::parse(res->body)["choices"][0]["message"]["content"].get<std::string>(); } catch (...) { return ""; }
}

// Try each synth backend (round-robin start + failover) until one returns a non-empty answer.
static std::string call_synth(const std::string& prompt) {
    auto bs = synth_backends();
    unsigned start = g_rr.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < bs.size(); i++) {
        std::string out = call_synth_cfg(bs[(start + i) % bs.size()], prompt);
        if (!out.empty()) return out;
    }
    return "";
}

// ---- federated handle resolution -------------------------------------------------------------------------------
// Route a (spoke, id) to the owning spoke's /lookup → the exact source, verbatim. The hub handle is (spoke, citation_id);
// this is what makes a federated citation *callable*: an agent reads a source's {spoke,id} and refetches it here.
static json lookup_spoke(const Spoke& sp, const std::string& id) {
    json out; out["spoke"] = sp.name; out["id"] = id; out["found"] = false;
    call_replica(sp, [&](const std::string& origin, const std::string& base) -> bool {
        httplib::Client cli(origin);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(15);
        auto res = cli.Get(base + "/lookup", httplib::Params{{"id", id}}, httplib::Headers{});
        if (!res || res->status != 200) return false;             // replica down → fail over to the next
        try {
            json j = json::parse(res->body);
            out["found"] = j.value("found", false);
            if (j.contains("section")) out["section"] = j["section"];
            if (j.contains("passage")) out["passage"] = j["passage"];
            return true;
        } catch (...) { return false; }
    });
    return out;
}

// ---- federated librarian: aggregate every spoke's /catalog ------------------------------------------------------
// Each spoke contributes cards via its /catalog: a leaf sgiandubh its degenerate self-card (or a catalog package's
// document cards); a CHILD CLAYMORE its own federated catalog (recursion). So the catalog rolls up the hierarchy —
// no node is excluded. With replicas/failover, like every other federated call.
static json catalog_spoke(const Spoke& sp, int depth) {
    json cards = json::array();
    call_replica(sp, [&](const std::string& origin, const std::string& base) -> bool {
        httplib::Client cli(origin);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(15);
        auto res = cli.Get(base + "/catalog?depth=" + std::to_string(depth));   // pass the remaining recursion budget
        if (!res || res->status != 200) return false;             // spoke lacks /catalog (older build) → fail over / skip
        try {
            for (auto& c : json::parse(res->body).value("cards", json::array())) {
                json cc = c;
                cc["spoke"] = sp.name;                             // tag provenance: which spoke this card came from
                cards.push_back(cc);
            }
            return true;
        } catch (...) { return false; }
    });
    return cards;
}
// Recursion-BOUNDED roll-up: each hub level decrements `depth`; at 0 we stop descending (a misconfigured cycle
// A→B→A terminates after `depth` hops instead of looping over HTTP). A leaf sgiandubh ignores ?depth. A spoke that's
// down contributes nothing (call_replica → empty) and the rest still aggregate — partial results, never a hard fail.
static json federate_catalog(int depth = 4) {
    json all = json::array();
    if (depth <= 0) return all;
    for (const auto& sp : g_spokes)
        for (auto& c : catalog_spoke(sp, depth - 1)) all.push_back(c);
    return all;
}

// ---- modes -----------------------------------------------------------------------------------------------------
static Result deterministic(const std::vector<SAns>& ranked) {
    Result r;
    r.mode = "deterministic";
    if (ranked.empty()) { r.body = REFUSE; r.mode = "abstain"; return r; }
    const SAns& a = ranked[0];
    r.body = a.answer;
    r.route = a.route; r.margin = a.margin;          // carry the spoke's provenance tier + fragile-link margin
    r.sources.push_back({a.spoke, !a.citation.empty() ? a.citation : a.source, a.citation_id});
    return r;
}

static Result synthesize(const std::string& query, const std::vector<SAns>& ranked) {
    if (ranked.empty()) { Result r; r.body = REFUSE; r.mode = "abstain"; return r; }   // refuse before any LLM call
    Result r;
    r.mode = "llm";
    std::string excerpts;
    for (int k = 0; k < (int)ranked.size() && k < g_top_k; k++) {
        const SAns& a = ranked[k];
        std::string cite = !a.citation.empty() ? a.citation : a.source;
        r.sources.push_back({a.spoke, cite, a.citation_id});
        excerpts += "[" + a.spoke + "] " + a.answer + (cite.empty() ? "" : ("  (source: " + cite + ")")) + "\n\n";
    }
    std::string prompt = "Answer the question using ONLY the expert excerpts below. Carry their sources in your "
        "answer. Add nothing that is not in them. If they do not cover it, say you don't know.\n\nQuestion: " +
        query + "\n\nExpert excerpts:\n" + excerpts;
    std::string out = call_synth(prompt);
    if (out.empty()) return deterministic(ranked);        // backend unreachable → fall back to the cited answer
    r.body = out;
    return r;
}

static Result hub_answer(const std::string& query) {
    auto ranked = fan_out(query);
    return g_mode == "llm" ? synthesize(query, ranked) : deterministic(ranked);
}

// ---- mode:"tools" — agentic routing: the synthesis LLM calls spokes as TOOLS; claymore executes them ------------
// claymore speaks the OpenAI tool-calling protocol on behalf of the hub LLM: it offers each spoke as a function,
// runs the LLM, executes any spoke tool_calls (the real spoke query), feeds the cited result back, and loops to a
// final answer. (sgiandubh, the leaf, has no tools — it answers.)
static json query_param() {
    return json{{"type", "object"},
                {"properties", json{{"query", json{{"type", "string"}, {"description", "the question to ask"}}}}},
                {"required", json::array({"query"})}};
}

static json list_param() {
    return json{{"type", "object"},
                {"properties", json{
                    {"query", json{{"type", "string"}, {"description", "what to list/enumerate (leave empty to list a whole section)"}}},
                    {"section", json{{"type", "string"}, {"description", "optional facet to restrict to"}}},
                    {"max", json{{"type", "integer"}, {"description", "max items to return (default 20)"}}}}},
                {"required", json::array()}};   // query optional: empty query + a section lists that whole section
}
static json lookup_param() {                                         // generic lookup_source: needs the spoke + id
    return json{{"type", "object"},
                {"properties", json{
                    {"spoke", json{{"type", "string"}, {"description", "which expert the source came from"}}},
                    {"id", json{{"type", "string"}, {"description", "the citation id / handle to refetch verbatim"}}}}},
                {"required", json::array({"id"})}};
}
static json id_param() {                                             // per-expert lookup_<spoke>: just the id
    return json{{"type", "object"},
                {"properties", json{{"id", json{{"type", "string"},
                    {"description", "the citation id / handle to refetch verbatim"}}}}},
                {"required", json::array({"id"})}};
}
static json spoke_tools(const std::set<std::string>& scope = {}) {
    json tools = json::array();
    if (!scope.empty()) {                                            // a teaching SESSION: only its in-bounds experts —
        for (const auto& sp : g_spokes) {                           // per-expert tools, no consult/catalog/find outside
            if (!scope.count(sp.name)) continue;                    // the scope (the syllabus boundary, enforced here)
            json fn; fn["name"] = "ask_" + sp.name;
            fn["description"] = "Consult the bounded expert on: " + sp.domain + ". Cited answer, or abstains.";
            fn["parameters"] = query_param();
            tools.push_back(json{{"type", "function"}, {"function", fn}});
            json lf; lf["name"] = "list_" + sp.name;
            lf["description"] = "List matching cited passages from the " + sp.name + " expert (many items, not one).";
            lf["parameters"] = list_param();
            tools.push_back(json{{"type", "function"}, {"function", lf}});
            json kf; kf["name"] = "lookup_" + sp.name;
            kf["description"] = "Refetch the exact cited source from the " + sp.name + " expert by its id.";
            kf["parameters"] = id_param();
            tools.push_back(json{{"type", "function"}, {"function", kf}});
        }
        return tools;
    }
    if (g_tool_style == "per-expert") {                              // one tool per spoke — the LLM picks the expert
        for (const auto& sp : g_spokes) {
            json fn;
            fn["name"] = "ask_" + sp.name;
            fn["description"] = "Consult the bounded expert on: " + sp.domain +
                                ". Returns a cited answer, or indicates it has nothing (abstains).";
            fn["parameters"] = query_param();
            tools.push_back(json{{"type", "function"}, {"function", fn}});
            json lf;
            lf["name"] = "list_" + sp.name;
            lf["description"] = "Retrieve a LIST of matching passages (a cited set) from the " + sp.name +
                                " expert — for 'list/table/all/which X' questions that want MANY items, not one answer.";
            lf["parameters"] = list_param();
            tools.push_back(json{{"type", "function"}, {"function", lf}});
            json kf;
            kf["name"] = "lookup_" + sp.name;
            kf["description"] = "Refetch the EXACT cited source verbatim from the " + sp.name +
                                " expert by its citation id — to verify or quote a source returned earlier.";
            kf["parameters"] = id_param();
            tools.push_back(json{{"type", "function"}, {"function", kf}});
        }
    } else {                                                         // generic — claymore fan-out-routes
        std::string domains;
        for (size_t i = 0; i < g_spokes.size(); i++)
            domains += (i ? ", " : "") + g_spokes[i].name + " (" + g_spokes[i].domain + ")";
        json fn;
        fn["name"] = "consult_experts";
        fn["description"] = "Ask the bounded-expert hub for a single cited answer. It routes to whichever expert "
                            "covers the question, or says nothing is covered. Experts: " + domains + ".";
        fn["parameters"] = query_param();
        tools.push_back(json{{"type", "function"}, {"function", fn}});
        json lf;
        lf["name"] = "list_matching";
        lf["description"] = "Retrieve a LIST of matching passages (a cited SET, not one answer) for "
                            "enumeration/aggregation questions — 'list/table/all/which X'. Use this instead of "
                            "consult_experts when the user wants MANY items. Optional 'section' restricts to a facet.";
        lf["parameters"] = list_param();
        tools.push_back(json{{"type", "function"}, {"function", lf}});
        json kf;
        kf["name"] = "lookup_source";
        kf["description"] = "Refetch the EXACT cited source verbatim by its citation id (and spoke) — to verify or "
                            "quote a source returned earlier. Experts: " + domains + ".";
        kf["parameters"] = lookup_param();
        tools.push_back(json{{"type", "function"}, {"function", kf}});
    }
    // LIBRARIAN tools — the catalog is UNIVERSAL (every spoke exposes /catalog; this hub federates them), so these are
    // always offered. catalog() = the directory of experts/collections; find_document() = search across them.
    if (!g_spokes.empty()) {
        json cf;
        cf["name"] = "catalog";
        cf["description"] = "List the federated catalog — every expert/collection this hub fronts (each card names a "
                            "content expert + what it covers). Use to discover WHAT exists before consulting an expert.";
        cf["parameters"] = list_param();
        tools.push_back(json{{"type", "function"}, {"function", cf}});
        json ff;
        ff["name"] = "find_document";
        ff["description"] = "Search the catalogs for documents/passages on a topic, across all experts. Returns cited "
                            "cards/passages whose handle points INTO a content expert — then ask/lookup that expert.";
        ff["parameters"] = query_param();
        tools.push_back(json{{"type", "function"}, {"function", ff}});
    }
    return tools;
}

// One tool-capable LLM turn (OpenAI shape). Returns the assistant message, or null on failure.
static json llm_turn_cfg(const json& cfg, const json& messages, const json& tools) {
    std::string origin, base;
    split_url(cfg.value("url", ""), origin, base);
    httplib::Client cli(origin);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(120);
    const char* key = std::getenv(cfg.value("api_key_env", "OPENAI_API_KEY").c_str());
    httplib::Headers h = {{"Authorization", std::string("Bearer ") + (key ? key : "")}};
    json req;
    req["model"] = cfg.value("model", "gpt-4o-mini");
    req["messages"] = messages;
    if (!tools.empty()) { req["tools"] = tools; req["tool_choice"] = "auto"; }
    auto res = cli.Post(base + "/chat/completions", h, req.dump(), "application/json");
    if (!res || res->status != 200) {
        std::string why = res ? ("HTTP " + std::to_string(res->status) + " — " + res->body.substr(0, 300))
                              : std::string("no response (is the model at the synthesis url up?)");
        fprintf(stderr, "[claymore] hub LLM call failed: %s\n", why.c_str());  // the usual cause of a spurious refuse
        return json(nullptr);
    }
    try { return json::parse(res->body)["choices"][0]["message"]; }
    catch (...) { fprintf(stderr, "[claymore] hub LLM: unparseable response: %s\n", res->body.substr(0, 300).c_str()); return json(nullptr); }
}

// Redundant tools-mode turn: try each synth backend (round-robin + failover) until one returns a message.
static json llm_turn(const json& messages, const json& tools) {
    auto bs = synth_backends();
    unsigned start = g_rr.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < bs.size(); i++) {
        json m = llm_turn_cfg(bs[(start + i) % bs.size()], messages, tools);
        if (!m.is_null()) return m;
    }
    return json(nullptr);
}

static const char* TOOLS_STEER =
    "You must answer using ONLY the expert tool(s) provided. For every question, call the appropriate tool to get a "
    "grounded, cited answer, then reply from that result and keep its citation. If the tools return nothing relevant, "
    "say the topic isn't covered. Never answer from your own prior knowledge.";

// `system_override` replaces the default steer (a teaching SESSION's assembled pedagogy prompt); `scope` restricts the
// offered tools to the in-bounds content experts; `key` (optional) confines every spoke call to a content SLICE (a
// subset of a large expert — e.g. a tutor on one chapter of a whole book). Empty → the normal open tools loop.
static Result run_tools_loop(const json& client_messages, const std::string& system_override = "",
                             const std::set<std::string>& scope = {}, const std::string& key = "") {
    Result r;
    r.mode = "tools";
    json msgs = client_messages.is_array() ? client_messages : json::array();
    json sys = json{{"role", "system"}, {"content", system_override.empty() ? std::string(TOOLS_STEER) : system_override}};
    msgs.insert(msgs.begin(), sys);
    json tools = spoke_tools(scope);
    const int MAX_ITERS = 6;
    // Execute one tool call (consult_experts fan-out, or a per-expert ask_<spoke>) → the cited result text.
    auto run_tool = [&](const std::string& name, const std::string& argstr) -> std::string {
        json args; try { args = json::parse(argstr); } catch (...) { args = json::object(); }
        std::string q = args.value("query", "");
        std::string toolres;
        // LOOKUP tools → resolve a citation handle to its exact source verbatim (verify/quote a cited result).
        if (name == "lookup_source" || name.rfind("lookup_", 0) == 0) {
            std::string id = args.value("id", "");
            std::string sn = (name == "lookup_source") ? args.value("spoke", "") : name.substr(7);
            for (const auto& s : g_spokes) {
                if (!sn.empty() && s.name != sn) continue;
                json hit = lookup_spoke(s, id);
                if (hit.value("found", false)) {
                    std::string sec = hit.value("section", ""), psg = hit.value("passage", "");
                    r.sources.push_back({s.name, sec, id});
                    if (g_verbose) fprintf(stderr, "[claymore] %s(id=\"%s\") → %s\n", name.c_str(), id.c_str(), s.name.c_str());
                    return sec.empty() ? psg : (sec + ": \"" + psg + "\"");
                }
            }
            return "(no cited source with id \"" + id + "\")";
        }
        // LIST/aggregate tools → the spoke's /retrieve extension, returning a SET of cited passages (not one answer)
        if (name == "list_matching" || name.rfind("list_", 0) == 0) {
            std::string section = args.value("section", "");
            int mx = args.value("max", 20);
            std::vector<Match> ms;
            if (name == "list_matching") {
                ms = retrieve_all(q, section, mx, key);                   // generic: fan /retrieve to all spokes (in slice)
            } else {                                                      // list_<spoke>: one expert
                std::string sn = name.substr(5);
                const Spoke* sp = nullptr;
                for (const auto& s : g_spokes) if (s.name == sn) sp = &s;
                if (sp) ms = retrieve_spoke(*sp, q, section, mx, key);
            }
            if (g_verbose) fprintf(stderr, "[claymore] %s(query=\"%s\", section=\"%s\") → %zu passage(s)\n", name.c_str(), q.c_str(), section.c_str(), ms.size());
            if (ms.empty()) return "(no matching passages)";
            for (const auto& m : ms) {
                toolres += "- " + m.passage + (m.section.empty() ? "" : ("  (source: " + m.section + ")")) + "\n";
                r.sources.push_back({m.spoke, m.section, cite_id(m.section)});
            }
            return toolres;
        }
        // CATALOG tool → the federated directory: every expert/collection this hub (recursively) fronts.
        if (name == "catalog") {
            json cards = federate_catalog();
            if (g_verbose) fprintf(stderr, "[claymore] catalog → %zu card(s)\n", cards.size());
            if (cards.empty()) return "(empty catalog)";
            for (auto& c : cards) {
                std::string label = c.contains("title") ? c.value("title", "") : c.value("summary", "");
                std::string h = c.value("handle", "");
                toolres += "- [" + c.value("spoke", "") + "] " + label + (h.empty() ? "" : ("  (handle: " + h + ")")) + "\n";
            }
            return toolres;
        }
        // FIND_DOCUMENT → search content + catalog spokes for matching passages/cards (handles point INTO experts).
        if (name == "find_document") {
            std::string section = args.value("section", "");
            int mx = args.value("max", 8);
            auto ms = retrieve_all(q, section, mx);
            if (g_verbose) fprintf(stderr, "[claymore] find_document(query=\"%s\") → %zu hit(s)\n", q.c_str(), ms.size());
            if (ms.empty()) return "(nothing in the catalogs matches)";
            for (const auto& m : ms) {
                toolres += "- " + m.passage +
                           (m.section.empty() ? "" : ("  (handle: " + cite_id(m.section) + ", spoke: " + m.spoke + ")")) + "\n";
                r.sources.push_back({m.spoke, m.section, cite_id(m.section)});
            }
            return toolres;
        }
        if (name == "consult_experts") {                                  // generic: claymore fan-out-routes
            auto ranked = fan_out(q, key);
            if (g_verbose) fprintf(stderr, "[claymore] consult_experts(query=\"%s\") → %zu non-abstaining spoke(s)\n", q.c_str(), ranked.size());
            if (ranked.empty()) toolres = "(no expert covers that — all abstained)";
            for (int k = 0; k < (int)ranked.size() && k < g_top_k; k++) {
                const SAns& a = ranked[k];
                std::string cite = !a.citation.empty() ? a.citation : a.source;
                toolres += "[" + a.spoke + "] " + a.answer + (cite.empty() ? "" : ("  (source: " + cite + ")")) + "\n";
                r.sources.push_back({a.spoke, cite, a.citation_id});
            }
        } else {                                                          // per-expert: ask_<spoke>
            std::string sn = (name.rfind("ask_", 0) == 0) ? name.substr(4) : name;
            const Spoke* sp = nullptr;
            for (const auto& s : g_spokes) if (s.name == sn) sp = &s;
            if (sp) {
                SAns a = ask_spoke(*sp, q, key);
                if (g_verbose) fprintf(stderr, "[claymore] %s(query=\"%s\") → %s\n", name.c_str(), q.c_str(), a.ok ? "answered" : "abstained");
                if (a.ok) {
                    std::string cite = !a.citation.empty() ? a.citation : a.source;
                    toolres = a.answer + (cite.empty() ? "" : ("  (source: " + cite + ")"));
                    r.sources.push_back({sp->name, cite, a.citation_id});
                } else {
                    toolres = "(the " + sn + " expert has nothing on that — abstained)";
                }
            } else {
                toolres = "(unknown tool: " + name + ")";
            }
        }
        return toolres;
    };
    // Some local models emit the tool call as plain JSON text in `content` rather than the structured tool_calls field
    // (llama.cpp doesn't always parse it, even with --jinja). Pull {name, arguments} out of the text so we run it anyway.
    auto as_text_toolcall = [](const std::string& s, std::string& name, std::string& argstr) -> bool {
        auto a = s.find('{'), b = s.rfind('}');                           // tolerate <tool_call> tags / surrounding prose
        if (a == std::string::npos || b == std::string::npos || b <= a) return false;
        try {
            json j = json::parse(s.substr(a, b - a + 1));
            if (!j.is_object() || !j.contains("name")) return false;
            name = j.value("name", "");
            const json& args = j.contains("arguments") ? j["arguments"] : (j.contains("parameters") ? j["parameters"] : json::object());
            argstr = args.is_string() ? args.get<std::string>() : args.dump();
            return !name.empty();
        } catch (...) { return false; }
    };
    for (int it = 0; it < MAX_ITERS; it++) {
        json m = llm_turn(msgs, tools);
        if (m.is_null()) { r.body = REFUSE; r.mode = "abstain"; return r; }   // backend unreachable
        if (m.contains("tool_calls") && m["tool_calls"].is_array() && !m["tool_calls"].empty()) {
            msgs.push_back(m);                                                // the assistant turn (with tool_calls)
            for (const auto& tc : m["tool_calls"]) {
                std::string name = tc.value("function", json::object()).value("name", "");
                std::string argstr = tc.value("function", json::object()).value("arguments", "{}");
                if (g_verbose) fprintf(stderr, "[claymore] iter %d: tool_call %s\n", it, name.c_str());
                json tm;
                tm["role"] = "tool";
                tm["tool_call_id"] = tc.value("id", "");
                tm["content"] = run_tool(name, argstr);
                msgs.push_back(tm);
            }
            continue;                                                        // loop: let the LLM use the results
        }
        std::string content = m.value("content", "");
        std::string tname, targs;
        if (as_text_toolcall(content, tname, targs)) {                       // tool call emitted as TEXT — run it anyway
            if (g_verbose) fprintf(stderr, "[claymore] iter %d: model emitted a tool call as text (%s); executing it\n", it, tname.c_str());
            std::string toolres = run_tool(tname, targs);
            msgs.push_back(m);                                                // the assistant text turn
            msgs.push_back(json{{"role", "user"},
                                {"content", "Expert tool results:\n" + toolres +
                                            "\nNow answer my question using ONLY these results, and keep the citation. Do not call the tool again."}});
            continue;
        }
        r.body = content;                                                    // final answer
        if (r.body.empty()) {
            if (g_verbose) fprintf(stderr, "[claymore] iter %d: model returned no tool call and empty content → refuse\n", it);
            r.body = REFUSE;
        } else if (g_verbose) {
            fprintf(stderr, "[claymore] iter %d: model produced final answer (%zu chars)\n", it, r.body.size());
        }
        // A refusal must not cite the sources it consulted-but-didn't-use: tag it abstain so the Sources block is dropped.
        if (r.body == REFUSE || is_abstain(r.body, "")) { r.mode = "abstain"; r.sources.clear(); }
        return r;
    }
    if (g_verbose) fprintf(stderr, "[claymore] hit %d-iteration cap without a final answer → refuse\n", MAX_ITERS);
    r.body = REFUSE;
    r.mode = "abstain";
    return r;                                                                // hit the iteration cap
}

// ---- rendering: text (default) or structured json (response_format) --------------------------------------------
// Tidy a spoke citation for display: "norm:id · Section" → "Section (id)"; otherwise return as-is.
static std::string pretty_cite(const std::string& c) {
    auto dot = c.find("\xC2\xB7");                                       // UTF-8 middle dot
    if (dot == std::string::npos) return c;
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    std::string id = trim(c.substr(0, dot)), sec = trim(c.substr(dot + 2));
    if (id.rfind("norm:", 0) == 0) id = id.substr(5);
    if (sec.empty()) return id;
    return id.empty() ? sec : (sec + " (" + id + ")");
}

static std::string render_text(const Result& r) {
    if (r.mode == "abstain" || r.sources.empty()) return r.body;
    if (r.mode == "deterministic") {
        const auto& s = r.sources[0];
        std::string out = r.body + (s.citation.empty() ? ("\n\n[" + s.spoke + "]")
                                          : ("\n\n\xF0\x9F\x93\x96 " + pretty_cite(s.citation) + "  \xC2\xB7  [" + s.spoke + "]"));
        if (!r.route.empty() || r.margin < 1e8) {     // carry the spoke's provenance tier (recall vs forge-tax)
            char mb[64];
            if (!r.route.empty() && r.margin < 1e8) snprintf(mb, sizeof mb, "%s · margin %+.2f", r.route.c_str(), r.margin);
            else if (!r.route.empty()) snprintf(mb, sizeof mb, "%s", r.route.c_str());
            else snprintf(mb, sizeof mb, "margin %+.2f", r.margin);
            out += "\n[provenance: " + std::string(mb) + "]";
        }
        return out;
    }
    // dedupe + pretty-print + collapse the noise (e.g. [logic] repeated 20x with no citation). Prefer cited entries;
    // if none carry a citation, list the distinct spokes once.
    std::vector<std::pair<std::string, std::string>> uniq;
    for (const auto& s : r.sources) {
        std::pair<std::string, std::string> p{s.spoke, pretty_cite(s.citation)};
        if (std::find(uniq.begin(), uniq.end(), p) == uniq.end()) uniq.push_back(p);
    }
    std::string src = "\n\nSources:";
    bool cited = false;
    for (const auto& s : uniq)
        if (!s.second.empty()) { src += " [" + s.first + "] " + s.second + ";"; cited = true; }
    if (!cited) {
        std::set<std::string> sp;
        for (const auto& s : uniq) sp.insert(s.first);
        for (const auto& n : sp) src += " [" + n + "];";
    }
    return r.body + src;
}

static std::string render_json(const Result& r) {
    json j;
    j["answer"] = r.body;
    j["mode"] = r.mode;
    // sgiandubh-compatible FLAT fields, so a PARENT claymore (or any client) parses this hub exactly like a leaf
    // spoke — this is what makes claymores nest (federated claymores). kind=abstain → the parent treats it as an abstain.
    j["kind"] = (r.mode == "abstain") ? "abstain" : "federated";
    if (!r.sources.empty()) {
        if (!r.sources[0].citation.empty()) j["citation"] = r.sources[0].citation;
        if (!r.sources[0].id.empty()) j["citation_id"] = r.sources[0].id;
    }
    json srcs = json::array();
    for (auto& s : r.sources) {                                       // each source carries its callable handle (spoke,id)
        json o; o["spoke"] = s.spoke;
        if (!s.citation.empty()) o["citation"] = s.citation;
        if (!s.id.empty()) o["id"] = s.id;                            // GET /lookup?spoke=<spoke>&id=<id> refetches it
        srcs.push_back(o);
    }
    j["sources"] = srcs;
    if (!r.route.empty()) j["route"] = r.route;       // provenance tier (RETRIEVED|SELECTED|COMPOSED)
    if (r.margin < 1e8) j["margin"] = r.margin;       // thinnest-decision margin (fragile-link signal)
    if (!r.suggestions.empty()) j["suggestions"] = r.suggestions;   // tutor: suggested next prompts for the learner
    return j.dump();
}

static bool wants_json(const json& body) {
    if (body.contains("response_format") && body["response_format"].is_object()) {
        std::string t = body["response_format"].value("type", "");
        if (t == "json_object" || t == "json_schema" || t == "json") return true;
    }
    return body.value("format", "") == "json";
}

static std::string user_text(const json& body) {
    if (body.contains("prompt") && body["prompt"].is_string()) return body["prompt"].get<std::string>();
    std::string user;
    if (body.contains("messages"))
        for (const auto& m : body["messages"]) {
            if (m.value("role", "") != "user") continue;
            const auto& c = m["content"];
            if (c.is_string()) user = c.get<std::string>();
            else if (c.is_array()) { user.clear(); for (auto& b : c) if (b.value("type", "") == "text") user += b.value("text", ""); }
        }
    return user;
}

// ---- OpenAI/Anthropic response envelopes (mirror sgiandubh) ----------------------------------------------------
static json chat_chunk(const json& delta, const char* finish) {
    json ch; ch["index"] = 0; ch["delta"] = delta; ch["finish_reason"] = finish ? json(finish) : json(nullptr);
    json c; c["id"] = "chatcmpl-claymore"; c["object"] = "chat.completion.chunk"; c["created"] = (long)std::time(nullptr);
    c["model"] = "claymore"; c["choices"] = json::array({ch});
    return c;
}
static json chat_completion(const std::string& content) {
    json choice; choice["index"] = 0; choice["message"] = json{{"role", "assistant"}, {"content", content}};
    choice["finish_reason"] = "stop"; choice["logprobs"] = nullptr;
    json r; r["id"] = "chatcmpl-claymore"; r["object"] = "chat.completion"; r["created"] = (long)std::time(nullptr);
    r["model"] = "claymore"; r["choices"] = json::array({choice});
    r["usage"] = json{{"prompt_tokens", 0}, {"completion_tokens", 0}, {"total_tokens", 0}};
    return r;
}
static json text_completion(const std::string& content) {
    json choice; choice["index"] = 0; choice["text"] = content; choice["finish_reason"] = "stop"; choice["logprobs"] = nullptr;
    json r; r["id"] = "cmpl-claymore"; r["object"] = "text_completion"; r["created"] = (long)std::time(nullptr);
    r["model"] = "claymore"; r["choices"] = json::array({choice});
    r["usage"] = json{{"prompt_tokens", 0}, {"completion_tokens", 0}, {"total_tokens", 0}};
    return r;
}
static json anthropic_message(const std::string& content) {
    json block; block["type"] = "text"; block["text"] = content;
    json r; r["id"] = "msg-claymore"; r["type"] = "message"; r["role"] = "assistant"; r["model"] = "claymore";
    r["content"] = json::array({block}); r["stop_reason"] = "end_turn"; r["stop_sequence"] = nullptr;
    r["usage"] = json{{"input_tokens", 0}, {"output_tokens", 0}};
    return r;
}

static void stream_answer(httplib::Response& res, std::string route, std::string content) {
    res.set_chunked_content_provider("text/event-stream",
        [route, content](size_t, httplib::DataSink& sink) {
            auto raw = [&](const std::string& s) { sink.write(s.data(), s.size()); };
            if (route == "anthropic") {
                json msg; msg["id"] = "msg-claymore"; msg["type"] = "message"; msg["role"] = "assistant";
                msg["model"] = "claymore"; msg["content"] = json::array(); msg["stop_reason"] = nullptr;
                msg["usage"] = json{{"input_tokens", 0}, {"output_tokens", 0}};
                json ms; ms["type"] = "message_start"; ms["message"] = msg;
                raw("event: message_start\ndata: " + ms.dump() + "\n\n");
                json cbs; cbs["type"] = "content_block_start"; cbs["index"] = 0;
                cbs["content_block"] = json{{"type", "text"}, {"text", ""}};
                raw("event: content_block_start\ndata: " + cbs.dump() + "\n\n");
            } else if (route == "chat") {
                raw("data: " + chat_chunk(json{{"role", "assistant"}}, nullptr).dump() + "\n\n");
            }
            size_t i = 0;
            while (i < content.size()) {
                size_t sp = content.find(' ', i);
                std::string piece = (sp == std::string::npos) ? content.substr(i) : content.substr(i, sp - i + 1);
                if (route == "anthropic") {
                    json d; d["type"] = "content_block_delta"; d["index"] = 0;
                    d["delta"] = json{{"type", "text_delta"}, {"text", piece}};
                    raw("event: content_block_delta\ndata: " + d.dump() + "\n\n");
                } else if (route == "text") {
                    json ch; ch["text"] = piece; ch["index"] = 0; ch["finish_reason"] = nullptr;
                    json c; c["id"] = "cmpl-claymore"; c["object"] = "text_completion"; c["created"] = (long)std::time(nullptr);
                    c["model"] = "claymore"; c["choices"] = json::array({ch});
                    raw("data: " + c.dump() + "\n\n");
                } else {
                    raw("data: " + chat_chunk(json{{"content", piece}}, nullptr).dump() + "\n\n");
                }
                i = (sp == std::string::npos) ? content.size() : sp + 1;
            }
            if (route == "anthropic") {
                raw("event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n");
                json md; md["type"] = "message_delta"; md["delta"] = json{{"stop_reason", "end_turn"}};
                md["usage"] = json{{"output_tokens", 0}};
                raw("event: message_delta\ndata: " + md.dump() + "\n\n");
                raw("event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n");
            } else if (route == "chat") {
                raw("data: " + chat_chunk(json::object(), "stop").dump() + "\n\n");
                raw("data: [DONE]\n\n");
            } else {
                json ch; ch["text"] = ""; ch["index"] = 0; ch["finish_reason"] = "stop";
                json c; c["id"] = "cmpl-claymore"; c["object"] = "text_completion"; c["created"] = (long)std::time(nullptr);
                c["model"] = "claymore"; c["choices"] = json::array({ch});
                raw("data: " + c.dump() + "\n\n");
                raw("data: [DONE]\n\n");
            }
            sink.done();
            return true;
        });
}

// ---- teaching sessions: a pedagogy template + a bounded scope start an LLM-led session -------------------------
// Query the pedagogy expert(s) for a template by a selector ("socratic intro tutor"). intent:"pedagogy" routes the
// spoke's strategy to the matching template; returns its system-prompt scaffold ("" if none).
static std::string fetch_template(const std::string& selector) {
    for (const auto& sp : g_spokes) {
        if (sp.role != "pedagogy") continue;
        std::string out;
        call_replica(sp, [&](const std::string& origin, const std::string& base) -> bool {
            httplib::Client cli(origin);
            cli.set_connection_timeout(5);
            cli.set_read_timeout(20);
            json req;
            req["intent"] = "pedagogy";
            req["messages"] = json::array({json{{"role", "user"}, {"content", selector}}});
            req["response_format"] = json{{"type", "json_object"}};
            auto res = cli.Post(base + "/v1/chat/completions", req.dump(), "application/json");
            if (!res || res->status != 200) return false;
            try {
                std::string c = json::parse(res->body)["choices"][0]["message"]["content"].get<std::string>();
                json a; try { a = json::parse(c); } catch (...) { a = json{{"answer", c}}; }
                std::string ans = a.value("answer", "");
                if (!ans.empty() && ans.rfind("That isn", 0) != 0) out = ans;   // a real template (not an abstain)
                return true;                                                    // responded → don't fail over
            } catch (...) { return false; }
        });
        if (!out.empty()) return out;
    }
    return "";
}

static std::string fill_vars(std::string s, const json& vars) {
    for (auto it = vars.begin(); it != vars.end(); ++it) {
        std::string needle = "{" + it.key() + "}";
        std::string val = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
        for (size_t p; (p = s.find(needle)) != std::string::npos;) s.replace(p, needle.size(), val);
    }
    return s;
}

struct Session { bool ok = false; std::string system, opening, suggest, note, key; std::set<std::string> scope; };

// Assemble a teaching session from the request's "session" object: resolve the template (inline `template_text`, else
// fetched from the pedagogy expert by `template`), bind the scope (explicit `scope`, else all content experts), fill
// {scope}/{objectives}/{content}/vars, and append the HARD guardrail. The tutor then gets tools ONLY for the scope.
static Session assemble_session(const json& sess) {
    Session s;
    // requested scope: explicit `scope`, else all content experts (no pedagogy/librarian meta-roles).
    std::set<std::string> requested;
    if (sess.contains("scope") && sess["scope"].is_array())
        for (const auto& x : sess["scope"]) requested.insert(x.get<std::string>());
    else
        for (const auto& sp : g_spokes)
            if (sp.role != "pedagogy" && sp.role != "librarian") requested.insert(sp.name);

    std::string tmpl = sess.value("template_text", "");           // caller-inline template wins; else the library
    if (tmpl.empty()) tmpl = fetch_template(sess.value("template", "tutor"));
    if (tmpl.empty())                                            // no pedagogy expert / no match → graceful built-in
        tmpl = "You are a bounded tutor for {scope}. Teach toward {objectives} using ONLY the expert tools; ask guiding "
               "questions one at a time and cite each fact. {content}";

    // Split off the tutor-metadata tail (opener + suggestions + scope restriction) the adapter packs after the sentinel.
    std::string opening_tmpl = sess.value("opening_text", "");    // caller-inline opener wins
    std::string suggest_tmpl = sess.value("suggest_text", "");
    std::vector<std::string> applies; std::string subject;
    size_t mp = tmpl.find(TUTOR_META);
    if (mp != std::string::npos) {
        std::string tail = trim(tmpl.substr(mp + TUTOR_META.size()));
        tmpl = trim(tmpl.substr(0, mp));
        try {
            json meta = json::parse(tail);
            if (opening_tmpl.empty()) opening_tmpl = meta.value("opening", "");
            if (suggest_tmpl.empty()) suggest_tmpl = meta.value("suggest", "");
            subject = meta.value("subject", "");
            if (meta.contains("applies_to") && meta["applies_to"].is_array())
                for (const auto& x : meta["applies_to"]) applies.push_back(x.get<std::string>());
        } catch (...) {}                                          // a malformed tail just means "no restriction/opener"
    }

    // SCOPE RESTRICTION: if the tutor declares which experts it applies to, intersect the requested scope with them —
    // explicit `applies_to` names ∪ a lexical subject↔(name+domain) match. An empty restriction applies to all.
    if (!applies.empty() || !subject.empty()) {
        std::set<std::string> allowed;
        for (const auto& sp : g_spokes) {
            if (!sp.role.empty()) continue;                       // content experts only
            bool ok = false;
            for (const auto& a : applies) if (sp.name == a) { ok = true; break; }
            if (!ok && !subject.empty() && relevance(subject, sp.name + " " + sp.domain) >= TUTOR_SUBJECT_MIN) ok = true;
            if (ok) allowed.insert(sp.name);
        }
        std::set<std::string> dropped;
        for (auto it = requested.begin(); it != requested.end();)
            if (allowed.count(*it)) ++it; else { dropped.insert(*it); it = requested.erase(it); }
        if (!dropped.empty()) {
            std::string d; for (const auto& n : dropped) d += (d.empty() ? "" : ", ") + n;
            s.note = "tutor scope restricted — dropped out-of-subject expert(s): " + d;
        }
        if (requested.empty()) {                                 // restricted to nothing in scope → don't start
            std::string allow; for (const auto& n : allowed) allow += (allow.empty() ? "" : ", ") + n;
            s.note = "this tutor applies only to {" + (allow.empty() ? std::string("<no matching expert>") : allow) +
                     "} — none in scope; not starting";
            s.ok = false;
            return s;
        }
    }
    s.scope = requested;

    std::string scope_desc;
    for (const auto& sp : g_spokes)
        if (s.scope.count(sp.name)) scope_desc += (scope_desc.empty() ? "" : ", ") + sp.name + " (" + sp.domain + ")";
    if (scope_desc.empty()) scope_desc = "the available experts";

    json vars = sess.value("variables", json::object());
    vars["scope"] = scope_desc;
    if (!vars.contains("objectives")) vars["objectives"] = sess.value("objectives", "the topic");
    vars["content"] = sess.value("content", "");
    s.system = fill_vars(tmpl, vars) +
               "\n\nGround every factual claim in the expert tools provided; you have tools ONLY for the in-scope "
               "experts (" + scope_desc + "). If the student goes outside that scope, say so and redirect. Keep citations.";
    // The opener (if any) is the template's own scaffold, filled with the same vars — claymore adds NO wording of its
    // own (the authored pedagogy lives in the template, not the binary). It's run as a kickoff turn so the tutor speaks
    // first; the system prompt's grounding guardrail (the in-code hard promise) still governs it.
    if (!opening_tmpl.empty()) s.opening = fill_vars(opening_tmpl, vars);
    if (!suggest_tmpl.empty()) s.suggest = fill_vars(suggest_tmpl, vars);
    s.key = sess.value("key", "");                                // confine the whole session to a content slice (subset key)
    s.ok = true;
    return s;
}

// Suggested next prompts for the learner, from the session's `suggest` scaffold (authored in the template). One plain
// no-tools completion reflecting on the conversation; claymore adds only the machine FORMAT contract (a JSON array of
// strings), never teaching wording. Returns [] with no scaffold or if the model didn't produce a usable list.
static std::vector<std::string> gen_suggestions(const json& history, const Session& ses) {
    std::vector<std::string> out;
    if (ses.suggest.empty() || g_synth.value("url", "").empty()) return out;
    json msgs = json::array();
    msgs.push_back(json{{"role", "system"}, {"content", ses.system}});
    for (const auto& m : history) msgs.push_back(m);
    msgs.push_back(json{{"role", "user"}, {"content",
        ses.suggest + " Reply with ONLY a JSON array of short plain-text strings (the prompts), nothing else."}});
    json m = llm_turn(msgs, json::array());                       // empty tools → a plain completion, no spoke calls
    if (m.is_null()) return out;
    std::string c = m.value("content", "");
    size_t a = c.find('['), b = c.rfind(']');
    if (a != std::string::npos && b != std::string::npos && b > a) {
        try {
            json arr = json::parse(c.substr(a, b - a + 1));
            if (arr.is_array())
                for (const auto& x : arr)
                    if (x.is_string() && !x.get<std::string>().empty()) out.push_back(x.get<std::string>());
        } catch (...) {}
    }
    if (out.empty() && !c.empty()) {                              // fallback: split bullets / numbered lines
        std::stringstream ss(c); std::string ln;
        while (std::getline(ss, ln)) {
            size_t p = ln.find_first_not_of(" \t-*•0123456789.)");
            if (p != std::string::npos) out.push_back(ln.substr(p));
        }
    }
    if (out.size() > 5) out.resize(5);                            // a sane cap regardless of the template
    return out;
}

// Print a suggestions block to the REPL (dim header on a TTY). No-op when empty.
static void print_suggestions(const std::vector<std::string>& sugs) {
    if (sugs.empty()) return;
    std::string blk;
    for (size_t k = 0; k < sugs.size(); ++k) blk += "  " + std::to_string(k + 1) + ". " + sugs[k] + "\n";
    printf("%s%s", g_tty ? "\x1b[2mSuggestions (type #1, #2, … to pick):\x1b[22m\n"
                         : "Suggestions (type #1, #2, … to pick):\n", blk.c_str());
    fflush(stdout);
}

static void handle(const httplib::Request& req, httplib::Response& res, const std::string& route) {
    json body;
    try { body = json::parse(req.body); }
    catch (...) { res.status = 400; res.set_content("{\"error\":\"invalid json\"}", "application/json"); return; }
    Result r;
    if (body.contains("session") && body["session"].is_object() && !g_synth.value("url", "").empty()) {
        Session ses = assemble_session(body["session"]);          // a teaching session: pedagogy prompt + scoped tools
        if (ses.ok) {
            json msgs = body.value("messages", json::array());
            bool has_user = false;
            for (const auto& m : msgs) if (m.value("role", "") == "user") { has_user = true; break; }
            // Kickoff: with an opener and no learner turn yet, the tutor speaks FIRST (set session.kickoff:false to opt out).
            bool kicked = !ses.opening.empty() && !has_user && body["session"].value("kickoff", true);
            if (kicked)
                r = run_tools_loop(json::array({json{{"role", "user"}, {"content", ses.opening}}}), ses.system, ses.scope, ses.key);
            else
                r = run_tools_loop(msgs, ses.system, ses.scope, ses.key);
            if (!ses.suggest.empty() && wants_json(body)) {       // structured consumers get suggested next prompts
                json hist = kicked ? json::array() : msgs;
                hist.push_back(json{{"role", "assistant"}, {"content", r.body}});
                r.suggestions = gen_suggestions(hist, ses);
            }
        } else { r.body = ses.note.empty() ? REFUSE : ses.note; r.mode = "abstain"; }   // no template / restricted out of scope
    } else if (g_mode == "tools" && !g_synth.value("url", "").empty()) {
        r = run_tools_loop(body.value("messages", json::array()));  // agentic: LLM calls spokes as tools
    } else {
        r = hub_answer(user_text(body));                          // deterministic | llm fan-out synthesis
    }
    std::string content = wants_json(body) ? render_json(r) : render_text(r);
    if (body.value("stream", false)) { stream_answer(res, route, content); return; }
    if (route == "anthropic") res.set_content(anthropic_message(content).dump(), "application/json");
    else if (route == "text") res.set_content(text_completion(content).dump(), "application/json");
    else res.set_content(chat_completion(content).dump(), "application/json");
}

int main(int argc, char** argv) {
    std::vector<std::string> pos;
    bool repl = false, plain = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--repl") repl = true;
        else if (a == "--plain") plain = true;                    // REPL: disable the fixed-input/footer TUI (plain getline)
        else if (a == "--verbose" || a == "-v") g_verbose = true;
        else if (a == "--min-relevance" && i + 1 < argc) g_min_relevance = std::atof(argv[++i]);
        else pos.push_back(a);
    }
    std::string cfg_path = pos.size() > 0 ? pos[0] : "spokes.json";
    int port = pos.size() > 1 ? std::stoi(pos[1]) : 9000;
    std::ifstream f(cfg_path);
    if (!f) { fprintf(stderr, "claymore: cannot open %s\n", cfg_path.c_str()); return 1; }
    json cfg; f >> cfg;
    for (auto& s : cfg["spokes"]) {                               // "url" (single) and/or "urls"/"replicas" (redundant copies)
        Spoke sp;
        sp.name = s.value("name", "");
        sp.domain = s.value("domain", "");
        sp.role = s.value("role", "");                            // "" content expert | "librarian" catalog
        sp.key = s.value("key", "");                              // optional subset key → a slice of a larger expert
        for (const char* key : {"urls", "replicas"})
            if (s.contains(key) && s[key].is_array())
                for (auto& u : s[key]) sp.urls.push_back(u.get<std::string>());
        if (s.contains("url") && s["url"].is_string()) sp.urls.push_back(s["url"].get<std::string>());
        std::vector<std::string> uniq;                           // dedupe (a replica listed twice, or url ∈ urls) —
        std::set<std::string> seen;                              // order-preserving, so round-robin isn't skewed
        for (auto& u : sp.urls) if (!u.empty() && seen.insert(u).second) uniq.push_back(u);
        sp.urls = std::move(uniq);
        if (!sp.urls.empty()) g_spokes.push_back(std::move(sp));
    }
    g_mode = cfg.value("mode", "deterministic");
    g_top_k = cfg.value("top_k", 3);
    g_tool_style = cfg.value("tool_style", "generic");
    g_synth = cfg.value("synthesis", json::object());
    std::string extra;
    if (g_mode == "llm" || g_mode == "tools")
        extra = " · synth=" + g_synth.value("format", "openai") + "@" + g_synth.value("url", "?");
    if (g_mode == "tools") extra += " · tools=" + g_tool_style;
    fprintf(stderr, "claymore: %zu spokes · mode=%s%s\n", g_spokes.size(), g_mode.c_str(), extra.c_str());

    // health check at startup — so a down spoke is obvious, not a silent "nothing covers that".
    json health = spoke_health();
    int nup = 0;
    for (const auto& h : health) {
        bool up = h.value("up", false);
        if (up) nup++;
        fprintf(stderr, "  spoke %-12s %-7s %d/%d replicas up  %s\n", h.value("name", "").c_str(),
                up ? "UP" : "DOWN", h.value("replicas_up", 0), h.value("replicas_total", 0), h.value("domain", "").c_str());
    }
    if (nup == 0)
        fprintf(stderr, "  WARNING: no spokes reachable — start the sgiandubh spoke servers first.\n");

    if (repl) {  // CLI: content Q&A by default; enter a MENTORING SESSION with /session (no server)
        bool have_llm = !g_synth.value("url", "").empty();
        g_tty = isatty(fileno(stdout));                           // render markdown/TeX only for an interactive terminal
        repltui::Repl ui;                                         // fixed input line + status footer (TTY only; --plain opts out)
        if (!plain) ui.start("CLAYMORE_REPL_PLAIN");
        fprintf(stderr, "claymore REPL — content Q&A by default.\n"
                        "  /catalog                      list the federated catalog (the librarian — what exists)\n"
                        "  /tutors                       list teaching templates (the pedagogy expert)\n"
                        "  /experts                      list content experts (what you can study)\n"
                        "  /session <tutor> [on <e>…] [key <subset>]   mentoring session, optionally confined to a content\n"
                        "                                slice (e.g. /session socratic-tutor on book key Chapter 3)\n"
                        "  #1 / #2 / …                   in a session, send that numbered suggestion as your next turn\n"
                        "  /end                          leave the session (back to Q&A)    /help    blank line = quit\n");
        Session session;                                          // current mentoring session (ok=false → not in one)
        std::string session_name;
        json history = json::array();                             // client-held conversation (the hub is stateless)
        std::vector<std::string> last_suggestions;                // the tutor's most-recent suggestions (pick with "#N")
        std::string line;
        while (true) {
            std::string foot;                                     // status footer: session state, else hub state
            if (session.ok) {
                foot = "[" + session_name + "]";
                std::string sc; for (const auto& n : session.scope) sc += (sc.empty() ? "" : ",") + n;
                if (!sc.empty()) foot += " on " + sc;
                if (!session.key.empty()) foot += " · key=" + session.key;
                foot += " · #N pick · /end · blank=quit";
            } else {
                foot = "claymore · mode=" + g_mode + " · " + std::to_string(g_spokes.size()) +
                       " spoke(s) · /help · blank=quit";
            }
            ui.set_footer(foot);
            std::string prompt = session.ok ? ("[" + session_name + "]> ") : "> ";
            if (ui.readline(prompt, line) == repltui::Repl::EOF_QUIT) break;
            if (line.empty()) break;                              // blank line → quit the REPL

            if (line[0] == '/') {                                 // ---- a REPL command ----
                std::istringstream ss(line.substr(1));
                std::string cmd; ss >> cmd;
                if (cmd == "help") {
                    fprintf(stderr, "  /catalog  /tutors  /experts  /session <tutor> [on <expert>…]  /end  (blank = quit)\n");
                } else if (cmd == "catalog") {            // the librarian, demonstrable without an LLM (like /tutors)
                    json cards = federate_catalog();
                    if (cards.empty()) fprintf(stderr, "  (empty catalog — no experts reachable)\n");
                    for (const auto& c : cards) {
                        std::string h = c.value("handle", c.value("title", c.value("id", std::string("?"))));
                        std::string s = c.value("summary", c.value("domain", std::string()));
                        if (s.size() > 90) s = s.substr(0, 90);
                        fprintf(stderr, "  %-26s %s\n", h.c_str(), s.c_str());
                    }
                } else if (cmd == "experts") {
                    for (const auto& sp : g_spokes)
                        if (sp.role.empty())             // content experts only (no librarian/pedagogy meta-roles)
                            fprintf(stderr, "  %-14s %s\n", sp.name.c_str(), sp.domain.c_str());
                } else if (cmd == "tutors") {
                    bool any = false; std::set<std::string> seen;
                    for (const auto& sp : g_spokes) {              // aggregate across ALL pedagogy providers (federation)
                        if (sp.role != "pedagogy") continue;
                        any = true;
                        for (const auto& m : retrieve_spoke(sp, "", "", 0))   // k=0 → every template (no 20-cap)
                            if (seen.insert(m.section).second)               // dedupe templates offered by >1 provider
                                fprintf(stderr, "  %s\n", m.section.c_str());
                    }
                    if (!any) fprintf(stderr, "  (no pedagogy expert configured — a role:\"pedagogy\" spoke)\n");
                } else if (cmd == "session") {
                    std::string tok, tmpl, subkey; std::vector<std::string> scope; int phase = 0;  // 0=tutor 1=experts 2=key
                    while (ss >> tok) {                           // "<tutor words…> [on <expert>…] [key <subset words…>]"
                        if (tok == "on") { phase = 1; continue; }
                        if (tok == "key") { phase = 2; continue; }
                        if (phase == 0) tmpl += (tmpl.empty() ? "" : " ") + tok;
                        else if (phase == 1) scope.push_back(tok);
                        else subkey += (subkey.empty() ? "" : " ") + tok;
                    }
                    if (!have_llm) { fprintf(stderr, "  sessions need a synthesis LLM (set \"synthesis\" in the config)\n"); continue; }
                    auto low = [](std::string x) { for (auto& c : x) if (c >= 'A' && c <= 'Z') c += 32; return x; };
                    // validate the `on <expert>` scope — an unknown name binds NO tools (a tutor with nothing to teach)
                    std::vector<std::string> bad;
                    for (const auto& n : scope) {
                        bool known = false;
                        for (const auto& sp : g_spokes) if (sp.name == n && sp.role.empty()) { known = true; break; }
                        if (!known) bad.push_back(n);
                    }
                    if (!bad.empty()) {
                        std::string u; for (const auto& n : bad) u += (u.empty() ? "" : ", ") + n;
                        fprintf(stderr, "  warning: not content experts (bind no tools): %s — see /experts\n", u.c_str());
                    }
                    if (!scope.empty() && bad.size() == scope.size()) {
                        fprintf(stderr, "  no valid experts in scope — the tutor would have no tools; not starting\n"); continue; }
                    // validate the tutor name across ALL pedagogy providers — warn (don't abort) if it'll fall back
                    if (!tmpl.empty()) {
                        bool hit = false, anyped = false;
                        for (const auto& sp : g_spokes) {
                            if (sp.role != "pedagogy") continue;
                            anyped = true;
                            for (const auto& m : retrieve_spoke(sp, "", "", 0))
                                if (low(m.section).find(low(tmpl)) != std::string::npos) { hit = true; break; }
                            if (hit) break;
                        }
                        if (anyped && !hit) fprintf(stderr, "  note: no tutor matches '%s' — using the built-in generic tutor (see /tutors)\n", tmpl.c_str());
                    }
                    json sess; sess["template"] = tmpl.empty() ? "tutor" : tmpl;
                    if (!scope.empty()) sess["scope"] = scope;
                    if (!subkey.empty()) sess["key"] = subkey;    // confine the tutor to a subset (a slice of a big expert)
                    Session s = assemble_session(sess);
                    if (!s.ok) {
                        fprintf(stderr, "  couldn't start a session — %s\n",
                                s.note.empty() ? "no template / pedagogy expert" : s.note.c_str());
                        continue;
                    }
                    if (!s.note.empty()) fprintf(stderr, "  %s\n", s.note.c_str());
                    session = s; session_name = tmpl.empty() ? "tutor" : tmpl; history = json::array(); last_suggestions.clear();
                    std::string sc; for (const auto& n : session.scope) sc += (sc.empty() ? "" : ", ") + n;
                    if (session.key.empty())
                        fprintf(stderr, "  entered '%s' on [%s] — start talking; /end to leave\n", session_name.c_str(), sc.c_str());
                    else
                        fprintf(stderr, "  entered '%s' on [%s] · subset key '%s' — start talking; /end to leave\n",
                                session_name.c_str(), sc.c_str(), session.key.c_str());
                    if (!session.opening.empty()) {               // tutor speaks first: run the template's opener as a kickoff turn
                        json seed = json::array({json{{"role", "user"}, {"content", session.opening}}});
                        Result o = run_tools_loop(seed, session.system, session.scope, session.key);
                        history.push_back(seed[0]);
                        history.push_back(json{{"role", "assistant"}, {"content", o.body}});
                        printf("%s\n", g_tty ? mdterm::render(render_text(o), true).c_str() : render_text(o).c_str());
                        fflush(stdout);
                        last_suggestions = gen_suggestions(history, session);
                        print_suggestions(last_suggestions);
                    }
                } else if (cmd == "end") {
                    session = Session{}; session_name.clear(); history = json::array(); last_suggestions.clear();
                    fprintf(stderr, "  left the session\n");
                } else {
                    fprintf(stderr, "  unknown command (/help)\n");
                }
                continue;
            }

            // In a session, "#N" picks suggestion N from the tutor's last list and sends it as this turn. A leading '#'
            // followed by anything non-numeric (e.g. "#define …") falls through and is treated as an ordinary query.
            if (session.ok && !line.empty() && line[0] == '#') {
                std::string num = trim(line.substr(1));
                if (!num.empty() && num.find_first_not_of("0123456789") == std::string::npos) {
                    int n = std::stoi(num);
                    if (n >= 1 && n <= (int)last_suggestions.size()) {
                        line = last_suggestions[n - 1];
                        fprintf(stderr, "  [#%d] %s\n", n, line.c_str());
                    } else {
                        fprintf(stderr, "  no suggestion #%d — there %s %zu (re-ask or type your own)\n",
                                n, last_suggestions.size() == 1 ? "is" : "are", last_suggestions.size());
                        continue;
                    }
                }
            }

            Result r;
            if (session.ok) {                                     // ---- a turn IN the mentoring session ----
                history.push_back(json{{"role", "user"}, {"content", line}});
                r = run_tools_loop(history, session.system, session.scope, session.key);
                history.push_back(json{{"role", "assistant"}, {"content", r.body}});
            } else {                                              // ---- default content Q&A ----
                r = (g_mode == "tools" && have_llm)
                        ? run_tools_loop(json::array({json{{"role", "user"}, {"content", line}}}))
                        : hub_answer(line);
            }
            printf("%s\n", g_tty ? mdterm::render(render_text(r), true).c_str() : render_text(r).c_str());
            fflush(stdout);
            if (session.ok) { last_suggestions = gen_suggestions(history, session); print_suggestions(last_suggestions); }
        }
        ui.stop();
        return 0;
    }

    fprintf(stderr, "claymore: listening :%d\n", port);
    httplib::Server svr;
    svr.Get("/v1/models", [](const httplib::Request&, httplib::Response& res) {
        json e; e["id"] = "claymore"; e["object"] = "model"; e["owned_by"] = "claymore";
        json m; m["object"] = "list"; m["data"] = json::array({e});
        res.set_content(m.dump(), "application/json");
    });
    // domain manifest — so an outer agent (or claymore-as-a-tool) can discover what this hub covers.
    svr.Get("/v1/domains", [](const httplib::Request&, httplib::Response& res) {
        json data = json::array();
        for (const auto& sp : g_spokes) data.push_back(json{{"name", sp.name}, {"domain", sp.domain}});
        json m; m["object"] = "list"; m["data"] = data;
        res.set_content(m.dump(), "application/json");
    });
    // live spoke health — up/down + latency per spoke; status=degraded if any (or all) are down.
    auto health_handler = [](const httplib::Request&, httplib::Response& res) {
        json spokes = spoke_health();
        int up = 0;
        for (const auto& s : spokes) if (s.value("up", false)) up++;
        json m;
        m["object"] = "health";
        m["status"] = (up == (int)spokes.size()) ? "ok" : (up > 0 ? "degraded" : "down");
        m["mode"] = g_mode;
        m["spokes_up"] = up;
        m["spokes_total"] = (int)spokes.size();
        m["spokes"] = spokes;
        res.status = (up > 0) ? 200 : 503;
        res.set_content(m.dump(), "application/json");
    };
    svr.Get("/health", health_handler);
    svr.Get("/healthz", health_handler);
    // Federated citation-as-handle: refetch the exact source a federated citation points to. GET /lookup?spoke=&id=
    // (spoke optional → tries all). Routes to the owning spoke's /lookup; bounded — unknown spoke/id → not found.
    auto lookup_handler = [](const httplib::Request& q, httplib::Response& res) {
        std::string id = q.has_param("id") ? q.get_param_value("id") : "";
        std::string spoke = q.has_param("spoke") ? q.get_param_value("spoke") : "";
        if (id.empty() && !q.body.empty()) {
            try { json b = json::parse(q.body); id = b.value("id", ""); spoke = b.value("spoke", spoke); } catch (...) {}
        }
        json out;
        bool found = false;
        for (const auto& sp : g_spokes) {
            if (!spoke.empty() && sp.name != spoke) continue;
            out = lookup_spoke(sp, id);
            if (out.value("found", false)) { found = true; break; }
        }
        if (!found && out.is_null()) { out = json::object(); out["id"] = id; out["found"] = false; }
        res.status = found ? 200 : 404;
        res.set_content(out.dump(), "application/json");
    };
    svr.Get("/lookup", lookup_handler);
    svr.Post("/lookup", lookup_handler);
    // Federated /retrieve — the sgiandubh extension, fanned across this hub's content spokes. So a PARENT claymore can
    // list-retrieve from this hub exactly as from a leaf (claymores nest: same API surface, replicas for HA).
    svr.Post("/retrieve", [](const httplib::Request& q, httplib::Response& res) {
        json b;
        try { b = json::parse(q.body); } catch (...) { b = json::object(); }
        auto ms = retrieve_all(b.value("query", ""), b.value("section", ""), b.value("k", 20));
        json matches = json::array();
        for (const auto& m : ms)
            matches.push_back(json{{"section", m.section}, {"passage", m.passage}, {"score", m.score}});
        json out; out["object"] = "retrieve"; out["matches"] = matches;
        res.set_content(out.dump(), "application/json");
    });
    // Federated /catalog — aggregate every spoke's /catalog (a child claymore contributes its own federated catalog →
    // the catalog rolls up the hierarchy). The universal librarian; degenerate self-cards from leaf sgiandubhs included.
    svr.Get("/catalog", [](const httplib::Request& q, httplib::Response& res) {
        int depth = q.has_param("depth") ? std::atoi(q.get_param_value("depth").c_str()) : 4;   // recursion budget
        json out; out["object"] = "catalog"; out["cards"] = federate_catalog(depth);
        res.set_content(out.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
    });
    // Session FACTORY: assemble a teaching session (template + scope + variables → system prompt + tool scope) WITHOUT
    // running it — for a UI / client to inspect, then drive the chat by passing the same "session" object per turn.
    svr.Post("/session", [](const httplib::Request& q, httplib::Response& res) {
        json b;
        try { b = json::parse(q.body); } catch (...) { b = json::object(); }
        json sess = b.contains("session") ? b["session"] : b;     // accept under "session" or at top level
        Session s = assemble_session(sess);
        json out; out["object"] = "session"; out["ok"] = s.ok;
        if (s.ok) {
            out["system"] = s.system;
            out["scope"] = json(std::vector<std::string>(s.scope.begin(), s.scope.end()));
            out["model"] = g_synth.value("model", "");
        } else {
            out["error"] = "no template resolved (need a pedagogy spoke, or pass session.template_text)";
        }
        res.set_content(out.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
    });
    svr.Post("/v1/chat/completions", [](const httplib::Request& q, httplib::Response& r) { handle(q, r, "chat"); });
    svr.Post("/v1/completions", [](const httplib::Request& q, httplib::Response& r) { handle(q, r, "text"); });
    svr.Post("/v1/messages", [](const httplib::Request& q, httplib::Response& r) { handle(q, r, "anthropic"); });
    svr.set_keep_alive_max_count(1000);
    svr.set_keep_alive_timeout(30);
    svr.set_tcp_nodelay(true);
    svr.listen("0.0.0.0", port);
    return 0;
}
