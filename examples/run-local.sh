#!/usr/bin/env bash
# Bring up the FULL local stack on a GPU box, no external APIs:
#   a tool-capable coding model (llama.cpp, on the GPU)  ──tools──▶  claymore  ──fan-out──▶  bounded-expert spokes
# Everything talks the OpenAI API; the model calls the experts as tools, claymore runs them, the model finalizes.
#
# Edit the paths/model below (or override via env), then: ./examples/run-local.sh
set -e
LLAMA=${LLAMA:-$HOME/code/llama.cpp/build/bin/llama-server}
MODEL=${MODEL:-$HOME/code/llama.cpp/models/qwen2.5-coder-7b-instruct-q4_k_m.gguf}
SG=${SG:-$HOME/code/sgiandubh}
HERE="$(cd "$(dirname "$0")/.." && pwd)"

echo "1/3 · coding model on the GPU (tool-calling via --jinja, default on) → :8080"
"$LLAMA" -m "$MODEL" -ngl 99 -c 8192 --host 127.0.0.1 --port 8080 >/tmp/llama.log 2>&1 &
echo "2/3 · bounded-expert spokes → :8081 (riscv), :8082 (logic)"
"$SG/build/sgiandubh" "$SG/package_riscv" 8081 --answer-from-corpus >/tmp/spoke_riscv.log 2>&1 &
"$SG/build/sgiandubh" "$SG/package_logic" 8082 --answer-from-corpus >/tmp/spoke_logic.log 2>&1 &

# wait for the model to load (the slow one) + the spokes
for url in localhost:8080/v1/models localhost:8081/v1/models localhost:8082/v1/models; do
  until curl -s --max-time 3 "$url" >/dev/null 2>&1; do sleep 2; done
done
echo "3/3 · claymore hub (tools mode) → :9000   ·   point any OpenAI client at http://localhost:9000/v1"
exec "$HERE/build/claymore" "$HERE/examples/local.spokes.json" 9000
