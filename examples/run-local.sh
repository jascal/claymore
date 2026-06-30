#!/usr/bin/env bash
# Bring up the FULL local stack (GPU box or CPU-only box), no external APIs:
#   a tool-capable coding model (llama.cpp, GPU or CPU)  ──tools──▶  claymore  ──fan-out──▶  bounded-expert spokes
# Everything talks the OpenAI API; the model calls the experts as tools, claymore runs them, the model finalizes.
#
# The stack shows ALL THREE layers of the design, not just content:
#   • CONTENT  — bounded experts (riscv :8081, logic :8082)            ← sgiandubh, the answers
#   • LIBRARY  — a librarian :8200 (role:"librarian")                  ← a model-free CATALOG over the content experts
#   • PEDAGOGY — tutors :8300 (role:"pedagogy", model-free prompts)    ← teaching templates that run a session
# The librarian + tutors are MODEL-FREE experts built by rosetta (a sibling checkout). With them up, the REPL can
# `/catalog`, `/tutors`, and enter a `/session <tutor> on <expert>` (see --cli below). They are OPTIONAL: if rosetta
# isn't present the script still runs the content stack and just notes that the catalog/tutoring layers are disabled.
#
# Edit the paths/model below (or override via env), then:
#   ./examples/run-local.sh            → claymore hub as an OpenAI server on :9000
#   ./examples/run-local.sh --cli      → claymore CLI/REPL instead (type queries; /tutors, /session, /catalog, /experts)
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
ROSETTA=${ROSETTA:-$HOME/code/rosetta}     # the MODEL-FREE expert builder (librarian + tutors). Optional.
HERE="$(cd "$(dirname "$0")/.." && pwd)"
LIBPKG="$HERE/build/demo-librarian"        # librarian package, built fresh over the content experts (gitignored)

# Stop any previous stack first — a leftover llama-server hogs the GPU (→ OOM on the new one) and old spokes hold the
# ports. (|| true so set -e doesn't abort when there's nothing to kill.)
pkill -f "build/bin/llama-server" 2>/dev/null || true
pkill -f "build/sgiandubh"        2>/dev/null || true
pkill -f "build/claymore"         2>/dev/null || true
sleep 1

if [ -n "$MODEL" ]; then SRC=(-m "$MODEL"); else SRC=(-hf "$HF_REPO"); fi
echo "1/4 · coding model (-ngl $NGL requested · --jinja tool-calling) → :8080   [${MODEL:-$HF_REPO}]"
"$LLAMA" "${SRC[@]}" -ngl "$NGL" -c 8192 --jinja --host 127.0.0.1 --port 8080 >/tmp/llama.log 2>&1 &

echo "2/4 · content experts → :8081 (riscv), :8082 (logic)"
"$SG/build/sgiandubh" "$SG/package_riscv" 8081 --answer-from-corpus >/tmp/spoke_riscv.log 2>&1 &
"$SG/build/sgiandubh" "$SG/package_logic" 8082 --answer-from-corpus >/tmp/spoke_logic.log 2>&1 &

# ---- model-free experts (librarian + tutors), built by rosetta — OPTIONAL --------------------------------------------
# The librarian is a CATALOG over the content experts (cross-document directory); the tutors expert is the prebuilt
# pedagogy template pack. Both are model-free (no GPU, no GloVe — lexical grounding). They wait on the same OpenAI API.
WAIT_URLS=(localhost:8080/v1/models localhost:8081/v1/models localhost:8082/v1/models)
MODELFREE=0
ROSETTA_PY="$ROSETTA/.venv/bin/python"
if [ -x "$ROSETTA_PY" ] && [ -d "$ROSETTA/examples/pedagogy/package" ]; then
  echo "3/4 · model-free experts → :8200 (library, a librarian), :8300 (tutors, pedagogy)"
  # Build the librarian catalog over THIS demo's content experts (expert-level cards → lib:<name> handles, dim=0).
  # NOTE: these cards are hand-authored for the demo — if you change the content experts above, keep them in sync.
  # (A real librarian indexes documents via rosetta adapters rather than hand cards; see rosetta build_librarian.)
  ( cd "$ROSETTA" && PYTHONPATH=py "$ROSETTA_PY" - "$LIBPKG" >/tmp/build_librarian.log 2>&1 <<'PY'
import sys
from pack.build import build_librarian
cards = [
    {"handle": "riscv", "title": "RISC-V ISA",
     "summary": "The RISC-V instruction set architecture specification: base integer ISA and standard extensions.",
     "sections": ["base integer", "extensions", "privileged", "instructions"]},
    {"handle": "logic", "title": "Formal Logic",
     "summary": "Propositional and predicate logic: connectives, quantifiers, inference and proof rules.",
     "sections": ["propositional", "predicate", "proofs"]},
]
build_librarian(sys.argv[1], documents=[], extra_cards=cards, target="", dim=0, label="expert")
PY
  ) && {
    "$SG/build/sgiandubh" "$LIBPKG"                          8200 --answer-from-corpus >/tmp/spoke_library.log 2>&1 &
    "$SG/build/sgiandubh" "$ROSETTA/examples/pedagogy/package" 8300 --answer-from-corpus >/tmp/spoke_tutors.log 2>&1 &
    WAIT_URLS+=(localhost:8200/v1/models localhost:8300/v1/models)
    MODELFREE=1
  } || echo "      ! librarian build failed (see /tmp/build_librarian.log) — continuing without the catalog/tutor layers"
else
  echo "3/4 · model-free experts SKIPPED — rosetta not found at \$ROSETTA ($ROSETTA)"
  echo "      clone https://github.com/jascal/rosetta there (with a .venv) to enable the librarian + tutors."
fi

# wait for the model to load (the slow one) + the spokes that were started — BOUNDED, so a hung/slow box fails loudly
# instead of spinning forever. First-run model download can be large, so the cap is generous + overridable.
WAIT_MAX_SECS=${WAIT_MAX_SECS:-1800}
for url in "${WAIT_URLS[@]}"; do
  waited=0
  until curl -s --max-time 3 "$url" >/dev/null 2>&1; do
    sleep 2; waited=$((waited + 2))
    if [ "$waited" -ge "$WAIT_MAX_SECS" ]; then
      echo "      ! timed out after ${WAIT_MAX_SECS}s waiting for $url — check /tmp/llama.log and /tmp/spoke_*.log" >&2
      exit 1
    fi
    [ $((waited % 30)) -eq 0 ] && echo "      … still waiting for $url (${waited}s; override the cap with WAIT_MAX_SECS=)"
  done
done

# Report ACTUAL model placement — never assume GPU. The build may be CPU/Vulkan/CUDA and -ngl silently falls back to
# CPU when there's no GPU (e.g. the big CPU box). Read the truth from the load log rather than claiming it.
off=$(grep -ioE "offloaded [0-9]+/[0-9]+ layers to GPU" /tmp/llama.log 2>/dev/null | tail -1)
bk=$(grep -ioE "(CUDA|Vulkan|Metal|ROCm|SYCL)" /tmp/llama.log 2>/dev/null | head -1 | tr 'a-z' 'A-Z')
if [ -n "$off" ]; then echo "      → model placement: ${off}${bk:+ [$bk]}"
else echo "      → model placement: CPU (no GPU offload)"; fi
[ "$MODELFREE" = 1 ] && echo "      → layers up: content (riscv, logic) + library (librarian) + tutors (pedagogy)" \
                     || echo "      → layers up: content only (riscv, logic)"
echo "      → tear it all down later with:  pkill -f 'llama-server|build/sgiandubh|build/claymore'"

if [ "$CLI" = 1 ]; then
  echo "4/4 · claymore CLI — content Q&A by default; /tutors · /experts · /session <tutor> on <expert> · /catalog"
  exec "$HERE/build/claymore" "$HERE/examples/local.spokes.json" --repl ${VERBOSE:+--verbose}
else
  echo "4/4 · claymore hub (tools mode) → :9000   ·   point any OpenAI client at http://localhost:9000/v1"
  exec "$HERE/build/claymore" "$HERE/examples/local.spokes.json" 9000 ${VERBOSE:+--verbose}
fi
