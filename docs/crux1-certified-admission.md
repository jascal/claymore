# Crux #1 — Can expert admission be a *certified entailment* over the corpus?

*Focused follow-up to [`dynamic-experts.md`](./dynamic-experts.md). Written to be reasoned about cold.*

## Setup (one paragraph)
We have **bounded experts**: small runtimes that answer only their material and abstain otherwise. A hub fans queries
to them; on a universal abstain (coverage gap) a **full LLM** answers provisionally and the decision is **distilled**
back into the appropriate expert so the next similar query is served by the fast, certified path. The single
load-bearing safety constraint is the **admission gate**: a distilled answer may enter a bounded expert *only if* it is
a **"faithful derivation from corpus evidence."** Otherwise the expert silently accretes ungrounded model output and
the bound — the entire reason the expert is trustworthy — erodes.

**The question:** can "faithful derivation" be a **checkable proof obligation**, not a confidence/grounding heuristic?

## The distinction the whole thing hinges on
We already emit, per model decision, a **faithful certificate**: a linear decomposition where `Σ block-contributions ==
logit`, re-derivable by a Datalog engine. **But that certifies faithfulness to the *model*** — "the model really
computed this output; here is the exact attribution." It says **nothing about whether the answer is entailed by the
corpus.** A hallucination has a perfectly valid model-decision certificate: it is a genuine decision, just ungrounded.

So admission needs a **different** certificate — **corpus-entailment** (`answer A follows from corpus(E)`), not
model-faithfulness. The crux is *constructing* that second certificate (and/or bridging from the first).

## The rigor spectrum for "A follows from corpus(E)"
1. **Lexical grounding** (today's heuristic): A overlaps some passage. Cheap; fooled by shared words / quote-mining.
2. **NL entailment judge**: a model decides "passages ⊨ A." But that's *another model* — it relocates the trust, it
   doesn't ground it.
3. **Claim decomposition + per-claim grounding**: split A into atomic claims; each must be supported by a passage.
   More auditable; still NL-soft per claim.
4. **Formal entailment**: if corpus-fragment + A are expressible in a formal language, entailment is *machine-checkable*
   (a real proof) — but only for *formalizable* claims/domains.

## The composition we're reaching for
We run a discipline of tagging every claim **proved** (machine-checked) / **empirical** (measured) / **open**. The hope
is that the admission gate becomes a **proof obligation discharged in that same machinery**, yielding admission *tiers*
that mirror it:
- **proved-admissible** — a machine-checked entailment from corpus evidence (formalizable claims).
- **empirically-grounded** — supported above a measured threshold (NL claims).
- **provisional** — full-model, no corpus tie (the safety net; never enters the certified core).

That would turn "grounding-gated admission" into **proof-gated admission**, and make the self-growing loop
auditable-by-construction rather than confidence-thresholded.

## Questions
1. **What is the right proof obligation** for "A is a faithful derivation from `corpus(E)`"? Is there a formulation
   cleaner / stronger than claim-decomposition + per-claim grounding?
2. **Can NL answers ever be *proved*-admissible**, or is formal entailment only reachable for answers first reduced to a
   formal/structured fragment — and if so, **how do you certify the reduction** (the NL→formal step is itself a trust
   hop)?
3. **Does model-faithfulness help?** We can certify the model's *decision* (`Σ contrib == logit`). Can that be composed
   with a corpus-entailment check, or are the two simply orthogonal (faithful-to-model vs faithful-to-corpus)?
4. **Graceful degradation:** what's the principled ladder proved → empirical → provisional, and how should the trust
   contract expose the tier so downstream agents can set policy?
5. **Failure modes of proof-gating:** where does a *formally valid* derivation still give false assurance — e.g., a
   passage quoted out of context, a corpus that is itself wrong, or an atomic-claim decomposition that loses a
   cross-claim dependency? Is "the corpus is ground truth" an assumption that needs its own audit?

## Why this matters here specifically
Most RAG/agent systems gate on *confidence*, which optimizes for "the model is sure" — exactly when it is confidently
wrong. We already have certification machinery for model decisions; the prize is extending it to **certify the
admission step itself**, so a system that *grows from its own traffic* stays bounded by *proof*, not by a threshold.
That is the difference between a clever cache and an auditable, self-improving knowledge system.
