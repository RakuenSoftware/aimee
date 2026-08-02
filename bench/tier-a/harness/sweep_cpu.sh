#!/bin/bash
# CPU speed sweep via llama.cpp.
#
# Run this ONLY when the GPU sweep is finished. The bench CT and the GPU CT share
# one i7-14700K, so concurrent work makes these numbers meaningless.
#
# Pinned to a fixed cpuset because the 14700K is a hybrid P/E-core part: without
# pinning the scheduler can land a run on efficiency cores and halve throughput
# for reasons that have nothing to do with the model.
#
# Q8_0 throughout. Tiny models degrade disproportionately under 4-bit, so Q8_0
# keeps the speed comparison from being contaminated by a quality cliff that the
# accuracy sweep (bf16) would not have seen.
set -u
cd "$(dirname "$0")/.."
BENCH=${BENCH:-/opt/llama.cpp/build/bin/llama-bench}
OUT=results/cpu
mkdir -p "$OUT"
THREADS=${THREADS:-8}
CPUSET=${CPUSET:-0-7}
# Prompt ~400 tokens (system prompt is ~330 + the note); generation 64 covers the
# observed completion length for this schema with headroom.
PP=${PP:-400}
TG=${TG:-64}

# repo:quant pairs resolved by llama-bench -hf.
MODELS="
ibm-granite/granite-4.0-350m-GGUF:Q8_0
Qwen/Qwen3-0.6B-GGUF:Q8_0
HuggingFaceTB/SmolLM2-360M-Instruct-GGUF:Q8_0
Qwen/Qwen3-1.7B-GGUF:Q8_0
LiquidAI/LFM2-350M-Extract-GGUF:Q8_0
unsloth/LFM2.5-230M-GGUF:Q8_0
ggml-org/gemma-3-270m-GGUF:Q8_0
"

echo "host: $(hostname)  threads=$THREADS cpuset=$CPUSET  $(date -u +%FT%TZ)"
for M in $MODELS; do
  SLUG=$(echo "$M" | tr '/:' '__')
  JSON="$OUT/$SLUG.json"
  if [ -s "$JSON" ]; then echo "SKIP $M"; continue; fi
  echo "=== RUN $M ==="
  if taskset -c "$CPUSET" "$BENCH" -hf "$M" -t "$THREADS" -p "$PP" -n "$TG" -r 3 \
       -o json > "$JSON" 2>"$OUT/$SLUG.log"; then
    echo "OK   $M"
  else
    echo "FAIL $M -> $(tail -2 "$OUT/$SLUG.log" | tr '\n' ' ' | cut -c1-180)"
    rm -f "$JSON"
  fi
done
echo "SWEEP_CPU_DONE"
