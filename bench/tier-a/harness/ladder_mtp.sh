#!/bin/bash
# The quant ladder, every arm under MTP.
#
# MTP is the only configuration that is both faster and REPEATABLE: 1.83x, and
# two fresh runs are 100/100 identical on E4B and on E2B. It is NOT identical to
# a sequential run (74/100) and that is fine -- the ladder compares arms to each
# other, so what it needs is one configuration held fixed across all of them.
#
# Concurrency is deliberately 1. 32 slots reached 4.5x and failed the only test
# that matters: two runs of the same config agreed on 75/100 extracted facts,
# because batch composition follows request arrival timing. Adding MTP to it did
# not help and did not stack (4.34x against 4.54x for slots alone).
#
# Every arm therefore re-runs from scratch, including E4B Q4, whose banked arm is
# sequential and cannot be mixed with these.
#
# Order is the operator's: the E2B Q4-vs-Q6 pair first, because that is the
# decision waiting on evidence.
# CT 140 is SHARED. Never `pkill -f llama-server` here: it kills every
# server in the container, including other sessions'. Kill by port.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
mkdir -p "$OUT"
HOST=192.168.1.253; RIP=192.168.0.5; PORT=8117
EXPECT=$(wc -l < "$GOLD")
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/ladder_mtp.log"; }

# label|hf repo:quant|draft repo
LADDER="\
E2B.UD-Q4_K_XL|unsloth/gemma-4-E2B-it-GGUF:UD-Q4_K_XL|unsloth/gemma-4-E2B-it-GGUF
E2B.UD-Q6_K_XL|unsloth/gemma-4-E2B-it-GGUF:UD-Q6_K_XL|unsloth/gemma-4-E2B-it-GGUF
E4B.UD-Q4_K_XL|unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL|unsloth/gemma-4-E4B-it-GGUF
E4B.UD-Q6_K_XL|unsloth/gemma-4-E4B-it-GGUF:UD-Q6_K_XL|unsloth/gemma-4-E4B-it-GGUF
E2B.UD-Q8_K_XL|unsloth/gemma-4-E2B-it-GGUF:UD-Q8_K_XL|unsloth/gemma-4-E2B-it-GGUF
E4B.UD-Q8_K_XL|unsloth/gemma-4-E4B-it-GGUF:UD-Q8_K_XL|unsloth/gemma-4-E4B-it-GGUF"

while IFS='|' read -r label repo draft; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.mtp.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then say "SKIP $label"; continue; fi

  say "SERVE $label  ($repo)"
  ssh -n -o ConnectTimeout=20 root@"$HOST" "pct exec 140 -- bash -lc 'for p in $(seq 8100 8420); do pkill -f \"port $p \" 2>/dev/null; done; true'" >/dev/null 2>&1 || true
  sleep 5
  ssh -n -o ConnectTimeout=25 root@"$HOST" \
    "pct exec 140 -- bash -lc 'HF_HOME=/opt/hf nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -hf $repo -hfd $draft --host 0.0.0.0 --port $PORT -c 8192 --no-webui --no-mmproj -ngl 99 > /opt/tierA/ladder-$label.log 2>&1 </dev/null &'" >/dev/null 2>&1
  ok=0
  # Generous: an uncached quant downloads several GiB before it serves.
  for _ in $(seq 1 160); do
    ssh -n -o ConnectTimeout=10 root@"$HOST" \
      "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
    sleep 15
  done
  if [ "$ok" != 1 ]; then
    say "FAIL $label: never healthy"
    ssh -n -o ConnectTimeout=20 root@"$HOST" "pct exec 140 -- tail -15 /opt/tierA/ladder-$label.log" 2>&1 | sed 's/^/    /' | tee -a "$OUT/ladder_mtp.log"
    continue
  fi

  # Provenance per arm: WHICH model actually loaded (--model is only a label, and
  # a stale server would silently repeat the previous quant), and whether
  # speculation really came up. Both have burned this session already.
  { ssh -n -o ConnectTimeout=15 root@"$HOST" "curl -sf http://$RIP:$PORT/props" 2>/dev/null | python3 -c "
import json,sys; d=json.load(sys.stdin)
print('model_loaded:', (d.get('model_path') or '').split('/')[-1])
print('total_slots :', d.get('total_slots'))" 2>/dev/null
    ssh -n -o ConnectTimeout=15 root@"$HOST" "curl -sf http://$RIP:$PORT/slots" 2>/dev/null | python3 -c "
import json,sys; d=json.load(sys.stdin); s=d[0] if d else {}
print('speculative :', s.get('speculative'))" 2>/dev/null
    ssh -n -o ConnectTimeout=20 root@"$HOST" \
      "nvidia-smi --query-gpu=index,name --format=csv,noheader; nvidia-smi --query-compute-apps=used_memory --format=csv,noheader" 2>/dev/null
  } > "$OUT/$label.mtp.device.txt" 2>&1
  loaded=$(grep model_loaded "$OUT/$label.mtp.device.txt" | cut -d' ' -f2-)
  spec=$(grep speculative "$OUT/$label.mtp.device.txt" | awk '{print $3}')
  say "  loaded=$loaded speculative=$spec"
  if [ "$spec" != "True" ]; then
    say "  WARNING: speculation is OFF for $label -- this arm is not comparable to the others"
  fi

  say "START $label"
  t0=$(date +%s)
  python3 harness/run_llamacpp.py --model "$label" --gold "$GOLD" \
    --thinking --max-tokens 8192 --concurrency 1 \
    --out "$pred" --base-url "http://$RIP:$PORT" >>"$OUT/$label.mtp.run.log" 2>&1
  t1=$(date +%s)
  got=$(wc -l < "$pred" 2>/dev/null || echo 0)
  if [ "$got" -ge "$EXPECT" ]; then
    python3 harness/score.py --gold "$GOLD" --pred "$pred" \
      --json-out "$OUT/$label.mtp.score.json" >/dev/null 2>&1
    f1=$(python3 -c "
import json;print('%.4f'%json.load(open('$OUT/$label.mtp.score.json'))['strict']['f1'])" 2>/dev/null || echo "scorer-refused")
    say "OK   $label rows=$got wall=$(( (t1-t0)/60 ))m$(( (t1-t0)%60 ))s strictF1=$f1"
  else
    say "FAIL $label incomplete ($got/$EXPECT)"
  fi
  # Marker so the E2B pair can be reported without waiting for the rest.
  [ "$label" = "E2B.UD-Q6_K_XL" ] && touch "$OUT/.e2b_pair_done"
done <<< "$LADDER"
say "=== LADDER COMPLETE ==="
