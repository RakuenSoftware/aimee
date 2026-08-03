#!/bin/bash
# CPU speed for batch C. Same protocol as the other CPU passes.
set -u
cd "$(dirname "$0")/.."
BENCH=${BENCH:-/opt/llama.cpp/build/bin/llama-bench}
OUT=results/cpu
mkdir -p "$OUT"
THREADS=${THREADS:-8}; CPUSET=${CPUSET:-0-7}; PP=${PP:-400}; TG=${TG:-64}

MODELS="ibm-granite/granite-4.1-3b-GGUF:Q8_0"

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
echo "SWEEP_CPU_GEN2C_DONE"
