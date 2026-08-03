#!/bin/bash
# What does Q4_K_M cost in accuracy?
#
# Q4 is the main speed lever for CPU Tier-A — roughly half the bytes to stream,
# and CPU generation is bandwidth-bound. It is only a lever if quality survives,
# and quality is cheap to measure on the GPU, so measure it there rather than
# conflating it with the CPU throughput question.
#
# The Q8_0 control already established that Q8_0 is byte-identical to bf16 on
# this task, so any drop here is attributable to Q4 alone.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=results/q4
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8092}

MODELS=(
  "gemma-4-E2B-it|ggml-org/gemma-4-E2B-it-GGUF:Q4_K_M"
  "granite-4.0-1b|ibm-granite/granite-4.0-1b-GGUF:Q4_K_M"
  "gemma-4-E4B-it|unsloth/gemma-4-E4B-it-GGUF:Q4_K_M"
)

for entry in "${MODELS[@]}"; do
  IFS='|' read -r LABEL REPO <<<"$entry"
  PRED="$OUT/$LABEL.pred.jsonl"; LOG="$OUT/$LABEL.server.log"
  [ -s "$PRED" ] && { echo "SKIP $LABEL"; continue; }
  echo "=== SERVE $LABEL Q4_K_M ==="
  $SERVER -hf "$REPO" --port "$PORT" -c 4096 --no-webui >"$LOG" 2>&1 &
  SRV=$!
  ready=0
  for _ in $(seq 1 200); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
    kill -0 $SRV 2>/dev/null || break
    sleep 15
  done
  if [ "$ready" = 1 ]; then
    if $PY harness/run_llamacpp.py --model "$LABEL" --gold data/gold.jsonl \
         --no-thinking \
         --out "$PRED" --base-url "http://127.0.0.1:$PORT" >>"$LOG" 2>&1; then
      $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
          --json-out "$OUT/$LABEL.score.json" >/dev/null 2>>"$LOG"
      # OK is claimed only if a score exists. run_llamacpp.py exits 0 even when
      # every row carries a transport error, so its exit status is not evidence
      # the run succeeded. A sweep that reports OK for an empty run is worse
      # than one that fails.
      if [ -s "$OUT/$LABEL.score.json" ]; then
        echo "OK   $LABEL"
      else
        echo "FAIL $LABEL -> scorer refused: $(tail -2 "$LOG" | tr '\n' ' ' | cut -c1-200)"
        rm -f "$PRED"
      fi
    else
      echo "FAIL $LABEL"; rm -f "$PRED"
    fi
  else
    echo "FAIL $LABEL -> server never healthy"
  fi
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  sleep 5
done
echo "SWEEP_Q4_DONE"
