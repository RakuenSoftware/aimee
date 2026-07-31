#!/bin/bash
# Do we need the confidence field at all?
#
# It is requested in the schema, most models copy the literal 0.0 straight out of
# the example, and the drain then used it to discard their work. Rather than
# filter on a number nobody calibrates, this asks whether the contract should
# carry it: dropping it shortens the prompt, removes the literal models imitate,
# and saves output tokens on the highest-volume LLM path in the KB.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=results/noconf
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8085}

MODELS=(
  "gemma-4-E4B-it|unsloth/gemma-4-E4B-it-GGUF:Q8_0|"
  "gemma-4-26B-A4B-it|unsloth/gemma-4-26B-A4B-it-GGUF:Q8_0|-ot .ffn_.*_exps.=CPU"
)

for entry in "${MODELS[@]}"; do
  IFS='|' read -r LABEL REPO EXTRA <<<"$entry"
  PRED="$OUT/$LABEL.pred.jsonl"; LOG="$OUT/$LABEL.server.log"
  [ -s "$PRED" ] && { echo "SKIP $LABEL"; continue; }
  echo "=== SERVE $LABEL (no-confidence prompt) ==="
  # shellcheck disable=SC2086
  $SERVER -hf "$REPO" --port "$PORT" -c 4096 --no-webui $EXTRA >"$LOG" 2>&1 &
  SRV=$!
  ready=0
  for _ in $(seq 1 240); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
    kill -0 $SRV 2>/dev/null || break
    sleep 15
  done
  if [ "$ready" = 1 ]; then
    if $PY harness/run_llamacpp.py --model "$LABEL" --gold data/gold.jsonl \
         --no-thinking \
         --out "$PRED" --base-url "http://127.0.0.1:$PORT" --no-confidence >>"$LOG" 2>&1; then
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
  KEEP='' HF_HOME="$HF_HOME" bash harness/prune_models.sh 2>/dev/null | tail -1
done
echo "SWEEP_NOCONF_DONE"
