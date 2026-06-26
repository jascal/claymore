#!/usr/bin/env bash
# Bring up the FULL local stack on a GPU box, no external APIs:
#   a tool-capable coding model (llama.cpp, on the GPU)  ──tools──▶  claymore  ──fan-out──▶  bounded-expert spokes
# Everything talks the OpenAI API; the model calls the experts as tools, claymore runs them, the model finalizes.
#
# Edit the paths/model below (or override via env), then:
#   ./examples/run-local.sh            → claymore hub as an OpenAI server on :9000
#   ./examples/run-local.sh --cli      → claymore CLI/REPL instead (type queries at a prompt; no server)
#   add --verbose to either            → trace the tools loop (tool calls, spoke hits, refuse reason)
set -e
CLI=0; VERBOSE=${VERBOSE:-}
for a in "$@"; do case "$a" in --cli) CLI=1 ;; --verbose|-v) VERBOSE=1 ;; esac; done
LLAMA=${LLAMA:-$HOME/code/llama.cpp/build/bin/llama-server}
# Model: by default auto-download from Hugging Face (-hf, cached under ~/.cache/llama.cpp on first run, ~4.7 GB).
# Use bartowski's SINGLE-FILE GGUFs: the official Qwen *-GGUF repos ship SPLIT quants that -hf can fetch
# incompletely, so the model loads and runs fast but emits garbage. Q4_K_M runs well on an 8 GB GPU at -ngl 99.
# To use a local .gguf instead, set MODEL=/path/to/model.gguf ; or pick a different repo with HF_REPO=... .
# Smaller/comfier option for tighter VRAM:  HF_REPO=bartowski/Qwen2.5-Coder-3B-Instruct-GGUF:Q4_K_M
HF_REPO=${HF_REPO:-bartowski/Qwen2.5-Coder-7B-Instruct-GGUF:Q4_K_M}
MODEL=${MODEL:-}
SG=${SG:-$HOME/code/sgiandubh}
HERE="$(cd "$(dirname "$0")/.." && pwd)"

if [ -n "$MODEL" ]; then SRC=(-m "$MODEL"); else SRC=(-hf "$HF_REPO"); fi
echo "1/3 · coding model on the GPU (CUDA -ngl 99 · tool-calling --jinja) → :8080   [${MODEL:-$HF_REPO}]"
"$LLAMA" "${SRC[@]}" -ngl 99 -c 8192 --jinja --host 127.0.0.1 --port 8080 >/tmp/llama.log 2>&1 &
echo "2/3 · bounded-expert spokes → :8081 (riscv), :8082 (logic)"
"$SG/build/sgiandubh" "$SG/package_riscv" 8081 --answer-from-corpus >/tmp/spoke_riscv.log 2>&1 &
"$SG/build/sgiandubh" "$SG/package_logic" 8082 --answer-from-corpus >/tmp/spoke_logic.log 2>&1 &

# wait for the model to load (the slow one) + the spokes
for url in localhost:8080/v1/models localhost:8081/v1/models localhost:8082/v1/models; do
  until curl -s --max-time 3 "$url" >/dev/null 2>&1; do sleep 2; done
done
if [ "$CLI" = 1 ]; then
  echo "3/3 · claymore CLI (tools mode) — type a query; blank line or Ctrl-D to exit"
  exec "$HERE/build/claymore" "$HERE/examples/local.spokes.json" --repl ${VERBOSE:+--verbose}
else
  echo "3/3 · claymore hub (tools mode) → :9000   ·   point any OpenAI client at http://localhost:9000/v1"
  exec "$HERE/build/claymore" "$HERE/examples/local.spokes.json" 9000 ${VERBOSE:+--verbose}
fi
