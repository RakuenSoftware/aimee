#!/bin/bash
# E4B Q4 again, this time at 32 slots with the MTP draft head, against the
# sequential arm of exactly the same model/quant/corpus/prompt that just
# finished. Everything is held constant except how the tokens are produced.
#
# The sequential reference is REAL banked benchmark data (1001 notes, 43.8 min,
# 22.9 notes/min), not a warm-up sample, so the comparison answers both questions
# at full size:
#
#   how much faster  -- wall clock and notes/min over the same 1001 notes
#   at what cost     -- raw completions compared byte-for-byte, note by note
#
# If the outputs match, parallel+MTP is free throughput and every future sweep
# gets it. If they do not, the ladder can still use it (all arms would share the
# same conditions) but its numbers stop being comparable to the sequential arms
# already banked, and that has to be recorded rather than discovered later.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
HOST=192.168.1.253; RIP=192.168.0.5; PORT=8110
SLOTS=${SLOTS:-32}
CTX=${CTX:-98304}
MODEL=/opt/hf/e4b-q4.gguf
MTP=/opt/hf/mtp-gemma-4-E4B-it-Q8_0.gguf
LABEL=E4B.UD-Q4_K_XL.v8.np${SLOTS}mtp

say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/parallel_mtp.log"; }

say "serving $MODEL with -np $SLOTS -c $CTX and MTP $(basename $MTP)"
ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
sleep 4
ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m $MODEL -md $MTP --host 0.0.0.0 --port $PORT -c $CTX -np $SLOTS --no-webui --no-mmproj -ngl 99 >/opt/tierA/np-mtp.log 2>&1 </dev/null &'" >/dev/null 2>&1
ok=0
for _ in $(seq 1 90); do
  ssh -n -o ConnectTimeout=10 root@"$HOST" \
    "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
  sleep 10
done
if [ "$ok" != 1 ]; then
  say "server would NOT start with the MTP draft head; showing why"
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- tail -25 /opt/tierA/np-mtp.log" 2>&1 | tee -a "$OUT/parallel_mtp.log"
  exit 1
fi

# What the server actually gave us -- slots requested is not slots granted.
ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "pct exec 140 -- grep -iE 'n_slots|n_ctx|draft|speculat' /opt/tierA/np-mtp.log | head -12; \
   echo '--- vram:'; nvidia-smi --query-compute-apps=used_memory --format=csv,noheader; \
   nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader" \
  > "$OUT/$LABEL.device.txt" 2>&1
say "server config + VRAM -> $LABEL.device.txt"
curl -sf "http://$RIP:$PORT/props" 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)
print('  total_slots granted:', d.get('total_slots'))
" | tee -a "$OUT/parallel_mtp.log"

say "running 1001 notes at concurrency $SLOTS"
t0=$(date +%s)
python3 harness/run_llamacpp.py --model "$LABEL" --gold "$GOLD" \
  --thinking --max-tokens 8192 --concurrency "$SLOTS" \
  --out "$OUT/$LABEL.pred.jsonl" --base-url "http://$RIP:$PORT" \
  >>"$OUT/$LABEL.run.log" 2>&1
t1=$(date +%s)
say "wall clock: $(( (t1-t0)/60 ))m $(( (t1-t0)%60 ))s for $(wc -l < "$OUT/$LABEL.pred.jsonl") notes"

python3 harness/compare_runs.py \
  --a "$OUT/E4B.UD-Q4_K_XL.v8.pred.jsonl" --a-label "sequential (1 in flight)" \
  --b "$OUT/$LABEL.pred.jsonl" --b-label "$SLOTS slots + MTP" \
  2>&1 | tee -a "$OUT/parallel_mtp.log"
