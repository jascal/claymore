# Crux #1b — How sound can the NL-entailment gate (the *empirical* tier) be?

*Focused follow-up to [`crux1-certified-admission.md`](./crux1-certified-admission.md). Readable cold; full context in
the two prior docs.*

## Setting (tight recap)
A self-distilling system of **bounded experts**: on a coverage miss, a full LLM answers provisionally and the decision
is distilled into an expert *only if it passes an admission gate* requiring **corpus-entailment** (the answer follows
from the expert's fixed corpus). A prior result reframed the safety story: **a *sound* corpus-entailment gate makes
bound erosion impossible by construction** — admitted ⇒ corpus-grounded ⇒ growth stays within the corpus's support.
So the whole question reduces to **gate soundness, per tier**:
- **proved** — *formal* entailment over a structured shadow; sound by construction, but only for *formalizable* domains.
- **empirical** — an **NL-entailment check** for natural-language claims. **This doc is about that check.**
- **provisional** — full-model fallback; never enters the certified core.

## The question
The empirical tier admits an NL answer A iff a check says "corpus(E) ⊨ A." That check is itself (almost certainly) a
model, so it errs. **A false positive — admitting an A the corpus does *not* entail — is exactly one unit of bound
erosion** (an ungrounded item in the certified core). Since erosion-impossibility *depended on the gate being sound*,
the empirical tier's entire safety is the **false-positive rate (FPR)** of this check. **How do you characterize,
bound, and control it?**

## What makes it tractable: the error asymmetry
The two errors are not symmetric, and the design should exploit that:
- **False positive** (admit ungrounded): costly — enters the certified core, and *compounds* (a wrongly-admitted "fact"
  can later be cited as support for *other* admissions).
- **False negative** (reject a groundable A): cheap — A just stays **provisional** (still answered by the full model,
  merely not certified).

So the target is **bounded FPR at whatever recall that costs** — a *high-precision* gate, not high-recall. False
negatives are nearly free; precision is the objective.

## Candidate handles — which actually *bound* FPR?
1. **Refutation framing** — ask "find a reason A is *not* entailed by these passages; default to NOT-admit." Default-
   reject + refutation is often more reliable than confirmation.
2. **Independent ensemble** — admit only on unanimous K checkers. FPR drops geometrically *iff* errors are independent —
   but model errors are correlated (shared training/data). Does lens/model/prompt diversity recover enough independence
   to matter, or is the correlation floor fatal?
3. **Conformal / distribution-free calibration** — calibrate on a labeled set → guarantee FPR ≤ α w.h.p. on
   *exchangeable* future inputs. Rigorous, but live traffic isn't iid and adversarial drift breaks exchangeability. Does
   conformal survive here, and in what restricted form (per-claim-type? with drift detection that re-calibrates?)
4. **Atomic-claim decomposition** — check each atomic claim (smaller → more reliable per-claim); admit iff all pass. But
   per-claim errors accumulate over an answer, and decomposition can *drop cross-claim structure* (quantifier scope, an
   "only if", a temporal/jurisdictional condition).

## The deeper questions
5. **Verifier–generator asymmetry — does it exist for NL entailment?** *(the make-or-break.)* Is *checking* "passages ⊨
   A" reliably easier than *generating* A — so a cheaper, more trustworthy checker suffices? Or does NL entailment
   inherit the *same* failure modes as generation, so the gate is no more reliable than the model it polices — meaning
   the empirical tier is a comfortable illusion and only the **proved** tier (formalizable domains) is ever truly safe?
   If the asymmetry holds, dynamic admission is broadly safe; if not, it's safe only where you can formalize.
6. **Feedback-loop compounding.** Admissions aren't independent: a false positive can become cited evidence for later
   admissions (self-poisoning). Does per-item FPR have to be replaced by a **cumulative-drift** bound? Is there a
   self-poisoning threshold — an FPR below which the certified core stays stable and above which it diverges?
7. **Scope decision.** If FPR can't be bounded for a claim type, the honest move is: that type **stays provisional,
   never empirical-certified.** So the empirical tier's scope = exactly the claim types where the check's FPR is
   bounded-and-acceptable. How do you *decide membership* — and must it be measured per claim type and per expert?

## Constraint from our discipline
We tag claims **proved / empirical / open**, where *empirical* means **measured**. So an *empirical-admissible* item
should ship with a **measured FPR + a confidence bound**, not a vibe. The open methodological question is what makes
that number *valid* under live, possibly-adversarial traffic with a feedback loop.

## The one we most want cracked
**Q5.** If checking entailment is reliably easier than generating, the empirical tier is buildable and its soundness is
boundable. If not, only the *proved* tier is real — which would sharply narrow where the whole self-distilling loop is
safe to run. Everything else here is engineering around that answer.
