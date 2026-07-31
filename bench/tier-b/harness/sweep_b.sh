#!/bin/bash
# Tier-B synthesize across the model ladder.
#
# E4B runs first and twice — once on GPU for quality, once on CPU for the number
# that decides whether a local install is worth offering. The rest follow on GPU
# so the ladder is comparable to Tier-A's.
#
# Tier-B does not set disable_thinking (kb_curator_provider_for_stage applies it
# to Tier-A only), so thinking is left on, matching production.
#
# Usage: sweep_b.sh [gpu|cpu]
set -u
cd "$(dirname "$0")/.."
MODE=${1:-gpu}
PY=${PY:-/opt/bench/bin/python}
OUT="results/$MODE"
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8095}

if [ "$MODE" = "cpu" ]; then
  SERVER=${SERVER:-/opt/llama.cpp/build/bin/llama-server}
  THREADS=${THREADS:-20}
  PLACE="-ngl 0 -t $THREADS"
  MODELS=("gemma-4-E4B-it|unsloth/gemma-4-E4B-it-GGUF:Q8_0|")
else
  SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
  PLACE=""
  MODELS=(
    "gemma-4-E4B-it|unsloth/gemma-4-E4B-it-GGUF:Q8_0|"
    "gemma-4-E2B-it|ggml-org/gemma-4-E2B-it-GGUF:Q8_0|"
    "gemma-4-26B-A4B-it|unsloth/gemma-4-26B-A4B-it-GGUF:Q8_0|-ot .ffn_.*_exps.=CPU"
    "gemma-4-12B-it|unsloth/gemma-4-12B-it-GGUF:Q8_0|"
    "Qwen3.6-27B|unsloth/Qwen3.6-27B-GGUF:Q8_0|"
    "Qwen3.6-35B-A3B|unsloth/Qwen3.6-35B-A3B-GGUF:Q8_0|-ot .ffn_.*_exps.=CPU"
    "granite-4.1-3b|ibm-granite/granite-4.1-3b-GGUF:Q8_0|"
    "granite-4.0-1b|ibm-granite/granite-4.0-1b-GGUF:Q8_0|"
    "Qwen3.5-0.8B|ggml-org/Qwen3.5-0.8B-GGUF:Q8_0|"
  )
fi

for entry in "${MODELS[@]}"; do
  IFS='|' read -r LABEL REPO EXTRA <<<"$entry"
  PRED="$OUT/$LABEL.pred.jsonl"; LOG="$OUT/$LABEL.server.log"
  [ -s "$PRED" ] && { echo "SKIP $LABEL"; continue; }
  echo "=== SERVE $LABEL ($MODE) ==="
  # shellcheck disable=SC2086
  $SERVER -hf "$REPO" --port "$PORT" -c 8192 --no-webui --no-mmproj $PLACE $EXTRA \
      >"$LOG" 2>&1 &
  SRV=$!
  ready=0
  for _ in $(seq 1 240); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
    kill -0 $SRV 2>/dev/null || break
    sleep 15
  done
  if [ "$ready" = 1 ]; then
    if $PY harness/run_b.py --model "$LABEL" --topics data/topics.jsonl \
         --out "$PRED" --base-url "http://127.0.0.1:$PORT" >>"$LOG" 2>&1; then
      $PY harness/score_b.py --topics data/topics.jsonl --pred "$PRED" \
          --json-out "$OUT/$LABEL.score.json" >/dev/null 2>>"$LOG"
      echo "OK   $LABEL"
    else
      echo "FAIL $LABEL -> $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-180)"
      rm -f "$PRED"
    fi
  else
    echo "FAIL $LABEL -> server never healthy: $(tail -2 "$LOG" | tr '\n' ' ' | cut -c1-160)"
  fi
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  sleep 5
  [ -x ../tier-a/harness/prune_models.sh ] && \
    KEEP="$REPO" HF_HOME="$HF_HOME" bash ../tier-a/harness/prune_models.sh 2>/dev/null | tail -1
done
echo "SWEEP_B_${MODE}_DONE"
