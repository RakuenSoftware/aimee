#!/bin/bash
# Does disable_thinking cost Tier-A anything?
#
# Production sets it, on the premise that extraction is mechanical. The failure
# modes that actually separate models on this benchmark are negation ("I no
# longer work there"), implicit inference, and restraint on factless notes —
# none of which look mechanical. This runs the same models with reasoning
# enabled, everything else identical.
#
# Token cap 8192, matching MF_LLM_OUT_CAP in src/kb/kb_memory_facts.c.
#
# It was 2048, on the reasoning that the proposal's §1 records an incident where
# a thinking pass consumed the completion budget before the JSON. That was the
# right worry and the wrong number: 2048 is a quarter of what production allows,
# so the harness reproduced the incident instead of testing for it.
# gemma-4-26B-A4B lost 11 of 70 notes to it and gemma-4-12B lost 8, every one
# of them emitting nothing, while E4B and E2B lost none. That reads as
# "thinking hurts big models" and is entirely an artefact of this constant.
#
# score.py now refuses to score any run containing a truncated row, so setting
# this too low fails loudly instead of producing a plausible wrong ladder.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=results/thinking
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8087}

# The whole ladder. The flag applied to every Tier-A provider, so removing it has
# to be validated on every provider. Qwen especially: those models have explicit
# thinking modes that emit long reasoning, so if the original .254 failure — a
# reasoning pass eating the completion budget — reproduces anywhere it reproduces
# there. Truncation is recorded per note, so that shows up as data rather than as
# an unexplained quality drop.
MODELS=(
  "gemma-4-E4B-it|unsloth/gemma-4-E4B-it-GGUF:Q8_0|"
  "gemma-4-E2B-it|ggml-org/gemma-4-E2B-it-GGUF:Q8_0|"
  "gemma-4-12B-it|unsloth/gemma-4-12B-it-GGUF:Q8_0|"
  "gemma-4-26B-A4B-it|unsloth/gemma-4-26B-A4B-it-GGUF:Q8_0|-ot .ffn_.*_exps.=CPU"
  "gemma-4-31B-it|unsloth/gemma-4-31B-it-GGUF:Q8_0|"
  "Qwen3.6-27B|unsloth/Qwen3.6-27B-GGUF:Q8_0|"
  "Qwen3.6-35B-A3B|unsloth/Qwen3.6-35B-A3B-GGUF:Q8_0|-ot .ffn_.*_exps.=CPU"
  "Qwen3.5-2B|unsloth/Qwen3.5-2B-GGUF:Q8_0|"
  "Qwen3.5-0.8B|ggml-org/Qwen3.5-0.8B-GGUF:Q8_0|"
  "granite-4.1-3b|ibm-granite/granite-4.1-3b-GGUF:Q8_0|"
  "granite-4.0-1b|ibm-granite/granite-4.0-1b-GGUF:Q8_0|"
)

for entry in "${MODELS[@]}"; do
  IFS='|' read -r LABEL REPO EXTRA <<<"$entry"
  PRED="$OUT/$LABEL.pred.jsonl"; LOG="$OUT/$LABEL.server.log"
  [ -s "$PRED" ] && { echo "SKIP $LABEL"; continue; }
  echo "=== SERVE $LABEL (thinking enabled) ==="
  # shellcheck disable=SC2086
  # --no-mmproj: this was the ONLY sweep missing it, so every model in this
  # ladder loaded its multimodal projector and gave up VRAM that could have held
  # weights. --no-mmap: llama.cpp warns that CPU tensor overrides with mmap on
  # fault through page cache per token, and gemma-4-26B-A4B was served with
  # every expert on CPU under exactly that condition. It measured 13.55 t/s
  # against gemma-4-12B's 57.76, and prompt processing at 84 tok/s on a 5080,
  # both of which are setup faults reported as model cost.
  #
  # Accuracy is unaffected — the same GGUF yields the same tokens wherever its
  # tensors sit — so the F1 results stand. The latency column does not.
  $SERVER -hf "$REPO" --port "$PORT" -c 8192 --no-webui --no-mmproj --no-mmap $EXTRA >"$LOG" 2>&1 &
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
         --thinking --max-tokens 8192 >>"$LOG" 2>&1; then
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
echo "SWEEP_THINKING_DONE"
