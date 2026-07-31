#!/bin/bash
# Second-pass, batch B: the 1B bracket, added once ~1B was accepted as viable if
# it earns its keep. Qwen3.5 has no 1B (it goes 0.8B -> 2B, both already in batch
# A), so Granite is what actually occupies this size class in Apache-2.0.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
export HF_HOME=${HF_HOME:-/opt/hf}

MODELS="
ibm-granite/granite-4.0-1b
ibm-granite/granite-4.0-h-1b
"

run_one() {  # $1=model $2=outdir $3=extra
  local M=$1 OUT=$2 EXTRA=${3:-}
  local SLUG=$(echo "$M" | tr '/' '_')
  local PRED="$OUT/$SLUG.pred.jsonl" LOG="$OUT/$SLUG.log"
  mkdir -p "$OUT"
  [ -s "$PRED" ] && { echo "SKIP $M ($OUT)"; return; }
  echo "=== RUN $M -> $OUT ==="
  if $PY harness/run_hf.py --model "$M" --gold data/gold.jsonl --out "$PRED" \
       $EXTRA >"$LOG" 2>&1; then
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
        --json-out "$OUT/$SLUG.score.json" >/dev/null 2>>"$LOG"
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" --pred-key pred_nofloor \
        --json-out "$OUT/$SLUG.score.nofloor.json" >/dev/null 2>>"$LOG"
    echo "OK   $M ($OUT)"
  else
    echo "FAIL $M ($OUT) -> $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-200)"
    rm -f "$PRED"
  fi
}

for M in $MODELS; do
  run_one "$M" results/gpu
  run_one "$M" results/ablation-conf --conf-fixed-prompt
done
echo "SWEEP_GEN2B_DONE"
