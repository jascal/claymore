# claymore

A **hub over [sgiandubh](https://github.com/jascal/sgiandubh) spokes** — one small C++ binary, OpenAI-compatible.

Point it at N bounded-expert spokes (each a sgiandubh server: a textbook, a spec, a manual). claymore fans a query
out to all of them, **drops the ones that abstain** (off-domain — the bound *is* the router), ranks the survivors by
confidence, and answers in one of two modes:

- **deterministic** — returns the top cited answer verbatim. Keeps every spoke guarantee (bounded, cited,
  injection-immune); no LLM in the loop.
- **llm** — synthesizes across the surviving cited answers with a hub LLM (any OpenAI-compatible endpoint). Flexible
  and conversational, but reintroduces the LLM's prompt-injection / hallucination surface *at the hub*.
- **tools** — *agentic*: a tool-capable hub LLM calls the spokes **as tools**; claymore executes the real spoke
  queries, feeds the cited results back, and loops to a final answer. One generic `consult_experts` tool by default
  (claymore fan-out-routes — best for small models), or `tool_style:"per-expert"` for one `ask_<spoke>` tool each.

The hard promise is enforced **in code, not a prompt**: if every spoke abstains, claymore refuses — which survives a
jailbroken hub LLM. Clients see one OpenAI endpoint regardless of mode.

```
                 ┌──────────── claymore ────────────┐
   client ──────▶│ fan-out · drop abstains · rank    │
                 └──┬─────────┬─────────┬────────────┘
                    ▼         ▼         ▼   (OpenAI /v1/chat/completions)
                 riscv     logic     anatomy …   ← sgiandubh spokes (bounded, cited)
                 :8081     :8082     :8083
```

## Getting started (full stack, from scratch)

**Prerequisites** — a C++17 compiler (`g++`); for the **spokes** ([sgiandubh](https://github.com/jascal/sgiandubh)):
[Soufflé](https://souffle-lang.github.io) + headers, `libsqlite3`, `zlib`, and `python3` with `numpy`/`scipy` (only
for building grounding vectors); for **claymore**: OpenSSL (optional — only for HTTPS llm-synthesis).
Debian/Ubuntu: `sudo apt install g++ souffle libsqlite3-dev zlib1g-dev libssl-dev python3-numpy python3-scipy`.

### 1 — a spoke (sgiandubh)
```bash
git clone https://github.com/jascal/sgiandubh && cd sgiandubh
./build.sh                                                  # → build/sgiandubh (embeds the Datalog engine)
```
Give it a **package** (the expert's data) — copy a prebuilt `package_*/`, or build one. The RISC-V ISA spec needs
**no model** (grab `norm-rules.json` from the latest
[riscv-isa-manual release](https://github.com/riscv/riscv-isa-manual/releases)):
```bash
python3 tools/normrules2package.py norm-rules.json package_riscv
python3 tools/build_grounding.py --corpus package_riscv/rules.txt      --out package_riscv --no-split
python3 tools/build_gram.py      --corpus package_riscv/rules_plain.txt --out package_riscv/gram
./build/sgiandubh package_riscv 8081 --answer-from-corpus &            # spoke up on :8081
```
(See sgiandubh's `WORKFLOW.md` to build experts from your own textbook PDFs / corpora — incl. the model-distilled
reasoning tier via fieldrun.)

### 2 — the hub (claymore)
```bash
git clone https://github.com/jascal/claymore && cd claymore
./build.sh                                                  # → build/claymore (TLS auto-detected)
cp spokes.example.json spokes.json                          # edit: list your spokes; set mode
./build/claymore spokes.json 9000                           # hub up on :9000
```

### 3 — use it
Point any OpenAI client at `http://localhost:9000/v1` (or `http://localhost:9000` for an Anthropic client). For the
`llm` synthesis mode (local llama.cpp or a remote API), see *The hub LLM* below.

**CLI / manual testing:** `./build/claymore spokes.json --repl` — read queries from stdin and print answers, no
server. Honors the configured mode (deterministic / llm / tools). Handy for quick checks against the hub.

## Why the abstain-router works
Every sgiandubh spoke answers only its own material and abstains otherwise, so claymore doesn't need a trained
router: ask everyone (each call is sub-millisecond), keep whoever didn't abstain. Add a textbook → add a spoke line.
At large spoke counts, pre-filter by each spoke's `domain` description before fanning out.

**Relevance gate (defense-in-depth).** Spoke abstain isn't perfect, so claymore also drops any response whose overlap
with the query is below `--min-relevance` (default 0.10, lexical) *before* it's cited or fed to the LLM — e.g. a
RISC-V vector rule returned for "predicate calculus" gets dropped, so it never pollutes the answer or the sources.
This is a cheap backstop, **not** a substitute for spoke abstain (a spoke queried directly still abstains on its own);
it only catches egregiously off-topic responses, and it's tunable.

## API surface (drop-in, same as sgiandubh)
Verified against the official OpenAI/Anthropic SDKs:
- OpenAI: `POST /v1/chat/completions`, `POST /v1/completions`, `GET /v1/models` (non-stream + SSE `stream:true`).
- Anthropic: `POST /v1/messages` (content-block response + Anthropic SSE events).
- `response_format:{type:"json_object"}` → `content` is a JSON string of `{answer, mode, sources:[{spoke,citation}]}`
  for an embedded app to render its own UI.
- `GET /v1/domains` — the **manifest**: what each spoke covers (so an outer agent / opencode can discover when to
  call claymore, and use it as a tool).
- `GET /health` (alias `/healthz`) — live **spoke health**: per-spoke up/down + latency, overall `status`
  (`ok`/`degraded`/`down`; HTTP 503 if all down). claymore also prints a spoke health report **at startup** (CLI and
  server), so an unreachable spoke is obvious instead of showing up as silent "nothing covers that" refusals.

claymore also *speaks* tool-calling in `mode:"tools"` (it drives a tool-capable LLM that calls the spokes). The
bounded *spokes* (sgiandubh) have no tools — they answer; tool-calling lives only at the hub.

## The hub LLM (`mode:"llm"`) — local model OR remote API
`synthesis.url` is just an endpoint, so either works — switch by config, no rebuild:

```jsonc
// LOCAL model via llama.cpp (run:  llama-server -m model.gguf --port 8080)  — OpenAI-compatible, no key
"synthesis": {"url": "http://localhost:8080/v1", "model": "local-model", "format": "openai"}

// REMOTE OpenAI (or any OpenAI-compatible API)
"synthesis": {"url": "https://api.openai.com/v1", "model": "gpt-4o-mini", "format": "openai", "api_key_env": "OPENAI_API_KEY"}

// REMOTE Anthropic
"synthesis": {"url": "https://api.anthropic.com", "model": "claude-3-5-haiku-latest", "format": "anthropic", "api_key_env": "ANTHROPIC_API_KEY", "max_tokens": 1024}
```
`format` selects the backend shape (`openai` covers llama.cpp + OpenAI + most; `anthropic` uses `/v1/messages`). Keys
are read from the named env var (local llama.cpp needs none). `top_k` bounds how many spoke answers feed the
synthesizer. If the backend is unreachable, claymore falls back to the deterministic cited answer.

## Guarantee boundary
Keep the hard promises in claymore (the deterministic gate: all-abstain → refuse; citations carried from spokes), and
let the optional hub LLM handle only phrasing/synthesis. A jailbreak of the hub LLM degrades *style*, not *safety* —
the bound and the citations live in code it can't reach.

License: Apache-2.0.
