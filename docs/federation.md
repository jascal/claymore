# Federation & the universal catalog

claymore and sgiandubh share one API surface (`/v1/chat/completions`, `/retrieve`, `/lookup`, `/catalog`), so they
**compose recursively**: a claymore is interchangeable with a leaf expert as a spoke, and every node — leaf or hub —
describes itself through `/catalog`. The "librarian" is not a special node; it's this universal capability.

## The recursive flow

```
                         parent claymore  (GET /catalog?depth=4)
                          │   aggregates each spoke's /catalog, tags provenance, rolls up
                          ▼
                    child claymore  (GET /catalog?depth=3)
                     │            │
       ┌─────────────┘            └──────────────┐
       ▼                                          ▼
  riscv sgiandubh                          arXiv catalog expert (sgiandubh)
  GET /catalog → 1 self-card               GET /catalog → self-card + 121 document cards
  (the DEGENERATE librarian:               (a catalog package built by rosetta build_librarian;
   "expert riscv — N passages over …")      each card's handle lib:arxiv:<doc> points INTO a content expert)
```

- **Leaf (sgiandubh)** — `/catalog` returns a degenerate self-card for its one expert (id, kind, passages, facets,
  summary). A *catalog package* additionally returns its per-document `lib:*` cards.
- **Hub (claymore)** — `/catalog` federates every spoke's `/catalog`, tags each card with the spoke, and returns the
  union. A spoke that is itself a claymore contributes its *already-rolled-up* catalog → the catalog climbs the tree.

## Safety & degradation

- **Bounded recursion.** `?depth` (default 4) decrements at each hub hop; at 0 a hub stops descending. A misconfigured
  cycle (A→B→A) terminates after `depth` hops instead of looping over HTTP.
- **Partial results.** A spoke that's down contributes nothing (failover exhausted → empty); the rest still aggregate.
  Catalog/directory calls never hard-fail on one bad node.
- **High availability.** Any spoke (leaf or child hub) can list `urls`/`replicas`; `call_replica` load-spreads +
  fails over. Redundancy is horizontal (replicas) and vertical (nested hubs).

## Two query paths

- **catalog()** (tool) / `GET /catalog` — the federated *directory*: what experts/collections exist.
- **find_document(query)** (tool) / `POST /retrieve` — *search* across all spokes; returns cited cards/passages whose
  handle (`lib:<expert>:<doc>` or a content `id`) points into the owning expert. The LLM then `ask`/`lookup`s it.

## Redundant hub LLM

In `tools`/`llm` mode the hub's own LLM is redundant too: `synthesis.backends` is a list of endpoints
(primary API → fallback provider → local model); `call_synth`/`llm_turn` round-robin + fail over across them.
