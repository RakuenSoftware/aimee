#!/bin/bash
# CPU speed for the current-generation models. Same protocol as sweep_cpu.sh:
# Q8_0, pinned cores, run only when nothing else is on the box.
set -u
cd "$(dirname "$0")/.."
BENCH=${BENCH:-/opt/llama.cpp/build/bin/llama-bench}
OUT=results/cpu
mkdir -p "$OUT"
THREADS=${THREADS:-8}
CPUSET=${CPUSET:-0-7}
PP=${PP:-400}
TG=${TG:-64}

MODELS="
ggml-org/gemma-4-E2B-it-GGUF:Q8_0
ggml-org/gemma-4-E4B-it-GGUF:Q8_0
ggml-org/Qwen3.5-0.8B-GGUF:Q8_0
ggml-org/Qwen3.5-2B-GGUF:Q8_0
ibm-granite/granite-4.0-1b-GGUF:Q8_0
ibm-granite/granite-4.0-h-1b-GGUF:Q8_0
"

echo "host: $(hostname) threads=$THREADS cpuset=$CPUSET $(date -u +%FT%TZ)"
for M in $MODELS; do
  SLUG=$(echo "$M" | tr '/:' '__')
  JSON="$OUT/$SLUG.json"
  [ -s "$JSON" ] && { echo "SKIP $M"; continue; }
  echo "=== RUN $M ==="
  if taskset -c "$CPUSET" "$BENCH" -hf "$M" -t "$THREADS" -p "$PP" -n "$TG" -r 3 \
       -o json > "$JSON" 2>"$OUT/$SLUG.log"; then
    echo "OK   $M"
  else
    echo "FAIL $M -> $(tail -2 "$OUT/$SLUG.log" | tr '\n' ' ' | cut -c1-180)"
    rm -f "$JSON"
  fi
done
echo "SWEEP_CPU_GEN2_DONE"
