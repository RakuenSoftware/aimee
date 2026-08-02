#!/bin/bash
# Isolate WHY speculative decoding is disabled, on the idle XTX, so the 5080
# keeps the benchmark to itself.
#
# On the 5080 at -np 32 the server logs
#   [spec] failed to measure draft model memory: failed to create llama_context
# and every slot then reports speculative=false. The draft head loads; enabling
# does not happen. Leading hypothesis: the draft context cannot be created at
# high -np, i.e. speculation and many slots are mutually exclusive here.
#
# So: same draft head, three slot counts, read /slots each time. If speculative
# is true at 1 and false at 32, the hypothesis holds.
#
# TWO CAVEATS, both load-bearing:
#
#   Different build. .254 runs llama-b10210 under Vulkan; .253 runs the CUDA
#   build (version 11 / 0005475). A negative here does not prove the CUDA build
#   behaves the same. The -np gating lives in common/ rather than a backend, so
#   it should transfer -- "should" is doing work in that sentence.
#
#   Device pinning is MANDATORY. Vulkan0 is a 16GB Phoenix iGPU and Vulkan1 is
#   the 7900 XTX; llama.cpp takes the FIRST device by default, which is how this
#   lane spent a week measuring an integrated GPU (defect 30). --device Vulkan1
#   is not optional and the device actually used is recorded below.
set -u
HOST=admin@192.168.1.254
BIN=/mnt/media/tierbench/bin/llama-b10210/llama-server
MODELS=/mnt/media/storage/models/gguf
PORT=8115
OUT=${OUT:?set OUT}
mkdir -p "$OUT"
LOG="$OUT/mtp_xtx.log"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$LOG"; }

say "fetching the E4B MTP head to .254 (once)"
ssh -n -o ConnectTimeout=20 "$HOST" \
  "cd $MODELS && [ -s mtp-gemma-4-E4B-it-Q8_0.gguf ] || curl -sfL -o mtp-gemma-4-E4B-it-Q8_0.gguf \
   'https://huggingface.co/ggml-org/gemma-4-E4B-it-GGUF/resolve/main/mtp-gemma-4-E4B-it-Q8_0.gguf?download=true'; \
   ls -la mtp-gemma-4-E4B-it-Q8_0.gguf" 2>&1 | tail -2 | tee -a "$LOG"

say "devices visible on .254:"
ssh -n -o ConnectTimeout=20 "$HOST" "$BIN --list-devices 2>/dev/null" | tee -a "$LOG"

for NP in 1 4 32; do
  say "--- starting with -np $NP, draft head on, pinned to Vulkan1"
  ssh -n -o ConnectTimeout=20 "$HOST" "pkill -f 'llama-server --model' ; pkill -f 'llama-server -m '" >/dev/null 2>&1 || true
  sleep 4
  ssh -n -o ConnectTimeout=25 "$HOST" \
    "nohup setsid $BIN -m $MODELS/gemma-4-E4B-it-UD-Q4_K_XL.gguf \
       -md $MODELS/mtp-gemma-4-E4B-it-Q8_0.gguf \
       --host 0.0.0.0 --port $PORT -c 8192 -np $NP --device Vulkan1 \
       --no-webui --no-mmproj -ngl 99 > /tmp/mtp-np$NP.log 2>&1 </dev/null &" >/dev/null 2>&1
  ok=0
  for _ in $(seq 1 60); do
    ssh -n -o ConnectTimeout=10 "$HOST" "curl -sf --max-time 5 http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
    sleep 10
  done
  if [ "$ok" != 1 ]; then
    say "  -np $NP: server never healthy; last log lines:"
    ssh -n -o ConnectTimeout=20 "$HOST" "tail -12 /tmp/mtp-np$NP.log" 2>&1 | tee -a "$LOG"
    continue
  fi
  spec=$(ssh -n -o ConnectTimeout=15 "$HOST" \
    "curl -sf http://127.0.0.1:$PORT/slots" 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin); s=d[0] if d else {}
print(f\"slots={len(d)} speculative={s.get('speculative')} types={(s.get('params') or {}).get('speculative.types')}\")" 2>/dev/null || echo "unreadable")
  say "  -np $NP -> $spec"
  ssh -n -o ConnectTimeout=20 "$HOST" "grep -iE 'spec|draft|Vulkan1|device' /tmp/mtp-np$NP.log | head -6" 2>&1 | sed 's/^/      /' | tee -a "$LOG"
done

say "cleaning up the probe server"
ssh -n -o ConnectTimeout=20 "$HOST" "pkill -f 'port $PORT'" >/dev/null 2>&1 || true
say "=== MTP PROBE DONE ==="
