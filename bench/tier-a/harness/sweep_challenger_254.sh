#!/bin/bash
# The challenger slate, on the 7900 XTX at .254, in parallel with .253's queue.
#
# Three models that could plausibly beat Gemma 4 for synthesis, all in the
# <=35B band, all with a licence we can actually ship against:
#
#   GLM-4.7-Flash          30B-A3B MoE, MIT
#   Olmo-3.1-32B-Think     32B dense, Apache-2.0, reasoning-tuned
#   Magistral-Small-2509   24B dense, Apache-2.0, reasoning-tuned
#
# The two Think/reasoning models are here for a measured reason rather than a
# hunch: the largest single effect on this benchmark so far is that leaving
# thinking ON is worth +0.09 F1 to gemma-4-E4B. A model trained for that is the
# obvious next thing to try.
#
# THIS HOST IS TRIAGE, NOT MEASUREMENT.
#
# .254 answers "is this model viable at all" — does it load, does it hold the
# output contract, does it produce anything worth spending .253 GPU time on. It
# does NOT produce numbers to quote. Quantisation differs from .253 (Q6_K/Q5_K_M
# against Q8_0, because 24GB of VRAM with 3GB of system RAM means a model must
# fit the card whole), the backend differs (RADV Vulkan against CUDA), and the
# box does other work.
#
# So: no cross-host control, and no comparing a figure from this lane against
# the .253 ladder. Anything that looks promising here gets re-run on .253 for a
# number. Anything that fails here has failed cheaply, which is the point.
#
# An earlier version of this file ran gemma-4-12B twice, at Q6 and Q8, to
# calibrate the two hosts against each other. That calibration is dropped: it
# was buying comparability this lane does not need and does not claim.
#
# llama.cpp is build 10210, commit 0005475 — the same commit .253 runs, so the
# runtime is not a variable.
set -u
cd "$(dirname "$0")/.."
ROOT=${ROOT:-/mnt/media/tierbench}

# Single-instance lock. Two copies of this sweep once ran at the same time, both
# serving GLM-4.7-Flash on port 8091, because a pkill was issued and its effect
# never checked before relaunching. The second instance also ran `rm -f
# *.pred.jsonl` over the first one's output. Nothing detected it; I found it by
# eye in a process listing. A benchmark that can silently run twice against one
# port produces numbers from an unknown server.
LOCK=$ROOT/challenger-254.lock
exec 9>"$LOCK"
if ! flock -n 9; then
  echo "REFUSING: another challenger sweep holds $LOCK (pid $(cat "$LOCK" 2>/dev/null))" >&2
  exit 1
fi
echo $$ >&9

# Refuse to start if anything already holds the port, whoever owns it.
if curl -sf "http://127.0.0.1:${PORT:-8091}/health" >/dev/null 2>&1; then
  echo "REFUSING: port ${PORT:-8091} already serving. Stop it first." >&2
  exit 1
fi
PY=${PY:-$ROOT/venv/bin/python}
BIN=$ROOT/bin/llama-b10210
OUT=results/challenger-254
mkdir -p "$OUT"
export HF_HOME=${HF_HOME:-$ROOT/hf}
export LD_LIBRARY_PATH="$BIN"
PORT=${PORT:-8091}

# Device 1 is the 7900 XTX. Device 0 is the PHOENIX iGPU at 8GB, which would
# silently take layers and wreck both throughput and fit if left visible.
export GGML_VK_VISIBLE_DEVICES=1

# gemma-4-12B stays as a smoke test, not a control: it is a model known to work
# on this task, so if IT comes out broken here the host is broken, not the
# challenger. One run, cheapest quant.
MODELS=(
  "gemma-4-12B-it.q6|unsloth/gemma-4-12B-it-GGUF:Q6_K|"
  "GLM-4.7-Flash.q6|unsloth/GLM-4.7-Flash-GGUF:Q6_K|"
  "Magistral-Small-2509.q6|unsloth/Magistral-Small-2509-GGUF:Q6_K|"
  "Olmo-3.1-32B-Think.q5|bartowski/allenai_Olmo-3.1-32B-Think-GGUF:Q5_K_M|"
)

for entry in "${MODELS[@]}"; do
  IFS='|' read -r LABEL REPO EXTRA <<<"$entry"
  PRED="$OUT/$LABEL.pred.jsonl"; LOG="$OUT/$LABEL.server.log"
  [ -s "$PRED" ] && { echo "SKIP $LABEL"; continue; }
  echo "=== SERVE $LABEL (7900 XTX, vulkan) ==="
  # shellcheck disable=SC2086
  "$BIN/llama-server" -hf "$REPO" --port "$PORT" -c 8192 --no-webui --no-mmproj \
      -ngl 99 $EXTRA >"$LOG" 2>&1 &
  SRV=$!
  ready=0
  for _ in $(seq 1 360); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
    kill -0 $SRV 2>/dev/null || break
    sleep 10
  done
  if [ "$ready" = 1 ]; then
    # --thinking and --max-tokens 8192 are NOT optional and NOT defaults.
    #
    # The first run of this sweep passed neither, so every model ran with
    # thinking suppressed against run_llamacpp.py's default 512-token cap, a
    # sixteenth of production's MF_LLM_OUT_CAP. That is not the configuration
    # the .253 ladder uses, so nothing was comparable to it, and it hit the
    # reasoning models hardest: Olmo-3.1-32B-THINK truncated on 59 of 70 notes.
    # A sweep whose entire purpose is testing reasoning-tuned models ran them
    # with reasoning off.
    if $PY harness/run_llamacpp.py --model "$LABEL" --gold data/gold.jsonl \
         --thinking --max-tokens 8192 \
         --out "$PRED" --base-url "http://127.0.0.1:$PORT" >>"$LOG" 2>&1; then
      for KEY in pred_grounded pred pred_nofloor; do
        $PY harness/score.py --gold data/gold.jsonl --pred "$PRED" \
            --pred-key "$KEY" --json-out "$OUT/$LABEL.score.$KEY.json" \
            >/dev/null 2>>"$LOG"
      done
      # OK is claimed only if a score actually exists.
      #
      # run_llamacpp.py exits 0 even when every row carries a transport error,
      # because it records failures per note rather than aborting. So the `if`
      # above is not evidence the run succeeded, and this printed
      # "OK GLM-4.7-Flash.q6" for a run whose server had been killed mid-note
      # and which produced no scoreable output at all. The `cp` failed loudly on
      # stderr and the OK was printed anyway.
      if [ -s "$OUT/$LABEL.score.pred_grounded.json" ]; then
        cp "$OUT/$LABEL.score.pred_grounded.json" "$OUT/$LABEL.score.json"
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
  # Device provenance. .253 learned this the hard way: a directory named "gpu"
  # held a model llama.cpp had quietly placed on CPU. Record what served it.
  offl=$(grep -oE 'offloaded [0-9]+/[0-9]+ layers to GPU' "$LOG" 2>/dev/null | tail -1) || true
  cpu_layers=$(grep -c 'assigned to device CPU' "$LOG" 2>/dev/null) || true
  printf '{"model":"%s","host":"192.168.1.254","gpu":"7900XTX","backend":"vulkan","repo":"%s","offload":"%s","cpu_layer_warnings":%s}\n' \
    "$LABEL" "$REPO" "${offl:-unknown}" "${cpu_layers:-0}" > "$OUT/$LABEL.device.json"
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  sleep 5
  # 43TB free on /mnt/media, so weights are kept rather than pruned. Re-running a
  # model here costs nothing but time.
done
echo "SWEEP_CHALLENGER_254_DONE"
