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

## Round 2 — converged refinements + two unresolved cruxes
*(after an external review pass)*

**Converged (adopt):**
- **Corpus = ground truth; an expert is a *materialized view* over it.** Distillation = view materialization; admission
  = the view must be derivable from the base table. This is the cleanest statement of the bound.
- **Machine-readable trust contract** + a `certified_only` query param / endpoint, so downstream agents set policy
  ("certified for high-stakes, provisional allowed for low").
- **Staleness via `base_model_version`** — a model update becomes a *re-compilation trigger* (eager re-distill of hot
  items, or lazy version-mismatch → treat as a miss).
- **Cost-aware admission** — distill on the Nth distinct miss in a window (or recurrence × savings > fill-cost × k);
  higher admission bar than a data cache because fills are expensive and ~irreversible.

**Sharpened / corrected:**
- **Grounding = corpus *support*, not retrieval *reproducibility*.** "Require retrieval to independently reach the same
  answer" is too strong — if retrieval already reaches it, distillation added nothing; the model's *synthesis beyond
  retrieval* is the whole value. Gate on **entailment/support by corpus passages**. Measure drift **vs the corpus, not
  vs the full model** (the model can be wrong or updated).

**Two cruxes still open (the real research):**
1. **Define "faithful derivation."** A *pure* view over the corpus **is** retrieval (no model). Distillation = view **+
   model inference**. Grounding gates the *endpoints* (evidence in, citation out) but **not the leap between them** — and
   "how much inference is a faithful derivation vs a hallucinated leap" is unsolved. This, not the invariant statement,
   is the load-bearing open problem for bound preservation.
2. **Spawning needs a corpus.** A dense miss-cluster is by definition off every existing corpus, and provisional
   full-model answers are **not ground truth** — so a "spawned expert" seeded from them is an *ungrounded memoization
   cache, not a bounded expert*. Emergent *bounded* experts require **acquiring a corpus** for the new domain, which
   traffic can't supply. Realistic form: "detect a coherent gap → **flag that a new corpus is needed**," not auto-create.

**Generalization, scoped:** separate *query-level* semantic reuse (paraphrase → nearest distilled answer; feasible now)
from *decision-level* rule induction reproducing `Σ contrib == logit` (symbolic regression of model internals; likely
intractable). The near-term path is query-level; decision-level induction stays a long-horizon maybe.

## The deciding reframe: gate the source, rebuild the artifact (don't patch the Datalog)
*The key simplification — it supersedes the "admit answers into the expert" framing above.*

Updating a partially-extracted expert is **the same operation as extracting it in the first place**. So don't treat
the dynamic loop as online learning (safely patch the certified Datalog); treat it as a **build system**:

> **expert = extract(corpus, questions, model)** — a pure function. Never hand-edit the compiled artifact; change the
> source and **rebuild the whole expert.**

Consequences:
- **The fallback answer is never certified content** — it's only a *signal that a gap exists*. So self-poisoning can't
  happen (nothing is patched) and the per-answer corpus-entailment gate evaporates. (The crux #1/#1b analysis was
  solving the wrong problem; it *relocates* — see below — rather than being wasted.)
- **Two gates, relocated and simpler:**
  - *Question-set gate (curriculum):* "is this query worth distilling?" Low-risk — it only spends extraction compute;
    the answer comes from re-extraction, not the fallback. The recurrence/cost policy lives here now.
  - *Corpus gate (ground truth):* "should this source enter the corpus?" High-bar, and **not the model's answer**
    (ungrounded). Recall miss → no corpus change; coverage gap → needs *authoritative source* (human/curated). This is
    where human-in-the-loop belongs, and it's a tiny surface vs gating every answer.
- **Grounded by construction:** the extraction step should be **RAG-over-corpus** — the model answers each curriculum
  question *using the corpus*, so every distilled answer is synthesized from the source (this is the contained change to
  `fieldrun --export-logic-corpus`). Then recall-misses fill grounded; coverage-gaps **abstain at extraction** (no
  passages exist) → stay provisional until the corpus is extended. The model's parametric guess never leaks in, and
  coherence falls out of the **holistic re-extraction over the whole corpus + question set** (bigger corpus + more
  questions → more coherent extraction).
- **Cadence:** rebuild is a **batch compile** (nightly / when the gated-update queue justifies it); provisional-full-
  model serves in between. CI/CD for experts: misses are bug reports, gating is triage, rebuild is the nightly.

Where the crux analysis still applies: **extraction-time grounding** (the RAG step) and the **answer trust tiers**
(proved / empirical / provisional still describe how much to trust an answer). What dissolves: incremental insertion
into the certified core.

## Converged design (after the crux rounds)
*(Read under the reframe above: "admission" = gating a corpus/question update, then rebuilding — not patching the
Datalog.)* Safety reduces to **structural constraints + one measured quantity**.

- **Admission requires corpus-entailment** ⇒ bound erosion is impossible by construction; growth asymptotes to the
  corpus's recall ceiling, never beyond. Misses auto-partition: *recall miss* → admit, *coverage gap* → provisional
  (closed only by adding to the corpus).
- **Raw-corpus-only grounding (provenance depth 0)** ⇒ no admission depends on another ⇒ self-poisoning impossible;
  per-item FPR composes independently.
- **RAG-over-corpus fallback** (not parametric) ⇒ there is evidence to entail against.
- **Three tiers, by what can be certified:**
  - **proved** — formal entailment over a structured shadow (formalizable domains / claim fragments; e.g. RISC-V from
    `norm-rules`). Sound by construction; **the only tier that can safely certify deep synthesis.**
  - **empirical** — NL-entailment gate, sound only for the **shallow-inference band** (light paraphrase / single-hop
    combine / aggregation); scope earned *per claim type* by a **measured FPR + confidence bound**; refutation /
    default-reject posture.
  - **provisional** — full-model fallback; everything uncovered or uncertifiable; always answers, never certified.
- **Evolution:** as structured-shadow coverage grows, empirical claims graduate to proved.

**Build implication.** The safest, highest-value first slice is the **proved gate on RISC-V** (the structured shadow
already exists as `norm-rules`) + **provisional** everywhere else; the **empirical** tier is added later, per claim
type, behind a measurement harness. Detail in [`crux1-certified-admission.md`](./crux1-certified-admission.md) and
[`crux1b-empirical-soundness.md`](./crux1b-empirical-soundness.md).

## Glossary
- **Bounded expert** — a model-free runtime that answers only its material and abstains elsewhere; the abstain is
  structural, not a filter.
- **Distillation** — fieldrun extracting a faithful Datalog certificate of the model's decision on a prompt.
- **Certified / faithful** — `Σ block-contributions == logit`; faithful to the model's *decision* (not a claim of
  factual correctness).
- **The bound is the router** — the hub needs no trained router; it asks everyone and keeps whoever didn't abstain.
