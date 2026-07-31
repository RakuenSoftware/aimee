#!/bin/bash
# Second-pass, batch C: newest Granite.
#
# Granite 4.1 exists but its smallest member is 3B -- IBM did not refresh the
# 350m/1b nano class, so granite-4.0-350m and granite-4.0-1b in batches A/B are
# the current models at their sizes, not stale picks. granite-4.1-3b is included
# because it is the newest Granite and 3B is near the top of the accepted size
# range. There is no Granite 4.2.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
export HF_HOME=${HF_HOME:-/opt/hf}

MODELS="ibm-granite/granite-4.1-3b"

run_one() {
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
echo "SWEEP_GEN2C_DONE"
