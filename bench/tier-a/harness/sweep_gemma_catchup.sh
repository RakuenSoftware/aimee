#!/bin/bash
# Catch-up sweep for the Gemma models, including the E4B incumbent baseline.
#
# google/gemma-3-270m-it and google/gemma-3n-E4B-it are gated on the Hub and this
# environment has no HF token, so the main sweep could not fetch them. The unsloth
# mirrors carry the same weights under the same Gemma licence and are ungated.
# The mirror is recorded in the results so the provenance is not lost — nothing
# here changes the licence position, which is that Gemma is not Apache/MIT and so
# cannot ship in the redistributed image regardless of how it was obtained.
#
# Runs both the production prompt and the confidence ablation, because the
# central question is whether E4B avoids the confidence-floor collapse that every
# small model fell into.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
export HF_HOME=${HF_HOME:-/opt/hf}

MODELS="
unsloth/gemma-3-270m-it
unsloth/gemma-3n-E4B-it
"

run_one() {  # $1=model $2=outdir $3=extra flags
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
echo "SWEEP_GEMMA_DONE"
