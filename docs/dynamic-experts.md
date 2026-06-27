# Dynamic Experts — a self-distilling cache over bounded experts

*Design sketch / thought experiment. Status: idea, not built. Written to be reasoned about by a reader with no prior
context on the system.*

## TL;DR
Today we have **bounded experts** (small, certified, fast) that answer within their material and **abstain** outside
it, behind a hub that routes by abstain. The proposal: when **every expert abstains** (a coverage gap), fall back to a
**full LLM** to answer *now*, and **asynchronously distill that decision back into the appropriate expert** so the next
identical/similar query is served by the certified expert. The abstain stops being a dead end and becomes the
**curriculum**: the system specializes on demand, against real traffic. It's a JIT compiler for knowledge — interpret on
the big model, compile the hot paths into certified experts.

---

## Background: the substrate (what already exists)

Three layers, all built and running:

1. **fieldrun (distiller, offline, heavy).** Runs a transformer over a prompt and emits, per decision, a **Datalog
   certificate**: `candidate / contrib("block", id, weight) / decide`, where `Σ contrib == logit`. It's a *faithful
   linear decomposition of the model's actual decision* on that prompt — not an approximation. Output: one `.dl` per
   decision (+ a short generated answer).

2. **sgiandubh (expert runtime, tiny, model-free).** A ~1.3 MB OpenAI-compatible server over a package of distilled
   items + a corpus. At query time it routes: **faithful** (a distilled item matches → replay its certified answer, the
   Soufflé engine re-derives `Σ block == logit`) → **retrieval** (best corpus passage, cited) → **gram** (n-gram
   continuation) → **abstain**. Key properties: *bounded* (off-domain is structurally unanswerable, not filtered),
   *grounded* (every answer carries a verbatim source passage), *auditable* (the answer is or is certified by a Datalog
   decision), *no model/GPU at runtime*.

3. **claymore (hub).** Fans a query to N expert "spokes," **drops the ones that abstain** ("the bound *is* the
   router"), ranks survivors by confidence, and either returns the top cited answer (deterministic) or has a hub LLM
   synthesize over the cited answers (tools/llm mode).

**The coverage fact that motivates this.** A memoized/Datalog expert covers *exactly the decisions distilled into it*.
Fully covering a *general* model is information-theoretically impossible as a compression — it would require ~the
model's full description length (you'd just rebuild the model). But a **bounded domain** is coverable at a scale set by
that domain's *decision entropy*, not the input space. The open problem has been: you can't pre-enumerate even a
domain's queries. **Dynamic experts answers that: don't enumerate — cover the empirical query distribution, online.**

---

## The idea

```
query
  └─▶ claymore fans out to spokes
        ├─ some spoke confident  ─▶ certified answer (fast, cheap, bounded)   ← the hot path
        └─ ALL abstain / below confidence  (a coverage gap)
              ├─ async: full LLM answers ─▶ return NOW, marked PROVISIONAL (uncertified)
              └─ enqueue: distill(query) ─▶ admission gate ─▶ hot-reload the right expert
                                                                   ↑ next identical/similar query is CERTIFIED
```

- **First ask** of a novel in-distribution query: slow, served by the full model, *provisional*.
- **Subsequent asks**: fast, cheap, certified, served by the now-specialized expert.
- Cost amortizes over recurrence; the certified core grows to cover whatever real users actually ask.

This is a **semantic cache** where the full model is the backing store, the bounded experts are the certified hot
cache, and **distillation is the cache-fill**. The cache key is semantic (query intent), not literal.

---

## Why it's interesting

- **It converges to the right scale.** The expert's size tracks the *empirical decision entropy of usage* (Zipfian,
  small) instead of the input space (unbounded). You discover the domain's true coverage requirement from traffic.
- **The abstain becomes a signal, not a failure.** Every gap is logged, prioritized by recurrence, and closed. The
  bound is the curriculum.
- **Measurable convergence.** You can watch certified-hit-rate climb and the distill queue drain as an expert
  saturates its domain — an online version of the compression/coverage question.
- **Graceful: always answers** (full-model safety net) while keeping a *fast, cheap, certified, bounded* core for the
  covered majority.
- **Emergent expert discovery (speculative):** if a coherent cluster of misses doesn't fit any existing domain, the
  system could **spawn a new expert** — it discovers which experts it needs.

---

## Open problems / design tensions (where we want sharp thinking)

1. **Bound erosion — the load-bearing constraint.** The full-model fallback answer is the *model's knowledge*, not
   *corpus-grounded* truth. If admitted wholesale, the expert slowly accretes ungrounded model output and drifts back
   into "the model + its hallucinations" — destroying the bound that justified the expert. Our current stance:
   **gate admission on grounding** (the answer must tie to a corpus passage / carry a citation), not on confidence.
   Confidence-only admission optimizes for "the model is sure," which is exactly when it's confidently wrong, and the
   feedback loop would compound that. *Is grounding-gated admission sufficient? What else bounds the drift?*

2. **Two-tier trust.** Provisional (full-model, uncertified) vs certified (distilled, bounded) answers must be visibly
   distinct to the caller. *How should that contract surface — a field, a separate endpoint, a confidence band?*

3. **Admission policy & cost.** Distillation is the expensive step; long-tail one-off queries never recur, so
   distilling every miss is wasteful. Cache-style answer: **distill on the 2nd miss**, not the 1st. *What's the right
   admission/eviction policy for a knowledge cache? Is there an analogue of LFU/ARC here?*

4. **Routing a miss is ambiguous by construction.** Nothing was confident — that's *why* it's a miss — so "the
   appropriate expert" isn't obvious. Classify the *answer's* content? Nearest domain by embedding? Spawn a new expert
   on a coherent off-domain cluster? *What's the principled routing/spawn rule?*

5. **Verification & adversarial traffic.** A live-traffic-fed expert is attackable; the full model's errors get baked
   in as "certified" (certified = *faithful to the model's decision*, which may still be wrong). *What verification gate
   (grounding + self-consistency + human-in-loop for high-stakes) keeps the certified tier trustworthy under hostile
   input?*

6. **Faithful generalization vs memoization.** Distilling a query covers *that* query. Covering *similar* novel queries
   faithfully (not just via lossy n-gram fallback) would need **rule induction** — synthesizing generalizing Datalog
   rules from accumulated decisions. *Is on-the-fly rule induction over the growing decision set tractable, and does it
   preserve the `Σ block == logit` faithfulness guarantee?*

7. **Staleness.** If the full model is updated, the cached/distilled decisions are now from an old model. *Versioning /
   invalidation policy?*

---

## Questions for the reader

1. Is **grounding-gated admission** the right (or sufficient) defense against bound erosion, or is there a cleaner
   invariant that keeps a self-growing expert bounded?
2. What's the **right admission/eviction policy** for a *knowledge* cache (vs a data cache) — what's the cost model when
   a "miss-fill" is an expensive, irreversible-ish distillation?
3. How should a **miss be routed** to an expert when, by definition, no expert claimed it — and when (if ever) should
   the system **spawn a new expert**?
4. Is there a principled **trust contract** between the certified and provisional tiers that downstream agents can rely
   on?
5. Could **rule induction** over the accumulated certificates turn this from memoization into faithful generalization
   without sacrificing auditability — and at what cost?
6. Failure modes we're not seeing? Where does this break at scale, under drift, or under adversarial use?

---

## Glossary
- **Bounded expert** — a model-free runtime that answers only its material and abstains elsewhere; the abstain is
  structural, not a filter.
- **Distillation** — fieldrun extracting a faithful Datalog certificate of the model's decision on a prompt.
- **Certified / faithful** — `Σ block-contributions == logit`; faithful to the model's *decision* (not a claim of
  factual correctness).
- **The bound is the router** — the hub needs no trained router; it asks everyone and keeps whoever didn't abstain.
