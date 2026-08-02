#!/bin/bash
# Gemma 4 31B dense — the top of the dense ladder, and the size-matched partner
# for the 26B-A4B MoE.
#
# With this the ladder covers dense at 4.5B / 12B / 27B / 31B against MoE at
# 26B-A4B / 35B-A3B, so the dense-vs-MoE discipline gap can be read at comparable
# total parameter counts rather than across a size jump.
#
# Dense, so no -ot AND no -ngl: a hard -ngl 99 forces every layer onto the GPU
# and a 29GB model on a 15.5GB card aborts with "n_gpu_layers already set by
# user to 99, abort". Leaving -ngl unset lets llama.cpp fit as many layers as
# free VRAM allows and page the rest through RAM.
# Q8_0, matching every other llama.cpp run; the E4B control showed Q8_0 is
# byte-identical to bf16 on this task, so these numbers join the main table.
#
# Weights are pruned after scoring — ~33GB, re-downloadable.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=results/llamacpp
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8082}

LABEL="gemma-4-31B-it"
REPO="unsloth/gemma-4-31B-it-GGUF:Q8_0"
PRED="$OUT/$LABEL.pred.jsonl"
LOG="$OUT/$LABEL.server.log"

if [ -s "$PRED" ]; then echo "SKIP $LABEL"; else
  echo "=== SERVE $LABEL ($REPO) dense, layer split ==="
  $SERVER -hf "$REPO" --port "$PORT" -c 4096 --no-webui >"$LOG" 2>&1 &
  SRV=$!
  ready=0
  for _ in $(seq 1 240); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
    kill -0 $SRV 2>/dev/null || break
    sleep 15
  done
  if [ "$ready" != 1 ]; then
    echo "FAIL $LABEL -> server never healthy: $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-200)"
  else
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
      echo "FAIL $LABEL -> $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-200)"
      rm -f "$PRED"
    fi
  fi
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  sleep 5
  KEEP='' HF_HOME="$HF_HOME" bash harness/prune_models.sh 2>/dev/null | tail -2
fi
echo "SWEEP_31B_DONE"
