#!/bin/bash
# ONE quant arm of the v8 ladder. MODEL and LABEL come from the caller, so every
# arm differs ONLY in the weights: same corpus, same prompt, same server build,
# same card, run back to back.
#
# The standing decision says Q6 is the default for E2B. Its evidence is thin in
# two independent ways:
#
#   n = 69 notes. The measured E2B gap was +0.0012 with a 95% CI of
#   [-0.0633, +0.0690] -- indistinguishable, and reported as such.
#
#   It was taken on .254 "under RADV Vulkan on the 7900 XTX". Defect 30 later
#   established that lane had been serving from an 8GB Phoenix iGPU with no
#   driver bound to the XTX, and voided its speed and fit numbers.
#
# So E2B's quant choice rests on an indistinguishable delta measured on hardware
# that turned out not to be the hardware. This runs it at 1001 notes on the 5080,
# same corpus, same prompt, same server process as the Q4 arm -- one variable.
#
# Device provenance is RECORDED here rather than assumed, which is the guard
# defect 30 left behind: -ngl 99 and an absence of offload warnings were both
# satisfied by the wrong card.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
EXPECT=$(wc -l < "$GOLD")
mkdir -p "$OUT"
LOG="$OUT/driver.log"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" >> "$LOG"; }

HOST=192.168.1.253
RIP=192.168.0.5
PORT=8110
MODEL=${MODEL:?set MODEL to the gguf path}
LABEL=${LABEL:?set LABEL}

pred="$OUT/$LABEL.pred.jsonl"
if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then
  say "SKIP $LABEL (already complete)"; exit 0
fi

say "SERVE $MODEL"
ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
sleep 4
ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m $MODEL --host 0.0.0.0 --port $PORT -c 8192 --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
ok=0
for _ in $(seq 1 90); do
  ssh -n -o ConnectTimeout=10 root@"$HOST" \
    "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
  sleep 10
done
[ "$ok" = 1 ] || { say "FAIL $LABEL: server never healthy"; exit 1; }

# Record WHICH card served this, and that the serving process is attributed to
# it. "-ngl 99 was requested" is what defect 30 recorded and it was not enough.
ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "nvidia-smi --query-gpu=index,name,pci.bus_id,memory.total --format=csv,noheader;
   echo '--- compute apps:';
   nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader;
   echo '--- serving:'; pct exec 140 -- pgrep -a llama-server" \
  > "$OUT/$LABEL.device.txt" 2>&1
say "DEVICE recorded -> $LABEL.device.txt"

say "START $LABEL"
python3 harness/run_llamacpp.py --model "$LABEL" --gold "$GOLD" \
  --thinking --max-tokens 8192 --out "$pred" \
  --base-url "http://$RIP:$PORT" >>"$OUT/$LABEL.run.log" 2>&1
got=$(wc -l < "$pred" 2>/dev/null || echo 0)
if [ "$got" -lt "$EXPECT" ]; then say "FAIL $LABEL: incomplete ($got/$EXPECT)"; exit 1; fi
think=$(python3 -c "
import json
n=t=0
for l in open('$pred'):
    r=json.loads(l); n+=1; t+=(r.get('reasoning_chars') or 0)>0
print(f'{t}/{n}')" 2>/dev/null)
say "OK   $LABEL rows=$got thinking=$think"
say "=== ARM DONE: $LABEL ==="
