#!/bin/bash
# Tier-B synthesize across the model ladder.
#
# E4B runs first and twice: once on GPU for quality, once on CPU for the number
# that decides whether a local install is worth offering. The rest follow on GPU
# so the ladder is comparable to Tier-A's.
#
# Tier-B does not set disable_thinking (kb_curator_provider_for_stage applies it
# to Tier-A only), so thinking is left on, matching production.
#
# Three modes, and the third exists because of a defect:
#
#   gpu     models that fit the 16GB card, served from it.
#   cpu     the E4B throughput control.
#   cpufit  models too large to be resident at Q8_0. These used to sit in the
#           gpu list, where llama.cpp's auto-fit silently placed them on CPU and
#           wrote the result into a directory named "gpu". Dense Qwen3.6-27B
#           served at 1.72 tok/s that way. Accuracy was unaffected (the same
#           GGUF answers the same wherever its tensors sit) but every speed
#           number in that directory was confounded. They are now a declared CPU
#           lane, in a directory that says so.
#
# cpufit is designed to run CONCURRENTLY with gpu: it is CPU-bound and the gpu
# lane is not, so serialising them buys nothing. It takes its own port and a
# bounded thread count so it cannot starve the gpu lane's prompt processing or
# the MoE expert offload. prune_models.sh already refuses to delete weights a
# live llama-server holds open, so the two lanes cannot prune each other's model.
#
# Usage: sweep_b.sh [gpu|cpu|cpufit]
set -u
cd "$(dirname "$0")/.."
MODE=${1:-gpu}
PY=${PY:-/opt/bench/bin/python}
OUT="results/$MODE"
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}

case "$MODE" in
  cpu)
    PORT=${PORT:-8095}
    # The CUDA binary with -ngl 0 and no visible device. /opt/llama.cpp/build
    # holds libraries but no llama-server binary, so there is no CPU-only build
    # to point at, and forcing the CUDA one off the GPU is equivalent and one
    # fewer thing to keep compiled.
    SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
    export CUDA_VISIBLE_DEVICES=""
    THREADS=${THREADS:-20}
    PLACE="-ngl 0 -t $THREADS"
    MODELS=("gemma-4-E4B-it|unsloth/gemma-4-E4B-it-GGUF:Q8_0|")
    ;;
  sub1b)
    # Tier-B has never been run below 1B. The Tier-A ladder was read as ruling
    # this range out, and that reading was an artefact of scoring against a
    # retired confidence floor (MEASUREMENT_LOG.md defect 17), so the range was
    # excluded here on a conclusion that did not hold. Qwen3.5-0.8B is included
    # because its first attempt returned 4096 tokens of reasoning and empty
    # content on every topic, which run_b.py could not distinguish from a harness
    # bug until it started recording reasoning_content.
    PORT=${PORT:-8089}
    SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
    PLACE=""
    MODELS=(
      "Qwen3.5-0.8B|ggml-org/Qwen3.5-0.8B-GGUF:Q8_0|"
      "Qwen3-0.6B|Qwen/Qwen3-0.6B-GGUF:Q8_0|"
      "granite-4.0-350m|ibm-granite/granite-4.0-350m-GGUF:Q8_0|"
      "granite-4.0-h-350m|ibm-granite/granite-4.0-h-350m-GGUF:Q8_0|"
      "granite-4.0-h-1b|ibm-granite/granite-4.0-h-1b-GGUF:Q8_0|"
      "LFM2.5-230M|unsloth/LFM2.5-230M-GGUF:Q8_0|"
      "LFM2-350M-Extract|LiquidAI/LFM2-350M-Extract-GGUF:Q8_0|"
      "SmolLM2-360M-Instruct|ggml-org/SmolLM2-360M-Instruct-Q8_0-GGUF:Q8_0|"
      "gemma-3-270m-it|ggml-org/gemma-3-270m-GGUF:Q8_0|"
    )
    ;;
  cpufit)
    # Own port, so it can share the box with the gpu lane. 8 of 20 threads
    # leaves the gpu lane the cores its expert offload needs, and an empty
    # CUDA_VISIBLE_DEVICES guarantees it reserves no VRAM alongside it.
    PORT=${PORT:-8096}
    SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
    export CUDA_VISIBLE_DEVICES=""
    THREADS=${THREADS:-8}
    PLACE="-ngl 0 -t $THREADS"
    MODELS=(
      "Qwen3.6-27B|unsloth/Qwen3.6-27B-GGUF:Q8_0|"
    )
    ;;
  *)
    PORT=${PORT:-8095}
    SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
    PLACE=""
    MODELS=(
      "gemma-4-E4B-it|unsloth/gemma-4-E4B-it-GGUF:Q8_0|"
      "gemma-4-E2B-it|ggml-org/gemma-4-E2B-it-GGUF:Q8_0|"
      "gemma-4-26B-A4B-it|unsloth/gemma-4-26B-A4B-it-GGUF:Q8_0|-ot .ffn_.*_exps.=CPU"
      "gemma-4-12B-it|unsloth/gemma-4-12B-it-GGUF:Q8_0|"
      "Qwen3.6-35B-A3B|unsloth/Qwen3.6-35B-A3B-GGUF:Q8_0|-ot .ffn_.*_exps.=CPU"
      "granite-4.1-3b|ibm-granite/granite-4.1-3b-GGUF:Q8_0|"
      "granite-4.0-1b|ibm-granite/granite-4.0-1b-GGUF:Q8_0|"
      "Qwen3.5-0.8B|ggml-org/Qwen3.5-0.8B-GGUF:Q8_0|"
    )
    ;;
esac

for entry in "${MODELS[@]}"; do
  IFS='|' read -r LABEL REPO EXTRA <<<"$entry"
  PRED="$OUT/$LABEL.pred.jsonl"; LOG="$OUT/$LABEL.server.log"
  [ -s "$PRED" ] && { echo "SKIP $LABEL"; continue; }
  echo "=== SERVE $LABEL ($MODE, port $PORT) ==="
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
  # Device provenance, recorded rather than implied by the directory name. This
  # is the check that would have caught Qwen3.6-27B serving from CPU inside
  # results/gpu/. A speed number without this file is device-unknown.
  # grep -c prints 0 AND exits 1 when it matches nothing, so a `|| echo 0`
  # fallback emits the count twice and produces invalid JSON. Take grep's own
  # output and swallow the exit status.
  cpu_layers=$(grep -c 'assigned to device CPU' "$LOG" 2>/dev/null) || true
  cpu_layers=${cpu_layers:-0}
  printf '{"model":"%s","mode":"%s","place":"%s","extra":"%s","cpu_layer_warnings":%s,"resident_on_gpu":%s}\n' \
    "$LABEL" "$MODE" "$PLACE" "$EXTRA" "$cpu_layers" \
    "$([ "$MODE" = gpu ] && [ "$cpu_layers" -eq 0 ] && echo true || echo false)" \
    > "$OUT/$LABEL.device.json"
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  sleep 5
  [ -x ../tier-a/harness/prune_models.sh ] && \
    KEEP="$REPO" HF_HOME="$HF_HOME" bash ../tier-a/harness/prune_models.sh 2>/dev/null | tail -1
done
echo "SWEEP_B_${MODE}_DONE"
