#!/bin/bash
# Qwen3.6-27B dense, to pair against Qwen3.6-35B-A3B.
#
# The question this settles: the 26B MoE beat the 12B dense on recall (0.906 vs
# 0.828) but was markedly less disciplined — schema validity 0.84 vs 0.96,
# abstention on factless notes 0.67 vs 0.90. Is that MoE, or is it those two
# particular models? Qwen3.6 ships a 27B dense and a 35B-A3B MoE from the same
# family and generation, so running both isolates the architecture.
#
# No -ot here: dense has no expert tensors to peel off, so llama.cpp splits by
# layer instead and roughly half stays resident. Slower per note than the MoE
# offload, which is itself part of the finding — MoE is the shape that offloads
# well.
#
# Q8_0, matching every other llama.cpp run. The E4B control showed Q8_0 produces
# byte-identical output to bf16 on this task, so these numbers are comparable to
# the whole table, not just to each other.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=results/llamacpp
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8081}

LABEL="Qwen3.6-27B"
REPO="unsloth/Qwen3.6-27B-GGUF:Q8_0"
SLUG="$LABEL"
PRED="$OUT/$SLUG.pred.jsonl"
LOG="$OUT/$SLUG.server.log"

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
    kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
    KEEP='' HF_HOME="$HF_HOME" bash harness/prune_models.sh 2>/dev/null | tail -2
  else
    if $PY harness/run_llamacpp.py --model "$LABEL" --gold data/gold.jsonl \
         --no-thinking \
         --out "$PRED" --base-url "http://127.0.0.1:$PORT" >>"$LOG" 2>&1; then
      $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
          --json-out "$OUT/$SLUG.score.json" >/dev/null 2>>"$LOG"
      # OK is claimed only if a score exists. run_llamacpp.py exits 0 even when
      # every row carries a transport error, so its exit status is not evidence
      # the run succeeded. A sweep that reports OK for an empty run is worse
      # than one that fails.
      if [ -s "$OUT/$SLUG.score.json" ]; then
        echo "OK   $LABEL"
      else
        echo "FAIL $LABEL -> scorer refused: $(tail -2 "$LOG" | tr '\n' ' ' | cut -c1-200)"
        rm -f "$PRED"
      fi
    else
      echo "FAIL $LABEL -> $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-200)"
      rm -f "$PRED"
    fi
    kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  fi
  # Weights are ~30GB and re-downloadable; the predictions are the artefact.
  sleep 5
  KEEP='' HF_HOME="$HF_HOME" bash harness/prune_models.sh 2>/dev/null | tail -2
fi
echo "SWEEP_DENSE_DONE"
