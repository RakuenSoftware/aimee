#!/bin/bash
# The sub-1B ladder, re-run properly.
#
# Why this exists: I reported six of these models as scoring exactly 0.0000 on
# Tier-A extraction and concluded that nothing below roughly 600M parameters
# produces usable output. That table was scored against MF_CONF_FLOOR, which the
# product had already removed in favour of fact_grounded(). Rescored against the
# gate that ships, four of the six are not zero: Qwen3-0.6B 0.4058,
# granite-4.0-350m 0.2063, granite-4.0-h-350m 0.1364, LFM2.5-230M 0.0263. See
# MEASUREMENT_LOG.md defect 17.
#
# The rescore fixed the gate. It could not fix the run: every one of those
# numbers was produced with disable_thinking set, which cost gemma-4-E4B 0.09 F1
# on this same set. So the corrected figures are a floor, not a result, and this
# sweep produces the missing half.
#
# Everything here is production configuration. No repetition penalty, no
# adjusted cap, no per-model prompt. LFM2-350M-Extract loops one fact to the
# token cap and SmolLM2-360M omits the "facts" wrapper; those are findings about
# what the shipped prompt gets from these models, and tuning them away would
# measure a system we do not run. The diagnostics lane already holds the
# reppen1.1 and cap512/cap2048 variants for anyone who wants the comparison.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=results/sub1b
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8088}

# Ordered by how much the rescore moved them, so the models that carry the
# question land first. Repos are the ones the cpu throughput lane already
# resolved, except granite-4.0-h-350m, which has only ever been run through
# transformers; if its GGUF does not exist the sweep says so rather than
# skipping it quietly.
MODELS=(
  "Qwen3-0.6B|Qwen/Qwen3-0.6B-GGUF:Q8_0|"
  "granite-4.0-350m|ibm-granite/granite-4.0-350m-GGUF:Q8_0|"
  "granite-4.0-h-350m|ibm-granite/granite-4.0-h-350m-GGUF:Q8_0|"
  "granite-4.0-h-1b|ibm-granite/granite-4.0-h-1b-GGUF:Q8_0|"
  "LFM2.5-230M|unsloth/LFM2.5-230M-GGUF:Q8_0|"
  "LFM2-350M-Extract|LiquidAI/LFM2-350M-Extract-GGUF:Q8_0|"
  # ggml-org/SmolLM2-360M-Instruct-Q8_0-GGUF does not exist; the server failed
  # to load with `load_model: failed to load model, ''` on both sweep passes.
  # HuggingFaceTB is the model's own publisher.
  "SmolLM2-360M-Instruct|HuggingFaceTB/SmolLM2-360M-Instruct-GGUF:Q8_0|"
  "gemma-3-270m-it|ggml-org/gemma-3-270m-GGUF:Q8_0|"
)

for entry in "${MODELS[@]}"; do
  IFS='|' read -r LABEL REPO EXTRA <<<"$entry"
  PRED="$OUT/$LABEL.pred.jsonl"; LOG="$OUT/$LABEL.server.log"
  [ -s "$PRED" ] && { echo "SKIP $LABEL"; continue; }
  echo "=== SERVE $LABEL (sub1b, thinking enabled) ==="
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
echo "SWEEP_SUB1B_DONE"
