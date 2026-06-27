# Core reasoning KB — factor reasoning from knowledge

*Design sketch / idea, **not built**. Sibling to [`dynamic-experts.md`](./dynamic-experts.md) and the crux docs.
Readable cold.*

## TL;DR
Each extracted expert today distills **both** the domain knowledge **and** the reasoning, entangled in the model's
decisions (and faithful to the *model*, not the corpus). Proposal: **split them** — ship a shared, **verified core
reasoning KB** (general inference rules) compiled into every Soufflé runtime, plus per-expert **fact bases**
(propositions/relations). The engine then **derives** in-domain answers by applying the core rules to the facts, instead
of replaying memoized decisions. Buys generalization (flexibility) and auditable proof-tree derivations (faithfulness)
— this is the **proved tier** — for formalizable domains.

## Are the exports composable? (the enabling property)
Datalog composes by **union** — a substrate strength neural weights lack (the least fixpoint of a union of rule-sets +
fact-sets is well-defined). In this runtime, with caveats:
- **Facts compose at runtime** — the compiled Soufflé engine re-derives from whatever EDB you load.
- **Rules are compiled in** (`souffle -g`, embedded, no spawn) — so composing *rule-sets* is a **build-time** act
  (compile them in) or needs the Soufflé interpreter (slower; drops the embed property). The compiled choice was
  deliberate; it just means rule-composition happens at build, not load.
- sgiandubh loads **one** package today; a multi-package union (merge `index.json` items + `knowledge.tsv` + fact-sets)
  is a modest feature. Cross-expert composition currently lives in **claymore's fan-out**, which keeps experts in
  *separate processes* — so it **cannot do cross-domain derivation**; an in-runtime union could.
- **But the current `.dl` exports are decode certificates** (`candidate/contrib/decide` — a memoized logit
  decomposition), **not reasoning KBs.** Unioning two gives two memoized decisions side by side, no new reasoning. So
  composability is real, but the current exports aren't the thing worth composing.

## The idea
- **Reasoning** → a shared **core KB** of general inference rules (modus ponens, transitivity, De Morgan, …),
  **compiled into every runtime**, **verified once**.
- **Knowledge** → per-expert **facts** (propositions/relations), loaded.
- **Serving order:** **derive** (core rules over the fact base) → **retrieve** (corpus passages) → gram → abstain. The
  derivable layer sits on top of the existing tiers, not replacing them.

## Why it helps
- **Flexibility** — the engine can **derive** new in-domain conclusions (apply rules to facts) instead of only replaying
  distilled decisions. That is the generalization the experts lack — the direct cure for the *frustratingly limited*
  half of the flexible/limited duality. It also sidesteps the "rule induction" gap: you don't *induce* rules, you
  *supply a verified core* and let Datalog's fixpoint generalize.
- **Faithfulness** — derivations are **auditable proof trees**, and an answer's trust reduces to *facts correct +
  core rules sound (verified once)* — a far smaller surface than per-expert distilled reasoning. This **is** the proved
  tier from `crux1`: a core reasoning KB is the formal-entailment machinery, and "always include it" is how you'd
  implement proof-gated admission.

## The boundary (honest)
Only works where knowledge is expressible as **propositions/relations + Datalog rules** — the **structured shadow**,
for **formalizable** domains (logic; a spec like RISC-V `norm-rules`). Arbitrary NL prose (a textbook) doesn't reduce to
clean propositions and stays in the **retrieval/empirical** tier — no core-KB derivation. So this is the *proved-tier
implementation*, powerful where the domain formalizes and composed with retrieval for the rest. **Not universal.**

## Two things that fall out
1. **A new export mode.** Export the domain as a propositional/relational **fact base** (alongside the decode
   certificates, which stay — they certify the model's decisions) and ship one verified core rule-set compiled into the
   engine. The fact base + core rules are the *derivable* layer.
2. **Cross-domain derivation the hub can't do.** Shared core + multiple domains' facts in *one* runtime → derive across
   them (RISC-V facts under logic rules). Claymore's fan-out structurally can't; an in-runtime union can. A capability,
   not just a refactor.

## Relation to the other docs
- It's the concrete implementation of `crux1`'s **proved tier** (formal entailment over a structured shadow).
- It cures the limited half of the **flexible/limited duality** (derivation > memoization).
- It pairs with **dynamic-experts**: the build-system reframe (gate the corpus/questions, rebuild) means you rebuild the
  *fact base*; the reasoning is the shared, verified, compiled-in core — never patched.

## Open questions
1. **Core rule-set scope** — domain-general logic only, or per-family cores (a logic core, a temporal/spec core)? How
   large before it's slow or unsound?
2. **Verifying the core** — the rules must be *sound* (modus ponens is; a sloppy rule isn't). One-time proof obligation,
   discharged by i-orca? This is the part that makes the whole thing trustworthy.
3. **Getting the fact base** — the structured-shadow extraction (NL/model → typed propositions) is itself a trust hop
   (`crux1` Q2: certify the NL→formal reduction). How is *that* certified?
4. **Facts are ground truth** — a derivation is only as good as its facts; a wrong fact yields a valid-but-wrong
   derivation (`crux1b`: who audits "the corpus is ground truth"). Provenance + scope conditions on facts.
5. **Compiled-in vs interpreter** — compile the core for speed/embedding (fits "always include") vs interpret for
   runtime rule-flexibility. Trade noted.
6. **Termination / cost** — Datalog fixpoint over rules + facts can blow up (recursion, dense joins — the LE-T2
   provenance/treewidth issue). Needs stratification + bounds.

**Status: idea.** Build only after the structured-shadow extraction and core-verification questions (2–4) have answers.
