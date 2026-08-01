#!/bin/bash
# The two models that fell between the lanes.
#
# Qwen3-1.7B is above the sub-1B sweep's range and was never added to the
# thinking ladder, so it belongs to neither and had no thinking-on measurement.
# It is not a minor omission: the confidence-floor correction moved it 0.4000 ->
# 0.5937, one of the largest swings in that whole rescore, and then nothing
# re-ran it under the current configuration.
#
# gemma-3n-E4B-it is the previous-generation 3n model, dropped when the ladder
# moved to Gemma 4. It is the only measurement of what one Gemma generation
# bought on this task: 3n-E4B scored 0.6479 against gemma-4-E4B's 0.8217, both
# on the shipping gate, but the 3n figure is thinking-OFF and so not comparable
# until it is re-run here.
#
# Results land in results/thinking/ because that is the lane they should have
# been in; the sweeps skip anything with predictions already on disk, so this
# cannot disturb the eight models already there.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=results/thinking
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8090}

# Ordered by how much the rescore moved them, so the models that carry the
# question land first. Repos are the ones the cpu throughput lane already
# resolved, except granite-4.0-h-350m, which has only ever been run through
# transformers; if its GGUF does not exist the sweep says so rather than
# skipping it quietly.
MODELS=(
  "Qwen3-1.7B|Qwen/Qwen3-1.7B-GGUF:Q8_0|"
  "gemma-3n-E4B-it|unsloth/gemma-3n-E4B-it-GGUF:Q8_0|"
)

for entry in "${MODELS[@]}"; do
  IFS='|' read -r LABEL REPO EXTRA <<<"$entry"
  PRED="$OUT/$LABEL.pred.jsonl"; LOG="$OUT/$LABEL.server.log"
  [ -s "$PRED" ] && { echo "SKIP $LABEL"; continue; }
  echo "=== SERVE $LABEL (orphans, thinking enabled) ==="
  # shellcheck disable=SC2086
  $SERVER -hf "$REPO" --port "$PORT" -c 8192 --no-webui --no-mmproj $EXTRA \
      >"$LOG" 2>&1 &
  SRV=$!
  ready=0
  for _ in $(seq 1 120); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
    kill -0 $SRV 2>/dev/null || break
    sleep 10
  done
  if [ "$ready" = 1 ]; then
    # Explicit, because the point of this sweep is to re-measure these models
    # under the CURRENT configuration. It previously inherited thinking-off and a
    # 512-token cap by omission, which is the exact pair that invalidated the
    # .254 challenger sweep.
    if $PY harness/run_llamacpp.py --model "$LABEL" --gold data/gold.jsonl \
         --thinking --max-tokens 8192 \
         --out "$PRED" --base-url "http://127.0.0.1:$PORT" >>"$LOG" 2>&1; then
      # Scored under all three gates. The default is the shipping one; the other
      # two are what the historical table used, and keeping them side by side is
      # what makes defect 17 checkable rather than a claim in a log.
      for KEY in pred_grounded pred pred_nofloor; do
        $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
            --pred-key "$KEY" --json-out "$OUT/$LABEL.score.$KEY.json" \
            >/dev/null 2>>"$LOG"
      done
      cp "$OUT/$LABEL.score.pred_grounded.json" "$OUT/$LABEL.score.json"
      # OK is claimed only if a score exists. run_llamacpp.py exits 0 even when
      # every row carries a transport error, so its exit status is not evidence
      # the run succeeded. A sweep that reports OK for an empty run is worse
      # than one that fails.
      if [ -s "$OUT/$LABEL.score.$KEY.json" ]; then
        echo "OK   $LABEL"
      else
        echo "FAIL $LABEL -> scorer refused: $(tail -2 "$LOG" | tr '\n' ' ' | cut -c1-200)"
        rm -f "$PRED"
      fi
    else
      echo "FAIL $LABEL -> $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-180)"
      rm -f "$PRED"
    fi
  else
    echo "FAIL $LABEL -> server never healthy: $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-180)"
  fi
  cpu_layers=$(grep -c 'assigned to device CPU' "$LOG" 2>/dev/null) || true
  printf '{"model":"%s","lane":"sub1b","extra":"%s","cpu_layer_warnings":%s}\n' \
    "$LABEL" "$EXTRA" "${cpu_layers:-0}" > "$OUT/$LABEL.device.json"
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  sleep 5
  [ -x harness/prune_models.sh ] && \
    KEEP="$REPO" HF_HOME="$HF_HOME" bash harness/prune_models.sh 2>/dev/null | tail -1
done
echo "SWEEP_ORPHANS_DONE"
