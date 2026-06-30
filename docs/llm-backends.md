# LLM backends for the claymore hub

How to point claymore's hub LLM at a separate endpoint (a hosted API, a remote
self-hosted model, a gateway) — and the one question that always comes up:
**"if the LLM is somewhere else, how do the expert tools get registered?"**

TL;DR — **you almost never need an LLM proxy.** claymore is the agent; the LLM is
just a text/tool-call generator it calls over HTTP. Switching endpoints is a
`synthesis` config change, and the tools travel *in the request body*, so they
work the same whether the model is local, remote, or behind a proxy.

---

## The mental model (read this first)

claymore is **not** plugged into the LLM as a plugin; it's the other way around.
claymore owns the loop and the tools; the LLM is a stateless callee:

```
                       per turn, claymore sends an ordinary OpenAI request:
                       { model, messages, tools:[…], tool_choice:"auto" }
  ┌──────────┐   HTTP   ┌─────────────┐
  │ claymore │ ───────▶ │  your LLM   │   the LLM only DECIDES which tool to call
  │ (agent)  │ ◀─────── │  endpoint   │   and returns tool_calls (or a final answer)
  └────┬─────┘  tool_calls└─────────────┘
       │ claymore EXECUTES the tool calls against the spokes,
       ▼ feeds the cited results back as role:"tool" messages, loops (≤6 iters)
  sgiandubh spokes (bounded, cited)
```

So:

- **The tools are "registered" in-band, every turn**, as the `tools` array in the
  chat-completions request (`llm_turn_cfg` in `claymore.cpp`). There is nothing to
  install or register *on the LLM side*.
- **The LLM never reaches your spokes.** It only emits tool-call *intent*; claymore
  runs the actual `/v1` calls. A jailbroken or hallucinating model still can't
  fabricate a citation — the answer text is grounded in what claymore fetched.
- **A proxy, if you use one, is transparent to all of this.** It forwards the
  request body (tools included) verbatim. "How do the tools get registered through
  a proxy?" → *exactly as without one.*

> ⚠️ **Guarantee boundary.** In `mode:"deterministic"` there is no LLM and the
> answer is a spoke's cited passage verbatim. The moment you add a hub LLM
> (`llm` or `tools`), you reintroduce that model's prompt-injection / hallucination
> surface **at the hub** — claymore keeps the citations honest, but the prose is the
> model's. Using a *remote* model also means your queries (and the spokes' returned
> passages) leave your machine. Both are worth a deliberate decision, not a default.

---

## Which mode needs what

| `mode` | Hub LLM? | Needs tool-calling? | What the LLM does |
|--------|----------|---------------------|-------------------|
| `deterministic` | no | — | nothing; returns the top cited answer verbatim |
| `llm` | yes | **no** | claymore fans out to the spokes itself; the LLM only *synthesizes* the already-cited answers into prose (one call) |
| `tools` | yes | **yes** | the LLM drives — it calls the expert tools; claymore executes them and loops to a final answer |

Key consequence for backend choice:

- **`mode:"tools"` requires an OpenAI-shaped endpoint that supports function/tool
  calling.** claymore's tools loop only speaks the OpenAI shape (`/chat/completions`
  + `tools`). It is tolerant of models that emit the tool call as JSON *text* in
  `content` (it parses and runs it anyway), but the endpoint must be OpenAI-compatible.
- **`mode:"llm"` works with almost anything** — OpenAI *or* Anthropic shape, with or
  without tool support — because claymore does the routing and the LLM only writes prose.
  This is the robust fallback for endpoints that can't do tools.

---

## Config: the `synthesis` block

Everything lives under `synthesis` in your `spokes.json` (there is no CLI flag).
Fields (`call_synth_cfg` / `llm_turn_cfg`):

| field | meaning | default |
|-------|---------|---------|
| `url` | endpoint **base** (see URL rules below) | — |
| `model` | model name the endpoint expects | `gpt-4o-mini` |
| `format` | `"openai"` or `"anthropic"` | `openai` |
| `api_key_env` | **NAME of the env var** holding the key (not the key itself) | `OPENAI_API_KEY` |
| `max_tokens` | response cap (used by the anthropic shape; good practice everywhere) | `1024` |
| `backends` | array of endpoint configs for failover/round-robin (see below) | — |

**Keys are read from the named environment variable**, never stored in the file:
claymore does `getenv(api_key_env)` and sends `Authorization: Bearer <key>` (openai)
or `x-api-key: <key>` (anthropic). A local model that needs no key: point
`api_key_env` at an unset var (e.g. `"NONE"`); the empty header is harmless.

**URL rules (a common footgun):**

- **openai** → claymore POSTs `url + "/chat/completions"`, so `url` **should end in
  `/v1`** (e.g. `https://api.openai.com/v1`).
- **anthropic** → claymore POSTs `url + "/v1/messages"`, so `url` should be the
  **bare host** (e.g. `https://api.anthropic.com`, no `/v1`).

Timeouts are 10s connect / 120s read. On an unreachable backend claymore logs
`[claymore] hub LLM call failed: …` and (in `llm` mode) falls back to the
deterministic cited answer; in `tools` mode an unreachable backend → refuse.

---

## Recipes

### Local model (llama.cpp / vLLM on this box) — `tools` or `llm`
```jsonc
"mode": "tools",
"synthesis": { "url": "http://localhost:8080/v1", "model": "local-model", "format": "openai", "api_key_env": "NONE" }
```
Run e.g. `llama-server -m model.gguf --port 8080 --jinja` (the `--jinja` enables
tool-call parsing). Any OpenAI-compatible local server works.

### Remote OpenAI-compatible API — `tools`
Works for OpenAI, OpenRouter, Together, Groq, Fireworks, a remote vLLM/TGI, etc.
```jsonc
"mode": "tools",
"synthesis": { "url": "https://api.openai.com/v1", "model": "gpt-4o-mini", "format": "openai", "api_key_env": "OPENAI_API_KEY" }
```
```bash
export OPENAI_API_KEY=sk-...        # or whatever you named in api_key_env
./build/claymore spokes.json 9000
```
For another provider, change `url` (its `/v1` base), `model`, and `api_key_env`.

### Remote Anthropic — `llm` only
The tools loop is OpenAI-shaped, so use Anthropic for prose synthesis, not agentic tools:
```jsonc
"mode": "llm",
"synthesis": { "url": "https://api.anthropic.com", "model": "claude-3-5-haiku-latest", "format": "anthropic", "api_key_env": "ANTHROPIC_API_KEY", "max_tokens": 1024 }
```
(To use an Anthropic model *with tools*, front it with an OpenAI-shaped proxy — see below.)

### Redundancy / multi-provider — built in, no proxy
`backends` is an array; each entry inherits the top-level fields and overrides them.
claymore tries them round-robin with failover until one answers.
```jsonc
"synthesis": {
  "format": "openai", "api_key_env": "OPENAI_API_KEY", "max_tokens": 1024,
  "backends": [
    { "url": "https://api.openai.com/v1", "model": "gpt-4o-mini" },
    { "url": "http://localhost:8080/v1",  "model": "local-model", "api_key_env": "NONE" }
  ]
}
```

---

## When a proxy *is* the right call

Reach for an LLM proxy (e.g. [LiteLLM](https://github.com/BerriAI/litellm), which
presents a standard OpenAI `/v1/chat/completions` and translates behind it) only for
things the `synthesis` config can't express:

| Situation | Why config alone doesn't do it | What the proxy gives you |
|-----------|-------------------------------|--------------------------|
| **Azure OpenAI** | Azure uses an `api-key` header + `?api-version=` query + deployment-path URLs — not claymore's single bearer | Present Azure as a vanilla OpenAI endpoint; claymore stays simple |
| **Anthropic/Gemini *with tools*** | claymore's tools loop is OpenAI-shaped only | The proxy exposes those providers as OpenAI tool-calling endpoints, so `mode:"tools"` keeps working |
| **Governance** | rate limiting, cost caps, caching, key rotation, per-tenant routing, audit logging | one chokepoint for all of it |

Point claymore at the proxy as if it were OpenAI:
```jsonc
"synthesis": { "url": "http://localhost:4000/v1", "model": "azure-gpt4o", "format": "openai", "api_key_env": "LITELLM_KEY" }
```
**The tools are unaffected:** claymore still emits the `tools` array; the proxy just
relays the request to the real provider. The proxy does not need to know the tools
exist.

What a proxy does **not** solve, and you should not expect it to: it won't make a
non-tool-calling model agentic. If the underlying model can't do function calling,
use `mode:"llm"` regardless of the proxy.

---

## Discussion checklist

When deciding a backend with your team:

1. **Mode first.** Do you want agentic tool use (`tools`, needs tool-calling) or just
   grounded prose (`llm`, works with anything)? Or no LLM at all (`deterministic`)?
2. **Data egress.** A remote model sends your queries *and* the spokes' returned
   passages off-box. Acceptable? If not, keep the model local.
3. **Trust boundary.** Adding a hub LLM moves injection/hallucination risk to the
   hub. claymore still guarantees citations are real, but the prose is the model's.
4. **Provider fit.** OpenAI-shaped + tool-calling → direct config. Azure / Anthropic-
   with-tools / a gateway → a proxy. Multiple providers for HA → `backends[]` (no proxy).
5. **Keys.** Set them via env vars named in `api_key_env`; don't commit them in
   `spokes.json` (it's gitignored in this repo, but still).

See also: [README → The hub LLM](../README.md#the-hub-llm-modellm--local-model-or-remote-api),
and `docs/federation.md` for fronting redundant spokes.
