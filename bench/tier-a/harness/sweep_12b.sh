#!/bin/bash
# Gemma 4 12B on the 5080 — a ceiling probe, not a deployment candidate.
#
# 12B in bf16 is ~24GB against 15.5GB of VRAM, so this runs with device_map=auto
# and accelerate offloads the overflow to CPU. That keeps the weights at full
# precision (no quantisation confound in the accuracy numbers) at the cost of
# latency: the ms/note figures from this run are NOT comparable to the
# fully-resident models and should not appear in a speed table.
#
# The question is what the task's ceiling looks like. If 12B lands near E4B's
# 0.705, the remaining error is the gold set and the task definition rather than
# model capacity, and buying a bigger model is not the lever.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
OUT=results/ceiling
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}

MODELS="google/gemma-4-12B-it"

for M in $MODELS; do
  SLUG=$(echo "$M" | tr '/' '_')
  PRED="$OUT/$SLUG.pred.jsonl"
  [ -s "$PRED" ] && { echo "SKIP $M"; continue; }
  echo "=== RUN $M (offloaded) ==="
  if $PY harness/run_hf.py --model "$M" --gold data/gold.jsonl --out "$PRED" \
       --device auto >"$OUT/$SLUG.log" 2>&1; then
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
        --json-out "$OUT/$SLUG.score.json" >/dev/null 2>>"$OUT/$SLUG.log"
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" --no-alias \
        --json-out "$OUT/$SLUG.score.noalias.json" >/dev/null 2>>"$OUT/$SLUG.log"
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" --pred-key pred_nofloor \
        --json-out "$OUT/$SLUG.score.nofloor.json" >/dev/null 2>>"$OUT/$SLUG.log"
    echo "OK   $M"
  else
    echo "FAIL $M -> $(tail -3 "$OUT/$SLUG.log" | tr '\n' ' ' | cut -c1-200)"
    rm -f "$PRED"
  fi
done
echo "SWEEP_12B_DONE"
