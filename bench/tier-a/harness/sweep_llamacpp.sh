#!/bin/bash
# MoE accuracy via llama.cpp with expert offload on the 5080.
#
# Why this runtime: a 26B/3.8B-active or 35B/3B-active model does not fit 15.5GB
# of VRAM, and transformers handled that badly — NF4 OOMed, NF4+auto was refused
# by bitsandbytes, enabling its CPU-offload path hit a meta-tensor crash, and
# bf16 offload ran at 74s a note. llama.cpp splits the model the way MoE is meant
# to be split: -ngl 99 puts everything on the GPU, then -ot pulls only the expert
# FFN tensors back to CPU. Attention and shared weights stay resident, and the
# sparsely-touched bulk sits in RAM.
#
# E4B runs first as a runtime control. Changing engine is a confound, so its
# llama.cpp number has to be read against its transformers number (0.705 bf16)
# before either MoE result means anything. Same discipline as the NF4 control.
#
# Q8_0 throughout, so quantisation is constant across all three.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=results/llamacpp
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8080}

# label|hf-repo:quant|extra server args
MODELS=(
  "gemma-4-E4B-it|unsloth/gemma-4-E4B-it-GGUF:Q8_0|"
  "gemma-4-26B-A4B-it|unsloth/gemma-4-26B-A4B-it-GGUF:Q8_0|-ot .ffn_.*_exps.=CPU"
  "Qwen3.6-35B-A3B|unsloth/Qwen3.6-35B-A3B-GGUF:Q8_0|-ot .ffn_.*_exps.=CPU"
)

for entry in "${MODELS[@]}"; do
  IFS='|' read -r LABEL REPO EXTRA <<<"$entry"
  SLUG=$(echo "$LABEL" | tr '/' '_')
  PRED="$OUT/$SLUG.pred.jsonl"
  LOG="$OUT/$SLUG.server.log"
  [ -s "$PRED" ] && { echo "SKIP $LABEL"; continue; }

  echo "=== SERVE $LABEL ($REPO) ${EXTRA:-fully resident} ==="
  # shellcheck disable=SC2086
  $SERVER -hf "$REPO" --port "$PORT" -ngl 99 -c 4096 --no-webui $EXTRA \
      >"$LOG" 2>&1 &
  SRV=$!

  # Weights may need downloading, so allow a long warm-up; poll health rather
  # than sleeping a fixed amount.
  ready=0
  for _ in $(seq 1 240); do
    if curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then ready=1; break; fi
    kill -0 $SRV 2>/dev/null || break
    sleep 15
  done

  if [ "$ready" != 1 ]; then
    echo "FAIL $LABEL -> server never became healthy: $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-200)"
    kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
    continue
  fi
  echo "     healthy; $(grep -iE 'offloaded|CUDA0 model buffer|CPU model buffer' "$LOG" | tail -2 | tr '\n' ' ')"

  if $PY harness/run_llamacpp.py --model "$LABEL" --gold data/gold.jsonl \
         --no-thinking \
       --out "$PRED" --base-url "http://127.0.0.1:$PORT" >>"$LOG" 2>&1; then
    $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
        --json-out "$OUT/$SLUG.score.json" >/dev/null 2>>"$LOG"
    # OK is claimed only if a score exists; the runner exits 0 even when every
    # row carries a transport error.
    if [ -s "$OUT/$LABEL.score.json" ]; then
      echo "OK   $LABEL"
    else
      echo "FAIL $LABEL -> scorer refused"
      rm -f "$PRED"
    fi
  else
    echo "FAIL $LABEL -> $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-200)"
    rm -f "$PRED"
  fi

  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  sleep 5
  # weights are ~30-50GB each and re-downloadable; predictions are what matters
  KEEP='' HF_HOME="$HF_HOME" bash harness/prune_models.sh 2>/dev/null | tail -2
done
echo "SWEEP_LLAMACPP_DONE"
