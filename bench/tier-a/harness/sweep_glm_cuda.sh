#!/bin/bash
# Is GLM-4.7-Flash broken, or is RADV Vulkan broken for it?
#
# On .254 the Q6_K GGUF under RADV produced three symptoms at once: literal '?'
# characters from a raw /completion with no chat template, 7943 reasoning tokens
# with zero content through the chat template, and 0.68 tok/s for a 30B-A3B
# resident on a 24GB card. Nothing is known about its extraction quality.
#
# This is the discriminating run: same model, CUDA instead of Vulkan, Q8_0
# instead of Q6_K, expert tensors to CPU so it fits the 16GB card the same way
# gemma-4-26B-A4B does.
#
#   works here  -> the .254 failure is the Vulkan path or the Q6 GGUF, and GLM
#                  gets a real evaluation on this lane
#   fails here  -> the model or the GGUF family, and GLM is out
#
# Two variables move at once (backend and quant), which is fine for a yes/no on
# "does it execute" and would not be fine for a quality comparison. If it works,
# the number that counts comes from this lane at Q8_0, which is what the rest of
# the .253 ladder uses anyway.
set -u
cd "$(dirname "$0")/.."
PY=${PY:-/opt/bench/bin/python}
SERVER=${SERVER:-/opt/llama.cpp/build-cuda/bin/llama-server}
OUT=results/thinking
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8087}

LABEL=GLM-4.7-Flash
REPO=unsloth/GLM-4.7-Flash-GGUF:Q8_0
PRED="$OUT/$LABEL.pred.jsonl"; LOG="$OUT/$LABEL.server.log"
[ -s "$PRED" ] && { echo "SKIP $LABEL"; exit 0; }

echo "=== SERVE $LABEL (CUDA, Q8_0, experts on CPU) ==="
$SERVER -hf "$REPO" --port "$PORT" -c 8192 --no-webui --no-mmproj \
    -ngl 99 -ot ".ffn_.*_exps.=CPU" >"$LOG" 2>&1 &
SRV=$!
ready=0
for _ in $(seq 1 360); do
  curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
  kill -0 $SRV 2>/dev/null || break
  sleep 10
done

if [ "$ready" = 1 ]; then
  # Probe before committing an hour to it. If this returns non-language the way
  # .254 did, there is no point running 70 notes through it.
  probe=$(curl -s "http://127.0.0.1:$PORT/completion" -H 'Content-Type: application/json' \
      -d '{"prompt":"The capital of France is","n_predict":12,"temperature":0}' \
      | $PY -c 'import json,sys; print((json.load(sys.stdin).get("content") or "")[:80])' 2>/dev/null)
  echo "PROBE $LABEL -> $(printf '%q' "$probe")"
  case "$probe" in
    *[Pp]aris*)
      echo "PROBE OK: coherent output on CUDA. The .254 failure was the Vulkan path or the Q6 GGUF."
      if $PY harness/run_llamacpp.py --model "$LABEL" --gold data/gold.jsonl \
           --thinking --max-tokens 8192 \
           --out "$PRED" --base-url "http://127.0.0.1:$PORT" >>"$LOG" 2>&1; then
        $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
            --json-out "$OUT/$LABEL.score.json" >/dev/null 2>>"$LOG"
        if [ -s "$OUT/$LABEL.score.json" ]; then echo "OK   $LABEL"
        else echo "FAIL $LABEL -> scorer refused"; rm -f "$PRED"; fi
      else
        echo "FAIL $LABEL -> runner error"; rm -f "$PRED"
      fi
      ;;
    *)
      echo "PROBE FAILED: CUDA output is also not language. The GGUF or the model, not the backend."
      echo "SKIPPING the 70-note run; it would cost an hour to produce nothing."
      ;;
  esac
else
  echo "FAIL $LABEL -> server never healthy: $(tail -3 "$LOG" | tr '\n' ' ' | cut -c1-200)"
fi
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
sleep 5
[ -x harness/prune_models.sh ] && KEEP="$REPO" HF_HOME="$HF_HOME" bash harness/prune_models.sh 2>/dev/null | tail -1
echo "SWEEP_GLM_CUDA_DONE"
