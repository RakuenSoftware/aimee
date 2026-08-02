#!/bin/bash
# ABLATION: same models, same gold set, one literal changed in the prompt.
#
# The main sweep found every small model emitting "confidence":0.0 — copied from
# the schema example in MF_SYSTEM_PROMPT_TMPL — which MF_CONF_FLOOR (0.6) then
# discards, so the drain commits nothing. This run raises that example value to
# 0.9 and changes nothing else, to separate "the model cannot extract" from "the
# prompt taught it to emit a confidence we then throw away".
#
# If accuracy jumps here, the fix is a one-line prompt change, not a different
# model. Results are written to results/ablation-conf and must never be reported
# as production numbers.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
OUT=results/ablation-conf
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}

MODELS="
ibm-granite/granite-4.0-350m
Qwen/Qwen3-0.6B
Qwen/Qwen3-1.7B
LiquidAI/LFM2-350M-Extract
google/gemma-3n-E4B-it
"

for M in $MODELS; do
  SLUG=$(echo "$M" | tr '/' '_')
  PRED="$OUT/$SLUG.pred.jsonl"
  LOG="$OUT/$SLUG.log"
  [ -s "$PRED" ] && { echo "SKIP $M"; continue; }
  echo "=== RUN $M (conf-fixed prompt) ==="
  EXTRA=""
  [ "$M" = "ibm-granite/granite-4.0-350m" ] && EXTRA="--no-kv-cache"
  if $PY harness/run_hf.py --model "$M" --gold data/gold.jsonl --out "$PRED" \
       --conf-fixed-prompt $EXTRA >"$LOG" 2>&1; then
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
        --json-out "$OUT/$SLUG.score.json" >/dev/null 2>>"$LOG" \
      && $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
        --pred-key pred_nofloor --json-out "$OUT/$SLUG.score.nofloor.json" \
        >/dev/null 2>>"$LOG" \
      && echo "OK   $M" || echo "SCOREFAIL $M"
  else
    echo "FAIL $M -> $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-200)"
    rm -f "$PRED"
  fi
done
echo "SWEEP_ABLATION_DONE"
