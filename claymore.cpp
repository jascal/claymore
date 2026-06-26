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
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <vector>
using json = nlohmann::json;

struct Spoke { std::string name, url, domain; };
struct SAns {
    std::string answer, kind, citation, source, spoke;
    double confidence = -1;
    bool ok = false;
};
struct Result {                                   // the hub's answer + provenance
    std::string body, mode;                       // body = answer text (no inline tag); mode = deterministic|llm|abstain
    std::vector<std::pair<std::string, std::string>> sources;  // (spoke, citation)
};

static std::vector<Spoke> g_spokes;
static std::string g_mode = "deterministic";
static int g_top_k = 3;
static std::string g_tool_style = "generic";      // tools mode: "generic" (one consult_experts tool) | "per-expert"
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

static bool is_abstain(const std::string& a, const std::string& kind) {
    return kind == "abstain" || a.empty() ||
           a.find("isn't covered") != std::string::npos || a.find("Try rephrasing") != std::string::npos;
}

// ---- spokes ----------------------------------------------------------------------------------------------------
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
        try { a = json::parse(content); }
        catch (...) { a = json{{"answer", content}, {"kind", "distilled"}}; }
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
    if (a.kind == "retrieved") return 0.9;
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

// Ping each spoke's /v1/models; report up/down + latency. Used at startup (visibility) and the /health endpoint.
static json spoke_health() {
    json arr = json::array();
    for (const auto& sp : g_spokes) {
        std::string origin, base;
        split_url(sp.url, origin, base);
        httplib::Client cli(origin);
        cli.set_connection_timeout(2);
        cli.set_read_timeout(3);
        auto t0 = std::chrono::steady_clock::now();
        auto res = cli.Get(base + "/v1/models");
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        bool up = res && res->status == 200;
        json o;
        o["name"] = sp.name; o["url"] = sp.url; o["domain"] = sp.domain;
        o["up"] = up; o["latency_ms"] = up ? (int)(ms + 0.5) : -1;
        arr.push_back(o);
    }
    return arr;
}

// ---- hub LLM (llm mode): local llama.cpp server OR remote API; OpenAI- or Anthropic-shaped ---------------------
static std::string call_synth(const std::string& prompt) {
    std::string origin, base;
    split_url(g_synth.value("url", ""), origin, base);
    if (origin.empty()) return "";
    httplib::Client cli(origin);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(120);
    const char* key = std::getenv(g_synth.value("api_key_env", "OPENAI_API_KEY").c_str());
    std::string keystr = key ? key : "";                  // local llama.cpp needs no key; the header is harmless
    if (g_synth.value("format", "openai") == "anthropic") {
        httplib::Headers h = {{"x-api-key", keystr}, {"anthropic-version", "2023-06-01"}};
        json req;
        req["model"] = g_synth.value("model", "claude-3-5-haiku-latest");
        req["max_tokens"] = g_synth.value("max_tokens", 1024);
        req["messages"] = json::array({json{{"role", "user"}, {"content", prompt}}});
        auto res = cli.Post(base + "/v1/messages", h, req.dump(), "application/json");
        if (!res || res->status != 200) return "";
        try { return json::parse(res->body)["content"][0]["text"].get<std::string>(); } catch (...) { return ""; }
    }
    httplib::Headers h = {{"Authorization", std::string("Bearer ") + keystr}};
    json req;
    req["model"] = g_synth.value("model", "gpt-4o-mini");
    req["messages"] = json::array({json{{"role", "user"}, {"content", prompt}}});
    auto res = cli.Post(base + "/chat/completions", h, req.dump(), "application/json");  // llama.cpp + OpenAI shape
    if (!res || res->status != 200) return "";
    try { return json::parse(res->body)["choices"][0]["message"]["content"].get<std::string>(); } catch (...) { return ""; }
}

// ---- modes -----------------------------------------------------------------------------------------------------
static Result deterministic(const std::vector<SAns>& ranked) {
    Result r;
    r.mode = "deterministic";
    if (ranked.empty()) { r.body = REFUSE; r.mode = "abstain"; return r; }
    const SAns& a = ranked[0];
    r.body = a.answer;
    r.sources.push_back({a.spoke, !a.citation.empty() ? a.citation : a.source});
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
        r.sources.push_back({a.spoke, cite});
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
static json spoke_tools() {
    json tools = json::array();
    if (g_tool_style == "per-expert") {                              // one tool per spoke — the LLM picks the expert
        for (const auto& sp : g_spokes) {
            json fn;
            fn["name"] = "ask_" + sp.name;
            fn["description"] = "Consult the bounded expert on: " + sp.domain +
                                ". Returns a cited answer, or indicates it has nothing (abstains).";
            fn["parameters"] = query_param();
            tools.push_back(json{{"type", "function"}, {"function", fn}});
        }
    } else {                                                         // generic — one tool; claymore fan-out-routes
        std::string domains;
        for (size_t i = 0; i < g_spokes.size(); i++)
            domains += (i ? ", " : "") + g_spokes[i].name + " (" + g_spokes[i].domain + ")";
        json fn;
        fn["name"] = "consult_experts";
        fn["description"] = "Ask the bounded-expert hub. It routes to whichever expert covers the question and "
                            "returns a cited answer, or says nothing is covered. Experts: " + domains + ".";
        fn["parameters"] = query_param();
        tools.push_back(json{{"type", "function"}, {"function", fn}});
    }
    return tools;
}

// One tool-capable LLM turn (OpenAI shape). Returns the assistant message, or null on failure.
static json llm_turn(const json& messages, const json& tools) {
    std::string origin, base;
    split_url(g_synth.value("url", ""), origin, base);
    httplib::Client cli(origin);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(120);
    const char* key = std::getenv(g_synth.value("api_key_env", "OPENAI_API_KEY").c_str());
    httplib::Headers h = {{"Authorization", std::string("Bearer ") + (key ? key : "")}};
    json req;
    req["model"] = g_synth.value("model", "gpt-4o-mini");
    req["messages"] = messages;
    if (!tools.empty()) { req["tools"] = tools; req["tool_choice"] = "auto"; }
    auto res = cli.Post(base + "/chat/completions", h, req.dump(), "application/json");
    if (!res || res->status != 200) return json(nullptr);
    try { return json::parse(res->body)["choices"][0]["message"]; } catch (...) { return json(nullptr); }
}

static Result run_tools_loop(const json& client_messages) {
    Result r;
    r.mode = "tools";
    json msgs = client_messages.is_array() ? client_messages : json::array();
    // Steer the hub LLM to actually consult the bounded experts (don't answer from its own knowledge) — so answers
    // stay grounded + cited. Prepended as the first system message; the client's messages follow.
    json sys = json{{"role", "system"},
                    {"content", "You must answer using ONLY the expert tool(s) provided. For every question, call "
                                "the appropriate tool to get a grounded, cited answer, then reply from that result "
                                "and keep its citation. If the tools return nothing relevant, say the topic isn't "
                                "covered. Never answer from your own prior knowledge."}};
    msgs.insert(msgs.begin(), sys);
    json tools = spoke_tools();
    const int MAX_ITERS = 6;
    for (int it = 0; it < MAX_ITERS; it++) {
        json m = llm_turn(msgs, tools);
        if (m.is_null()) { r.body = REFUSE; r.mode = "abstain"; return r; }   // backend unreachable
        if (m.contains("tool_calls") && m["tool_calls"].is_array() && !m["tool_calls"].empty()) {
            msgs.push_back(m);                                                // the assistant turn (with tool_calls)
            for (const auto& tc : m["tool_calls"]) {
                std::string name = tc.value("function", json::object()).value("name", "");
                std::string argstr = tc.value("function", json::object()).value("arguments", "{}");
                std::string q;
                try { q = json::parse(argstr).value("query", ""); } catch (...) {}
                std::string toolres;
                if (name == "consult_experts") {                              // generic: claymore fan-out-routes
                    auto ranked = fan_out(q);
                    if (ranked.empty()) toolres = "(no expert covers that — all abstained)";
                    for (int k = 0; k < (int)ranked.size() && k < g_top_k; k++) {
                        const SAns& a = ranked[k];
                        std::string cite = !a.citation.empty() ? a.citation : a.source;
                        toolres += "[" + a.spoke + "] " + a.answer + (cite.empty() ? "" : ("  (source: " + cite + ")")) + "\n";
                        r.sources.push_back({a.spoke, cite});
                    }
                } else {                                                       // per-expert: ask_<spoke>
                    std::string sn = (name.rfind("ask_", 0) == 0) ? name.substr(4) : name;
                    const Spoke* sp = nullptr;
                    for (const auto& s : g_spokes) if (s.name == sn) sp = &s;
                    if (sp) {
                        SAns a = ask_spoke(*sp, q);
                        if (a.ok) {
                            std::string cite = !a.citation.empty() ? a.citation : a.source;
                            toolres = a.answer + (cite.empty() ? "" : ("  (source: " + cite + ")"));
                            r.sources.push_back({sp->name, cite});
                        } else {
                            toolres = "(the " + sn + " expert has nothing on that — abstained)";
                        }
                    } else {
                        toolres = "(unknown tool: " + name + ")";
                    }
                }
                json tm;
                tm["role"] = "tool";
                tm["tool_call_id"] = tc.value("id", "");
                tm["content"] = toolres;
                msgs.push_back(tm);
            }
            continue;                                                        // loop: let the LLM use the results
        }
        r.body = m.value("content", "");                                     // final answer
        if (r.body.empty()) r.body = REFUSE;
        return r;
    }
    r.body = REFUSE;
    r.mode = "abstain";
    return r;                                                                // hit the iteration cap
}

// ---- rendering: text (default) or structured json (response_format) --------------------------------------------
static std::string render_text(const Result& r) {
    if (r.mode == "abstain" || r.sources.empty()) return r.body;
    if (r.mode == "deterministic") {
        const auto& s = r.sources[0];
        return r.body + (s.second.empty() ? ("\n\n[" + s.first + "]")
                                          : ("\n\n\xF0\x9F\x93\x96 " + s.second + "  \xC2\xB7  [" + s.first + "]"));
    }
    std::string src = "\n\nSources:";
    for (auto& s : r.sources) src += " [" + s.first + "]" + (s.second.empty() ? "" : (" " + s.second)) + ";";
    return r.body + src;
}

static std::string render_json(const Result& r) {
    json j;
    j["answer"] = r.body;
    j["mode"] = r.mode;
    json srcs = json::array();
    for (auto& s : r.sources) { json o; o["spoke"] = s.first; if (!s.second.empty()) o["citation"] = s.second; srcs.push_back(o); }
    j["sources"] = srcs;
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

static void handle(const httplib::Request& req, httplib::Response& res, const std::string& route) {
    json body;
    try { body = json::parse(req.body); }
    catch (...) { res.status = 400; res.set_content("{\"error\":\"invalid json\"}", "application/json"); return; }
    Result r = (g_mode == "tools" && !g_synth.value("url", "").empty())
                   ? run_tools_loop(body.value("messages", json::array()))     // agentic: LLM calls spokes as tools
                   : hub_answer(user_text(body));                              // deterministic | llm fan-out synthesis
    std::string content = wants_json(body) ? render_json(r) : render_text(r);
    if (body.value("stream", false)) { stream_answer(res, route, content); return; }
    if (route == "anthropic") res.set_content(anthropic_message(content).dump(), "application/json");
    else if (route == "text") res.set_content(text_completion(content).dump(), "application/json");
    else res.set_content(chat_completion(content).dump(), "application/json");
}

int main(int argc, char** argv) {
    std::vector<std::string> pos;
    bool repl = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--repl") repl = true;
        else pos.push_back(a);
    }
    std::string cfg_path = pos.size() > 0 ? pos[0] : "spokes.json";
    int port = pos.size() > 1 ? std::stoi(pos[1]) : 9000;
    std::ifstream f(cfg_path);
    if (!f) { fprintf(stderr, "claymore: cannot open %s\n", cfg_path.c_str()); return 1; }
    json cfg; f >> cfg;
    for (auto& s : cfg["spokes"]) g_spokes.push_back({s.value("name", ""), s.value("url", ""), s.value("domain", "")});
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
        fprintf(stderr, "  spoke %-10s %-30s %s\n", h.value("name", "").c_str(), h.value("url", "").c_str(),
                up ? "UP" : "DOWN (unreachable)");
    }
    if (nup == 0)
        fprintf(stderr, "  WARNING: no spokes reachable — start the sgiandubh spoke servers first.\n");

    if (repl) {  // CLI: read queries from stdin, print answers (no server) — for manual testing
        fprintf(stderr, "claymore REPL — type a query; blank line or Ctrl-D to exit.\n");
        std::string line;
        while (true) {
            fprintf(stderr, "\n> ");
            fflush(stderr);
            if (!std::getline(std::cin, line) || line.empty()) break;
            Result r = (g_mode == "tools" && !g_synth.value("url", "").empty())
                           ? run_tools_loop(json::array({json{{"role", "user"}, {"content", line}}}))
                           : hub_answer(line);
            printf("%s\n", render_text(r).c_str());
            fflush(stdout);
        }
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
    svr.Post("/v1/chat/completions", [](const httplib::Request& q, httplib::Response& r) { handle(q, r, "chat"); });
    svr.Post("/v1/completions", [](const httplib::Request& q, httplib::Response& r) { handle(q, r, "text"); });
    svr.Post("/v1/messages", [](const httplib::Request& q, httplib::Response& r) { handle(q, r, "anthropic"); });
    svr.set_keep_alive_max_count(1000);
    svr.set_keep_alive_timeout(30);
    svr.set_tcp_nodelay(true);
    svr.listen("0.0.0.0", port);
    return 0;
}
