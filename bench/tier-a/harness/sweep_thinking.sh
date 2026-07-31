#!/bin/bash
# Does disable_thinking cost Tier-A anything?
#
# Production sets it, on the premise that extraction is mechanical. The failure
# modes that actually separate models on this benchmark are negation ("I no
# longer work there"), implicit inference, and restraint on factless notes —
# none of which look mechanical. This runs the same models with reasoning
# enabled, everything else identical.
#
# Token cap raised to 2048: the proposal's §1 records a real incident where the
# thinking pass consumed the completion budget before the JSON, committing zero
# facts. If that recurs here it should show as truncation, not as a mystery.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=results/thinking
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8087}

MODELS=(
  "gemma-4-E4B-it|unsloth/gemma-4-E4B-it-GGUF:Q8_0|"
  "gemma-4-26B-A4B-it|unsloth/gemma-4-26B-A4B-it-GGUF:Q8_0|-ot .ffn_.*_exps.=CPU"
  "Qwen3.5-2B|unsloth/Qwen3.5-2B-GGUF:Q8_0|"
)

for entry in "${MODELS[@]}"; do
  IFS='|' read -r LABEL REPO EXTRA <<<"$entry"
  PRED="$OUT/$LABEL.pred.jsonl"; LOG="$OUT/$LABEL.server.log"
  [ -s "$PRED" ] && { echo "SKIP $LABEL"; continue; }
  echo "=== SERVE $LABEL (thinking enabled) ==="
  # shellcheck disable=SC2086
  $SERVER -hf "$REPO" --port "$PORT" -c 8192 --no-webui $EXTRA >"$LOG" 2>&1 &
  SRV=$!
  ready=0
  for _ in $(seq 1 240); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
    kill -0 $SRV 2>/dev/null || break
    sleep 15
  done
  if [ "$ready" = 1 ]; then
    if $PY harness/run_llamacpp.py --model "$LABEL" --gold data/gold.jsonl \
         --out "$PRED" --base-url "http://127.0.0.1:$PORT" \
         --thinking --max-tokens 2048 >>"$LOG" 2>&1; then
      $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
          --json-out "$OUT/$LABEL.score.json" >/dev/null 2>>"$LOG"
      echo "OK   $LABEL"
    else
      echo "FAIL $LABEL"; rm -f "$PRED"
    fi
  else
    echo "FAIL $LABEL -> server never healthy"
  fi
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  sleep 5
  KEEP='' HF_HOME="$HF_HOME" bash harness/prune_models.sh 2>/dev/null | tail -1
done
echo "SWEEP_THINKING_DONE"
