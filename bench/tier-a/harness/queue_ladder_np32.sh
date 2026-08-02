#!/bin/bash
# The quant ladder at 32 slots, queued behind the running E4B Q4 np32 arm.
#
# MTP is passed but is currently INERT: the server reports speculative=false and
# speculative.types="none" on every slot after logging "[spec] failed to measure
# draft model memory: failed to create llama_context from model". The 4.5x
# measured on E4B Q4 is therefore parallel slots alone. -md is left on so that if
# the draft head starts working the arms pick it up, and the per-arm device.txt
# records whether speculation was live for THAT arm rather than assuming.
#
# E2B Q4 is re-run here because its sequential arm was aborted at ~90 notes. That
# is not a loss: every arm in this ladder now shares one configuration, which is
# what the quant comparison needs. Comparability to the banked SEQUENTIAL E4B Q4
# arm is a separate question, answered by compare_runs.py on the arm running now.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
HOST=192.168.1.253; RIP=192.168.0.5; PORT=8110
SLOTS=${SLOTS:-32}
CTX=${CTX:-98304}
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/ladder_np32.log"; }

# label|gguf|mtp head
LADDER="\
E2B.UD-Q4_K_XL.v8|/opt/hf/e2b-q4.gguf|/opt/hf/mtp-gemma-4-E2B-it-Q8_0.gguf
E2B.UD-Q6_K_XL.v8|/opt/hf/e2b-q6.gguf|/opt/hf/mtp-gemma-4-E2B-it-Q8_0.gguf
E4B.UD-Q6_K_XL.v8|/opt/hf/e4b-q6.gguf|/opt/hf/mtp-gemma-4-E4B-it-Q8_0.gguf
E2B.UD-Q8_K_XL.v8|/opt/hf/e2b-q8.gguf|/opt/hf/mtp-gemma-4-E2B-it-Q8_0.gguf
E4B.UD-Q8_K_XL.v8|/opt/hf/e4b-q8.gguf|/opt/hf/mtp-gemma-4-E4B-it-Q8_0.gguf"

say "waiting for the E4B Q4 np32 arm to finish"
exp=$(wc -l < "$GOLD")
until [ "$(wc -l < "$OUT/E4B.UD-Q4_K_XL.v8.np32mtp.pred.jsonl" 2>/dev/null || echo 0)" -ge "$exp" ]; do
  sleep 30
done
say "starting the ladder at $SLOTS slots"

while IFS='|' read -r label model mtp; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$exp" ]; then say "SKIP $label"; continue; fi
  say "SERVE $model (-np $SLOTS, draft $(basename $mtp))"
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
  sleep 4
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m $model -md $mtp --host 0.0.0.0 --port $PORT -c $CTX -np $SLOTS --no-webui --no-mmproj -ngl 99 >/opt/tierA/arm.log 2>&1 </dev/null &'" >/dev/null 2>&1
  ok=0
  for _ in $(seq 1 90); do
    ssh -n -o ConnectTimeout=10 root@"$HOST" \
      "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
    sleep 10
  done
  if [ "$ok" != 1 ]; then
    say "  $label: would not serve with the draft head; retrying without it"
    ssh -n -o ConnectTimeout=20 root@"$HOST" \
      "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
    sleep 4
    ssh -n -o ConnectTimeout=20 root@"$HOST" \
      "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m $model --host 0.0.0.0 --port $PORT -c $CTX -np $SLOTS --no-webui --no-mmproj -ngl 99 >/opt/tierA/arm.log 2>&1 </dev/null &'" >/dev/null 2>&1
    for _ in $(seq 1 90); do
      ssh -n -o ConnectTimeout=10 root@"$HOST" \
        "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
      sleep 10
    done
    [ "$ok" = 1 ] || { say "FAIL $label: server never healthy"; continue; }
  fi

  # Per-arm provenance: which card, and whether speculation was actually live.
  # "-md was passed" is not evidence that drafting happened.
  { ssh -n -o ConnectTimeout=20 root@"$HOST" \
      "nvidia-smi --query-gpu=index,name,pci.bus_id --format=csv,noheader; \
       nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader";
    echo "--- speculative actually enabled:";
    curl -sf "http://$RIP:$PORT/slots" 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin); s=d[0] if d else {}
print('  slots:', len(d))
print('  speculative:', s.get('speculative'))
print('  spec types :', (s.get('params') or {}).get('speculative.types'))
print('  n_ctx/slot :', s.get('n_ctx'))" 2>/dev/null;
  } > "$OUT/$label.device.txt" 2>&1
  spec=$(grep -m1 "speculative:" "$OUT/$label.device.txt" | awk '{print $2}')
  say "  device recorded; speculative=$spec"

  say "START $label (concurrency $SLOTS)"
  t0=$(date +%s)
  python3 harness/run_llamacpp.py --model "$label" --gold "$GOLD" \
    --thinking --max-tokens 8192 --concurrency "$SLOTS" \
    --out "$pred" --base-url "http://$RIP:$PORT" >>"$OUT/$label.run.log" 2>&1
  t1=$(date +%s)
  got=$(wc -l < "$pred" 2>/dev/null || echo 0)
  if [ "$got" -ge "$exp" ]; then
    say "OK   $label rows=$got wall=$(( (t1-t0)/60 ))m$(( (t1-t0)%60 ))s"
  else
    say "FAIL $label incomplete ($got/$exp)"
  fi
done <<< "$LADDER"
say "=== LADDER COMPLETE ==="
