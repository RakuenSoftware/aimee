#!/bin/bash
# GPU accuracy sweep. Each model is independent: a failure (gated repo, OOM,
# unsupported architecture) is recorded and the sweep continues, so one bad model
# does not cost the whole run.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
OUT=results/gpu
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}

MODELS="
ibm-granite/granite-4.0-350m
ibm-granite/granite-4.0-h-350m
Qwen/Qwen3-0.6B
HuggingFaceTB/SmolLM2-360M-Instruct
Qwen/Qwen3-1.7B
LiquidAI/LFM2-350M-Extract
LiquidAI/LFM2.5-230M
google/gemma-3-270m-it
google/gemma-3n-E4B-it
"

for M in $MODELS; do
  SLUG=$(echo "$M" | tr '/' '_')
  PRED="$OUT/$SLUG.pred.jsonl"
  LOG="$OUT/$SLUG.log"
  if [ -s "$PRED" ]; then echo "SKIP $M (predictions exist)"; continue; fi
  echo "=== RUN $M ==="
  EXTRA=""
  # granite-4.0-350m: transformers picks a hybrid Mamba cache this non-hybrid
  # checkpoint cannot satisfy, so generate() raises. Same outputs without it.
  [ "$M" = "ibm-granite/granite-4.0-350m" ] && EXTRA="--no-kv-cache"
  if $PY harness/run_hf.py --model "$M" --gold data/gold.jsonl --out "$PRED" \
       $EXTRA >"$LOG" 2>&1; then
    # Two views of the same run: what production commits, and the same
    # extraction with MF_CONF_FLOOR lifted.
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
        --json-out "$OUT/$SLUG.score.json" >/dev/null 2>>"$LOG" \
      && $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
        --pred-key pred_nofloor \
        --json-out "$OUT/$SLUG.score.nofloor.json" >/dev/null 2>>"$LOG" \
      && echo "OK   $M" || echo "SCOREFAIL $M"
  else
    echo "FAIL $M -> $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-200)"
    rm -f "$PRED"
  fi
done
echo "SWEEP_GPU_DONE"
