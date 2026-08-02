#!/bin/bash
# E4B on CPU for Tier-B, at Tier-B's actual token shapes.
#
# Tier-B is nothing like Tier-A in token profile, so the Tier-A CPU numbers say
# nothing about it. Two shapes, both read from the code:
#
#   judge       kb_curator_judge.c: {task, mention:{name,context}, candidate:
#               {name}, score} in, {"same_entity":true|false} out. Small prompt,
#               a handful of tokens generated. Approximated at p=200 n=16.
#
#   synthesize  kb_curator_synthesize.c: {task, topic, sources:[...]} where
#               CURATOR_SYNTH_DEFAULT_K = 8 artifact payloads are inlined, and
#               the model writes a grounded paragraph. Large prefill, real
#               generation. Approximated at p=2000 n=300, and p=4000 for a
#               heavier topic.
#
# The volume difference is the point. Tier-A runs on every note forever; Tier-B
# runs on entity promotion and topic synthesis, which is orders of magnitude
# rarer. A per-call cost that is fatal for Tier-A may be entirely acceptable
# here, and that is what this measures.
set -u
cd "$(dirname "$0")/.."
BENCH=${BENCH:-/opt/llama.cpp/build/bin/llama-bench}
OUT=results/cpu-tierb
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}

MODEL="unsloth/gemma-4-E4B-it-GGUF:Q8_0"
THREADS="8 20"

echo "host $(hostname) $(date -u +%FT%TZ)  model $MODEL"
for T in $THREADS; do
  SLUG="gemma-4-E4B-it_Q8_0_t${T}"
  JSON="$OUT/$SLUG.json"
  [ -s "$JSON" ] && { echo "SKIP t=$T"; continue; }
  echo "=== E4B CPU threads=$T (judge and synthesize shapes) ==="
  # p200/n16 = judge; p2000/n300 and p4000/n300 = synthesize.
  if taskset -c "0-$((T-1))" "$BENCH" -hf "$MODEL" -t "$T" \
       -p 200 -p 2000 -p 4000 -n 16 -n 300 -r 3 -o json \
       > "$JSON" 2>"$OUT/$SLUG.log"; then
    echo "OK   t=$T"
  else
    echo "FAIL t=$T -> $(tail -2 "$OUT/$SLUG.log" | tr '\n' ' ' | cut -c1-160)"
    rm -f "$JSON"
  fi
done
echo "SWEEP_CPU_TIERB_DONE"
