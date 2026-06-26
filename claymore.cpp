// claymore — a hub over sgiandubh spokes (C++; cpp-httplib + nlohmann/json, one small binary).
//
// Fans a query out to N bounded-expert spokes (each an OpenAI-compatible sgiandubh server), drops the ones that
// ABSTAIN (off-domain — the bound IS the router), ranks the survivors by confidence, and answers in one of two modes:
//
//   deterministic  — return the top cited answer verbatim. Keeps every spoke guarantee (bounded, cited,
//                    injection-immune); no LLM in the loop.
//   llm            — synthesize across the surviving cited answers with a hub LLM (OpenAI-compatible endpoint).
//                    Flexible/conversational, but reintroduces the LLM's prompt-injection/hallucination surface.
//
// The hard promise is enforced IN CODE, not a prompt: if every spoke abstains, claymore refuses — which survives a
// jailbroken hub LLM. claymore is itself OpenAI-compatible, so clients see one endpoint.
#include "httplib.h"
#include "json.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <future>
#include <string>
#include <vector>
using json = nlohmann::json;

struct Spoke { std::string name, url, domain; };
struct SAns {
    std::string answer, kind, citation, source, spoke;
    double confidence = -1;
    bool ok = false;
};

static std::vector<Spoke> g_spokes;
static std::string g_mode = "deterministic";
static int g_top_k = 3;
static json g_synth = json::object();   // {url, model, api_key_env}
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

static bool is_abstain(const std::string& a, const std::string& kind) {
    return kind == "abstain" || a.empty() ||
           a.find("isn't covered") != std::string::npos || a.find("Try rephrasing") != std::string::npos;
}

// Query one spoke for structured output; ok=false if it abstained / errored (treated as "not this expert").
static SAns ask_spoke(const Spoke& sp, const std::string& query) {
    SAns r;
    std::string origin, base;
    split_url(sp.url, origin, base);
    httplib::Client cli(origin);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(15);
    json req;
    req["messages"] = json::array({json{{"role", "user"}, {"content", query}}});
    req["response_format"] = json{{"type", "json_object"}};
    auto res = cli.Post(base + "/v1/chat/completions", req.dump(), "application/json");
    if (!res || res->status != 200) return r;
    try {
        std::string content = json::parse(res->body)["choices"][0]["message"]["content"].get<std::string>();
        json a;
        try { a = json::parse(content); }                       // structured {answer,kind,citation?,source?,confidence?}
        catch (...) { a = json{{"answer", content}, {"kind", "distilled"}}; }  // spoke didn't honor json → raw text
        std::string ans = a.value("answer", ""), kind = a.value("kind", "distilled");
        if (is_abstain(ans, kind)) return r;
        r.answer = ans; r.kind = kind; r.spoke = sp.name;
        r.citation = a.value("citation", ""); r.source = a.value("source", "");
        if (a.contains("confidence") && a["confidence"].is_number()) r.confidence = a["confidence"].get<double>();
        r.ok = true;
    } catch (...) { return r; }
    return r;
}

static double score(const SAns& a) {
    if (a.confidence >= 0) return a.confidence;
    if (a.kind == "retrieved") return 0.9;          // authoritative verbatim source
    if (a.kind == "distilled") return 0.6;
    if (a.kind == "generated") return 0.3;
    return 0.5;
}

static std::vector<SAns> fan_out(const std::string& query) {
    std::vector<std::future<SAns>> futs;
    for (const auto& sp : g_spokes)
        futs.push_back(std::async(std::launch::async, ask_spoke, std::cref(sp), std::cref(query)));
    std::vector<SAns> res;
    for (auto& f : futs) { SAns a = f.get(); if (a.ok) res.push_back(a); }
    std::sort(res.begin(), res.end(), [](const SAns& x, const SAns& y) { return score(x) > score(y); });
    return res;
}

static std::string deterministic(const std::vector<SAns>& ranked) {
    if (ranked.empty()) return REFUSE;
    const SAns& a = ranked[0];
    std::string cite = !a.citation.empty() ? a.citation : a.source;
    return a.answer + (cite.empty() ? ("\n\n[" + a.spoke + "]")
                                    : ("\n\n\xF0\x9F\x93\x96 " + cite + "  \xC2\xB7  [" + a.spoke + "]"));
}

static std::string synthesize(const std::string& query, const std::vector<SAns>& ranked) {
    if (ranked.empty()) return REFUSE;                          // refuse IN CODE — never call the LLM with nothing
    std::string excerpts;
    for (int k = 0; k < (int)ranked.size() && k < g_top_k; k++) {
        const SAns& a = ranked[k];
        std::string cite = !a.citation.empty() ? a.citation : a.source;
        excerpts += "[" + a.spoke + "] " + a.answer + (cite.empty() ? "" : ("  (source: " + cite + ")")) + "\n\n";
    }
    std::string prompt = "Answer the question using ONLY the expert excerpts below. Carry their sources in your "
        "answer. Add nothing that is not in them. If they do not cover it, say you don't know.\n\nQuestion: " +
        query + "\n\nExpert excerpts:\n" + excerpts;
    std::string origin, base;
    split_url(g_synth.value("url", ""), origin, base);
    httplib::Client cli(origin);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(60);
    const char* key = std::getenv(g_synth.value("api_key_env", "OPENAI_API_KEY").c_str());
    httplib::Headers h = {{"Authorization", std::string("Bearer ") + (key ? key : "")}};
    json req;
    req["model"] = g_synth.value("model", "gpt-4o-mini");
    req["messages"] = json::array({json{{"role", "user"}, {"content", prompt}}});
    auto res = cli.Post(base + "/chat/completions", h, req.dump(), "application/json");
    if (!res || res->status != 200) return deterministic(ranked);   // hub LLM failed → fall back to deterministic
    try { return json::parse(res->body)["choices"][0]["message"]["content"].get<std::string>(); }
    catch (...) { return deterministic(ranked); }
}

static std::string answer(const std::string& query) {
    auto ranked = fan_out(query);
    return g_mode == "llm" ? synthesize(query, ranked) : deterministic(ranked);
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

static json chat_completion(const std::string& content) {
    json choice;
    choice["index"] = 0;
    choice["message"] = json{{"role", "assistant"}, {"content", content}};
    choice["finish_reason"] = "stop";
    json r;
    r["id"] = "chatcmpl-claymore"; r["object"] = "chat.completion"; r["created"] = (long)std::time(nullptr);
    r["model"] = "claymore"; r["choices"] = json::array({choice});
    r["usage"] = json{{"prompt_tokens", 0}, {"completion_tokens", 0}, {"total_tokens", 0}};
    return r;
}

int main(int argc, char** argv) {
    std::string cfg_path = argc > 1 ? argv[1] : "spokes.json";
    int port = argc > 2 ? std::stoi(argv[2]) : 9000;
    std::ifstream f(cfg_path);
    if (!f) { fprintf(stderr, "claymore: cannot open %s\n", cfg_path.c_str()); return 1; }
    json cfg; f >> cfg;
    for (auto& s : cfg["spokes"]) g_spokes.push_back({s.value("name", ""), s.value("url", ""), s.value("domain", "")});
    g_mode = cfg.value("mode", "deterministic");
    g_top_k = cfg.value("top_k", 3);
    g_synth = cfg.value("synthesis", json::object());
    fprintf(stderr, "claymore: %zu spokes · mode=%s · listening :%d\n", g_spokes.size(), g_mode.c_str(), port);

    httplib::Server svr;
    svr.Get("/v1/models", [](const httplib::Request&, httplib::Response& res) {
        json e; e["id"] = "claymore"; e["object"] = "model"; e["owned_by"] = "claymore";
        json m; m["object"] = "list"; m["data"] = json::array({e});
        res.set_content(m.dump(), "application/json");
    });
    auto handle = [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) { res.status = 400; res.set_content("{\"error\":\"invalid json\"}", "application/json"); return; }
        res.set_content(chat_completion(answer(user_text(body))).dump(), "application/json");
    };
    svr.Post("/v1/chat/completions", handle);
    svr.Post("/v1/completions", handle);
    svr.set_keep_alive_max_count(1000);
    svr.set_keep_alive_timeout(30);
    svr.set_tcp_nodelay(true);
    svr.listen("0.0.0.0", port);
    return 0;
}
