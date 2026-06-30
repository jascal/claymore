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
server. Honors the configured mode (deterministic / llm / tools). Handy for quick checks against the hub. The REPL
also exposes the other two layers of the stack: `/catalog` (the librarian — what experts exist), `/tutors` (teaching
templates from a `role:"pedagogy"` spoke), `/experts` (the content experts), and `/session <tutor> [on <expert>…]` to
enter a bounded **mentoring session** (multi-turn; the tutor is scoped to the chosen experts = the syllabus boundary);
`/end` leaves it. `/help` lists them.

**Full-stack demo:** `examples/run-local.sh` brings up a no-external-APIs stack on one box — a tool-capable coding
model (llama.cpp) + content experts (riscv, logic) + a **librarian** (`role:"librarian"`, a model-free catalog over
the content experts) + **tutors** (`role:"pedagogy"`, model-free prompt templates). Run it with `--cli` for the REPL
(try `/catalog`, `/tutors`, `/session socratic-tutor on riscv`, then `#1`/`#2` to pick a suggested next prompt) or bare
for the OpenAI server on :9000. The librarian and tutors are built by [rosetta](https://github.com/jascal/rosetta) (a
sibling checkout); without it the script still runs the content layer and notes the others are disabled.

**Big-box demo:** `examples/run-bigbox.sh` is the same stack with a much larger hub brain — **Qwen3-30B-A3B-Instruct-2507**
(a sparse MoE: ~3.3B active params/token, so it's light on CPU compute but wants the full ~18.6 GB of weights in RAM to
be fast). Use it on a machine with plenty of RAM (and optionally a GPU); on a small-RAM box the experts page from disk
and it drags. Same flags (`--cli`, `--verbose`) and env knobs (`HF_REPO`/`MODEL`/`NGL`/`CTX`/`EXTRA_LLAMA_ARGS`).

A quick REPL tour (`./examples/run-local.sh --cli`):

```
/catalog                          # the librarian — every expert/collection this hub fronts
/experts                          # the content experts you can study
/tutors                           # the teaching templates (model-free prompt experts)
/session socratic-tutor on riscv  # enter a bounded tutoring session; ask away; /end to leave
what does the LR/SC pair guarantee?
/end
how many RISC-V instructions are there?   # back to plain Q&A (fans out to the experts)
```

Tear the whole stack down with `pkill -f 'llama-server|build/sgiandubh|build/claymore'`.

**REPL rendering.** On an interactive terminal the REPL renders **Markdown and inline TeX / MathML math** (headings,
bold/italic, code blocks, lists, and `$…$` / `\(…\)` / `$$…$$` / `<math>…</math>` → Unicode). It's TTY-gated: piped
output stays raw, so scripts and tests are unaffected.

**Subset experts (a key into a large expert).** A spoke may carry an optional `"key"`: claymore forwards it on every
call so that spoke sees only the **slice** of the expert's content whose section/id matches the key. Two spoke entries
can share one `url` but differ in `key` — one big sgiandubh fronted as several bounded subset-experts (e.g. a `riscv`
spoke and a `riscv-vector` spoke with `"key": "V Extension"` over the same server). Off-slice queries abstain.

**Teaching sessions** (`/session`, or a request `session` object) are driven by pedagogy templates built by
[rosetta](https://github.com/jascal/rosetta). Beyond the system scaffold a template may carry: an **opener** (the tutor
speaks first), **suggested next prompts** (shown after each turn), and a **scope restriction** (`applies_to`/`subject` —
which experts the tutor may be applied to). All of that wording lives in the template, not in claymore — the binary adds
only the in-code safety guardrail.

**A tutor on a slice of one expert.** The session and subset features compose: give a session a `key` and the tutor is
confined to that slice on every tool call — e.g. a tutor on a whole-book expert restricted to one chapter. In the REPL:
`/session socratic-tutor on book key Chapter 3`; via the API: `{"session": {"template": "...", "scope": ["book"],
"key": "Chapter 3"}}`. (You can also pre-bake a subset as its own keyed spoke and start the tutor on that — same effect.)

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

**Pointing at a separate / remote LLM, `mode:"tools"` vs `mode:"llm"`, redundant `backends`, and when an LLM proxy
(Azure, Anthropic-with-tools, gateways) is or isn't needed — and how the expert tools get registered through one — are
covered in [`docs/llm-backends.md`](docs/llm-backends.md).** Short version: claymore is the agent, the tools ride in
the request body, so switching endpoints is just config — a proxy is rarely required and is transparent to the tools.

## Guarantee boundary
Keep the hard promises in claymore (the deterministic gate: all-abstain → refuse; citations carried from spokes), and
let the optional hub LLM handle only phrasing/synthesis. A jailbreak of the hub LLM degrades *style*, not *safety* —
the bound and the citations live in code it can't reach.

License: Apache-2.0.
