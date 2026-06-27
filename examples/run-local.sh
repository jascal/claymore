#!/usr/bin/env bash
# Bring up the FULL local stack (GPU box or CPU-only box), no external APIs:
#   a tool-capable coding model (llama.cpp, GPU or CPU)  ──tools──▶  claymore  ──fan-out──▶  bounded-expert spokes
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
NGL=${NGL:-99}   # GPU layers to offload. Harmless on a CPU box (llama.cpp falls back to CPU); set NGL=0 to be explicit.
# Model: by default auto-download from Hugging Face (-hf, cached under ~/.cache/llama.cpp on first run, ~4.7 GB).
# Use bartowski's SINGLE-FILE GGUFs: the official Qwen *-GGUF repos ship SPLIT quants that -hf can fetch
# incompletely, so the model loads and runs fast but emits garbage. Q4_K_M runs well on an 8 GB GPU at -ngl 99.
# To use a local .gguf instead, set MODEL=/path/to/model.gguf ; or pick a different repo with HF_REPO=... .
# Smaller/comfier option for tighter VRAM:  HF_REPO=bartowski/Qwen2.5-Coder-3B-Instruct-GGUF:Q4_K_M
HF_REPO=${HF_REPO:-bartowski/Qwen2.5-Coder-7B-Instruct-GGUF:Q4_K_M}
MODEL=${MODEL:-}
SG=${SG:-$HOME/code/sgiandubh}
HERE="$(cd "$(dirname "$0")/.." && pwd)"

# Stop any previous stack first — a leftover llama-server hogs the GPU (→ OOM on the new one) and old spokes hold the
# ports. (|| true so set -e doesn't abort when there's nothing to kill.)
pkill -f "build/bin/llama-server" 2>/dev/null || true
pkill -f "build/sgiandubh"        2>/dev/null || true
sleep 1

if [ -n "$MODEL" ]; then SRC=(-m "$MODEL"); else SRC=(-hf "$HF_REPO"); fi
echo "1/3 · coding model (-ngl $NGL requested · --jinja tool-calling) → :8080   [${MODEL:-$HF_REPO}]"
"$LLAMA" "${SRC[@]}" -ngl "$NGL" -c 8192 --jinja --host 127.0.0.1 --port 8080 >/tmp/llama.log 2>&1 &
echo "2/3 · bounded-expert spokes → :8081 (riscv), :8082 (logic)"
"$SG/build/sgiandubh" "$SG/package_riscv" 8081 --answer-from-corpus >/tmp/spoke_riscv.log 2>&1 &
"$SG/build/sgiandubh" "$SG/package_logic" 8082 --answer-from-corpus >/tmp/spoke_logic.log 2>&1 &

# wait for the model to load (the slow one) + the spokes
for url in localhost:8080/v1/models localhost:8081/v1/models localhost:8082/v1/models; do
  until curl -s --max-time 3 "$url" >/dev/null 2>&1; do sleep 2; done
done

# Report ACTUAL model placement — never assume GPU. The build may be CPU/Vulkan/CUDA and -ngl silently falls back to
# CPU when there's no GPU (e.g. the big CPU box). Read the truth from the load log rather than claiming it.
off=$(grep -ioE "offloaded [0-9]+/[0-9]+ layers to GPU" /tmp/llama.log 2>/dev/null | tail -1)
bk=$(grep -ioE "(CUDA|Vulkan|Metal|ROCm|SYCL)" /tmp/llama.log 2>/dev/null | head -1 | tr 'a-z' 'A-Z')
if [ -n "$off" ]; then echo "      → model placement: ${off}${bk:+ [$bk]}"
else echo "      → model placement: CPU (no GPU offload)"; fi

if [ "$CLI" = 1 ]; then
  echo "3/3 · claymore CLI (tools mode) — type a query; blank line or Ctrl-D to exit"
  exec "$HERE/build/claymore" "$HERE/examples/local.spokes.json" --repl ${VERBOSE:+--verbose}
else
  echo "3/3 · claymore hub (tools mode) → :9000   ·   point any OpenAI client at http://localhost:9000/v1"
  exec "$HERE/build/claymore" "$HERE/examples/local.spokes.json" 9000 ${VERBOSE:+--verbose}
fi
