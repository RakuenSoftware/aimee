#!/bin/bash
# Second-pass sweep against the CURRENT model field (verified on the Hub
# 2026-07-31). The first pass was built from a stale candidate list: it baselined
# Gemma 3n E4B when the incumbent is Gemma 4 E4B, and it treated the Gemma family
# as licence-blocked when Gemma 4 shipped Apache-2.0 on 2026-04-02.
#
# Runs the production prompt and the confidence ablation for each model, since
# the first pass showed the MF_CONF_FLOOR interaction dominates the production
# numbers for small models.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
export HF_HOME=${HF_HOME:-/opt/hf}

MODELS="
google/gemma-4-E4B-it
google/gemma-4-E2B-it
Qwen/Qwen3.5-0.8B
Qwen/Qwen3.5-2B
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
echo "SWEEP_GEN2_DONE"
