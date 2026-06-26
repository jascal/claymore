# claymore

A **hub over [sgiandubh](https://github.com/jascal/sgiandubh) spokes** — one small C++ binary, OpenAI-compatible.

Point it at N bounded-expert spokes (each a sgiandubh server: a textbook, a spec, a manual). claymore fans a query
out to all of them, **drops the ones that abstain** (off-domain — the bound *is* the router), ranks the survivors by
confidence, and answers in one of two modes:

- **deterministic** — returns the top cited answer verbatim. Keeps every spoke guarantee (bounded, cited,
  injection-immune); no LLM in the loop.
- **llm** — synthesizes across the surviving cited answers with a hub LLM (any OpenAI-compatible endpoint). Flexible
  and conversational, but reintroduces the LLM's prompt-injection / hallucination surface *at the hub*.

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

## Build & run
```bash
./build.sh                                   # one binary: build/claymore  (TLS auto-detected for https synthesis)
cp spokes.example.json spokes.json           # edit: your spokes + mode
# start your spokes (sgiandubh), then:
./build/claymore spokes.json 9000
```
Then point any OpenAI client at `http://localhost:9000/v1`.

## Why the abstain-router works
Every sgiandubh spoke answers only its own material and abstains otherwise, so claymore doesn't need a trained
router: ask everyone (each call is sub-millisecond), keep whoever didn't abstain. Add a textbook → add a spoke line.
At large spoke counts, pre-filter by each spoke's `domain` description before fanning out.

## Config (`spokes.json`)
```json
{
  "spokes": [{"name":"riscv","url":"http://localhost:8081","domain":"RISC-V ISA spec"}],
  "mode": "deterministic",
  "top_k": 3,
  "synthesis": {"url":"https://api.openai.com/v1","model":"gpt-4o-mini","api_key_env":"OPENAI_API_KEY"}
}
```
`mode: "llm"` uses `synthesis` (any OpenAI-compatible endpoint; key read from the named env var). `top_k` bounds how
many spoke answers feed the synthesizer.

## Guarantee boundary
Keep the hard promises in claymore (the deterministic gate: all-abstain → refuse; citations carried from spokes), and
let the optional hub LLM handle only phrasing/synthesis. A jailbreak of the hub LLM degrades *style*, not *safety* —
the bound and the citations live in code it can't reach.

License: Apache-2.0.
