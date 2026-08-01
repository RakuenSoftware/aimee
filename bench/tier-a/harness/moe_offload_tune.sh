#!/bin/bash
# How much of gemma-4-26B-A4B can stay on the 16GB 5080, and what does that buy?
#
# Supersedes the first tune (/opt/tierA/moe-tune.sh), whose numbers cannot be
# used. Three faults, all in the probe rather than in llama.cpp:
#
#   1. It never reproduced the ladder's config, so there was no control. Every
#      probe carried --no-mmap and --no-mmproj, which the ladder did not.
#   2. Readiness capped at 900s. With --no-mmap a ~28GB Q8_0 must be read into
#      RAM before the server answers /health, and the baseline probe was killed
#      mid-load and recorded as "OOM_OR_FAILED" with an empty reason. The box has
#      48GB and the log contains no allocation failure: that was a timeout.
#   3. tg was scraped from a single cold 100-token generation taken immediately
#      after load, so it measured page-fault-in, not steady state. That is why
#      the results were non-monotonic (40:7.38, 32:7.26, 24:6.86, 16:9.88) when
#      less offload can only be faster.
#
# Every number below therefore comes from a warmed server over a generation long
# enough to average, and the ladder config is measured first as the control.
set -u
BIN=${BIN:-/opt/llama.cpp/build-cuda/bin/llama-server}
REPO=${REPO:-unsloth/gemma-4-26B-A4B-it-GGUF:Q8_0}
export HF_HOME=${HF_HOME:-/opt/hf}
PORT=${PORT:-8093}
OUT=${OUT:-/opt/tierA/moe-tune2.txt}
WARMUP_TOK=128
MEASURE_TOK=400
READY_TRIES=240   # 40 minutes: a cold --no-mmap load is disk-bound, not hung
: > "$OUT"

gen() {  # <n_predict> -> prints nothing, drives the server
  curl -s --max-time 1800 "http://127.0.0.1:$PORT/completion" \
    -H 'Content-Type: application/json' \
    -d "{\"prompt\":\"Write a detailed technical description of how a B-tree index works.\",\"n_predict\":$1,\"temperature\":0,\"cache_prompt\":false}" \
    >/dev/null 2>&1
}

probe() {  # <label> <extra args...>
  local label=$1; shift
  local log=/tmp/moe2-$label.log
  "$BIN" -hf "$REPO" --port "$PORT" -c 8192 --no-webui -ngl 99 "$@" > "$log" 2>&1 &
  local srv=$!
  local ready=0
  for _ in $(seq 1 $READY_TRIES); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ready=1; break; }
    kill -0 $srv 2>/dev/null || break
    sleep 10
  done
  if [ "$ready" != 1 ]; then
    # Name the failure. An allocation failure and a load that never finished are
    # different findings and the first tune conflated them.
    local why
    why=$(grep -oiE 'out of memory|failed to allocate|cannot allocate|unable to allocate' "$log" | head -1)
    if kill -0 $srv 2>/dev/null; then
      echo "$label LOAD_TIMEOUT after $((READY_TRIES*10))s (server still loading)" | tee -a "$OUT"
    else
      echo "$label FAILED ${why:-server exited, see $log}" | tee -a "$OUT"
    fi
    kill -9 $srv 2>/dev/null; wait $srv 2>/dev/null; sleep 10; return
  fi

  gen $WARMUP_TOK                       # fault everything in; discarded
  local mark; mark=$(wc -l < "$log")    # measure only what follows
  gen $MEASURE_TOK
  local tg
  tg=$(tail -n +$((mark+1)) "$log" | grep -oE 'eval time =.*\(.*, *[0-9.]+ tokens per second\)' \
        | tail -1 | grep -oE '[0-9.]+ tokens per second' | grep -oE '[0-9.]+')
  local vram
  vram=$(grep -oE 'CUDA0 model buffer size *= *[0-9.]+ MiB' "$log" | tail -1 | grep -oE '[0-9.]+' | tail -1)
  echo "$label tg=${tg:-none} t/s vram=${vram:-?} MiB" | tee -a "$OUT"
  kill $srv 2>/dev/null; wait $srv 2>/dev/null; sleep 10
}

# Control first: exactly what sweep_thinking.sh served, mmap on, projector on.
probe ladder-baseline -ot ".ffn_.*_exps.=CPU"

# Same total offload, with the two flags the ladder was missing. Isolates the
# flags from the offload split, which the first tune could not do.
probe all-experts-cpu --no-mmproj --no-mmap -ot ".ffn_.*_exps.=CPU"

# Then walk the offload down. Fewer CPU layers should be monotonically faster
# until VRAM runs out; if it is not monotone, the measurement is still wrong.
for n in 40 32 24 16 12 8; do
  probe ncmoe-$n --no-mmproj --no-mmap --n-cpu-moe $n
done
echo TUNE2_DONE >> "$OUT"
