#!/usr/bin/env bash
# BIG-BOX demo: the exact same stack as run-local.sh (content experts riscv/logic + librarian + tutors + the claymore
# hub in tools mode), but the hub brain is Qwen3-30B-A3B-Instruct-2507 instead of the 7B coder. This is a thin wrapper
# over run-local.sh — it only swaps the model and the readiness/tuning knobs, so everything else (spokes, tutors,
# /session, /catalog, the OpenAI endpoint on :9000) is identical.
#
# WHY A3B HERE: "A3B" = ~3.3B *active* params per token (8 of 128 experts fire), so per-token compute is that of a ~3B
# dense model — light for a fast CPU. The cost is MEMORY: the full ~18.6 GB (Q4_K_M) of weights must be reachable. On a
# box with enough RAM to hold it resident, decode is bandwidth-bound and snappy (~tens of tok/s); on a small-RAM box the
# weights page from disk and it drags. So: run this where there's plenty of RAM (and optionally a GPU). The non-thinking
# Instruct-2507 variant is deliberate — no reasoning traces clogging claymore's agentic tool loop.
#
# Usage (same flags as run-local.sh):
#   ./examples/run-bigbox.sh             → claymore hub (tools) as an OpenAI server on :9000, A3B brain
#   ./examples/run-bigbox.sh --cli       → the REPL (try /session socratic-tutor on riscv, then #1/#2 to pick a suggestion)
#   ./examples/run-bigbox.sh --verbose   → trace the tools loop
#
# Tuning (all overridable):
#   HF_REPO=...:Q5_K_M   bigger/smaller quant (Q5_K_M ~21.7GB, Q6_K ~25GB for more quality; Q3_K_M ~14.7GB to fit tighter)
#   MODEL=/path.gguf     use a local file instead of -hf download
#   NGL=99               GPU layers (default 99; harmlessly falls back to CPU when there's no GPU). NGL=0 forces CPU.
#   EXTRA_LLAMA_ARGS="--no-mmap"   on a big-RAM box, load fully into RAM (no first-token page faults). Needs RAM ≥ model.
#   CTX=16384            context window (this wrapper defaults higher than run-local for tool-result-heavy sessions)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"

# Qwen3-30B-A3B-Instruct-2507, Q4_K_M (~18.6 GB, SINGLE-FILE — `-hf` fetches it cleanly, no split-shard caveat).
export HF_REPO="${HF_REPO:-unsloth/Qwen3-30B-A3B-Instruct-2507-GGUF:Q4_K_M}"
export CTX="${CTX:-16384}"
# Big first-run download (~18.6 GB) + load → raise the readiness cap (still overridable).
export WAIT_MAX_SECS="${WAIT_MAX_SECS:-5400}"

echo "[run-bigbox] hub brain = ${MODEL:-$HF_REPO}  (A3B: ~3.3B active/token; wants the full weights in RAM to be fast)"
exec "$HERE/run-local.sh" "$@"
