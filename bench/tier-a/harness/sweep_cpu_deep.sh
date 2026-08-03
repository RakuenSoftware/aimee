#!/bin/bash
# Can Tier-A actually run on CPU? A proper look, not one configuration.
#
# The first CPU pass measured exactly one setup: Q8_0, 8 threads, one request at
# a time, re-processing the whole prompt on every call. That produced 18s a note
# for E2B and became "CPU is not viable". A drain does not care about
# single-request latency, it cares about notes per hour, and three levers were
# never tested:
#
#   quant     Q4_K_M streams roughly half the bytes of Q8_0, and CPU generation
#             is bandwidth-bound. The quality cost is unmeasured — Q8_0 was
#             byte-identical to bf16 on this task, Q4 will not be, so anything
#             promising here has to be re-scored for accuracy before it counts.
#   threads   8 was the container's pinned set, not a property of the task. A
#             laptop has 8-16, a home server more.
#   prefix    ~250 of the ~400 prompt tokens are the system prompt, byte-identical
#             on every call. Approximated here by benchmarking a 150-token prompt
#             beside the 400-token one; a real cache hit should land between them.
#
# Concurrency is measured separately by sweep_cpu_throughput.sh, because
# llama-bench cannot express parallel sequences and that is the lever most likely
# to matter for a queue.
set -u
cd "$(dirname "$0")/.."
BENCH=${BENCH:-/opt/llama.cpp/build/bin/llama-bench}
OUT=results/cpu-deep
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}

MODELS="
ggml-org/gemma-4-E2B-it-GGUF
ibm-granite/granite-4.0-1b-GGUF
"
QUANTS="Q8_0 Q4_K_M"
THREADS="8 20"

echo "host $(hostname) $(date -u +%FT%TZ)"
for M in $MODELS; do
  for Q in $QUANTS; do
    for T in $THREADS; do
      SLUG=$(echo "${M}_${Q}_t${T}" | tr '/:.' '____')
      JSON="$OUT/$SLUG.json"
      [ -s "$JSON" ] && { echo "SKIP $M $Q t=$T"; continue; }
      echo "=== $M $Q threads=$T ==="
      if taskset -c "0-$((T-1))" "$BENCH" -hf "$M:$Q" -t "$T" -p 400 -p 150 -n 48 -r 3 \
           -o json > "$JSON" 2>"$OUT/$SLUG.log"; then
        echo "OK   $M $Q t=$T"
      else
        echo "FAIL $M $Q t=$T -> $(tail -2 "$OUT/$SLUG.log" | tr '\n' ' ' | cut -c1-140)"
        rm -f "$JSON"
      fi
    done
  done
done
echo "SWEEP_CPU_DEEP_DONE"
