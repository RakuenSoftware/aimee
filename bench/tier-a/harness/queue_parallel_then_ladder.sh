#!/bin/bash
# Wait for the sequential baseline, prove whether parallel slots change outputs,
# then run the rest of the quant ladder at whatever concurrency is safe.
#
# The server has been running llama.cpp's DEFAULT 4 slots because -np was never
# passed, while holding 3.7 GiB of a 16 GiB card. At batch=1 the weights are read
# once per token, so the GPU is bandwidth-bound and largely idle on compute;
# serving many sequences reads them once per batch instead. It will not be 32x,
# but 2x alone halves a 40-minute arm.
#
# The determinism check runs on E2B Q4 specifically because the sequential arm
# for exactly that model+quant+corpus finishes immediately before it, so the
# comparison is against real benchmark data rather than a synthetic warm-up.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
HOST=192.168.1.253; RIP=192.168.0.5; PORT=8110
SLOTS=${SLOTS:-32}
# Total context is divided across slots, so ask for enough that a slot still has
# real headroom: prompt is ~600 tokens and completions run ~400 (p95 582), but
# --max-tokens is 8192 and a slot that cannot honour it would truncate. score.py
# REFUSES truncated runs, which is the guard that would catch it.
CTX=${CTX:-98304}

say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/parallel.log"; }

say "waiting for the sequential baseline (E4B Q4, E2B Q4) to finish"
until grep -q "BASELINE DONE" "$OUT/driver.log" 2>/dev/null; do sleep 60; done
say "baseline done"

say "restarting E2B Q4 with -np $SLOTS -c $CTX"
ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
sleep 4
ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m /opt/hf/e2b-q4.gguf --host 0.0.0.0 --port $PORT -c $CTX -np $SLOTS $SERVER_FLAGS --no-webui --no-mmproj -ngl 99 >/opt/tierA/e2b-q4-np.log 2>&1 </dev/null &'" >/dev/null 2>&1
ok=0
for _ in $(seq 1 90); do
  ssh -n -o ConnectTimeout=10 root@"$HOST" \
    "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
  sleep 10
done
[ "$ok" = 1 ] || { say "FAIL: server with $SLOTS slots never became healthy"; exit 1; }

ssh -n -o ConnectTimeout=20 root@"$HOST" \
  "nvidia-smi --query-compute-apps=used_memory --format=csv,noheader; \
   nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader" \
  > "$OUT/parallel.vram.txt" 2>&1
say "VRAM with $SLOTS slots: $(cat "$OUT/parallel.vram.txt" | tr '\n' ' ')"

# Three server configurations, cheapest-first. Greedy decoding is NOT the lever
# here -- argmax is already the most reproducible map from logits to tokens, and
# sampling would add randomness on top. The logits themselves differ because
# batch composition changes matmul reduction order and float addition is not
# associative. These flags reduce how much composition varies.
SERVER_FLAGS=""
BEST_FLAGS=""; BEST_SPD=1
for variant in "default" "--no-kv-unified" "--no-kv-unified --no-cont-batching"; do
  [ "$variant" = "default" ] && vf="" || vf="$variant"
  say "testing $SLOTS slots with flags: ${vf:-<none>}"
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
  sleep 4
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m /opt/hf/e2b-q4.gguf -md /opt/hf/mtp-gemma-4-E2B-it-Q8_0.gguf --host 0.0.0.0 --port $PORT -c $CTX -np $SLOTS $vf --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
  ok=0
  for _ in $(seq 1 90); do
    ssh -n -o ConnectTimeout=10 root@"$HOST" \
      "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
    sleep 10
  done
  [ "$ok" = 1 ] || { say "  server would not start with '${vf:-<none>}'; skipping"; continue; }
  tag=$(echo "${vf:-default}" | tr ' -' '__')
  python3 harness/check_parallel_determinism.py \
    --base-url "http://$RIP:$PORT" --model E2B.UD-Q4_K_XL \
    --gold "$GOLD" --n 48 --concurrency "$SLOTS" \
    --out "$OUT/determinism$tag.json" 2>&1 | tee -a "$OUT/parallel.log"
  read -r id spd <<< "$(python3 -c "
import json;d=json.load(open('$OUT/determinism$tag.json'))
print(1 if d['identical']==d['n'] else 0, d['speedup'])" 2>/dev/null || echo '0 1')"
  say "  -> identical=$id speedup=${spd}x"
  if [ "$id" = "1" ]; then BEST_FLAGS="$vf"; BEST_SPD=$spd; break; fi
done

if [ -n "${BEST_FLAGS+x}" ] && [ "$BEST_SPD" != "1" ]; then
  CONC=$SLOTS; SERVER_FLAGS="$BEST_FLAGS"
  say "outputs identical with '${SERVER_FLAGS:-<none>}'; ladder runs at concurrency $CONC (${BEST_SPD}x)"
else
  # Identical-to-sequential could not be had. That does NOT force sequential:
  # the ladder compares quants to EACH OTHER, so identical conditions across
  # arms is what it needs. Parallel is kept and the caveat recorded -- these
  # arms are mutually comparable but not directly comparable to the sequential
  # E4B Q4 / E2B Q4 arms already banked.
  CONC=$SLOTS; SERVER_FLAGS="--no-kv-unified"
  say "no flag combination reproduced sequential output. Keeping concurrency $CONC"
  say "CAVEAT: ladder arms are comparable to each other, NOT to the sequential arms"
fi

# label|gguf|mtp draft head. The draft head is per-family, so E2B arms draft
# with the E2B head and E4B arms with the E4B head; a mismatched vocab would be
# rejected at load.
LADDER="\
E2B.UD-Q6_K_XL.v8|/opt/hf/e2b-q6.gguf|/opt/hf/mtp-gemma-4-E2B-it-Q8_0.gguf
E4B.UD-Q6_K_XL.v8|/opt/hf/e4b-q6.gguf|/opt/hf/mtp-gemma-4-E4B-it-Q8_0.gguf
E2B.UD-Q8_K_XL.v8|/opt/hf/e2b-q8.gguf|/opt/hf/mtp-gemma-4-E2B-it-Q8_0.gguf
E4B.UD-Q8_K_XL.v8|/opt/hf/e4b-q8.gguf|/opt/hf/mtp-gemma-4-E4B-it-Q8_0.gguf"

while IFS='|' read -r label model mtp; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  exp=$(wc -l < "$GOLD")
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$exp" ]; then say "SKIP $label"; continue; fi
  say "SERVE $model (-np $SLOTS, MTP $(basename $mtp))"
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
  sleep 4
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m $model -md $mtp --host 0.0.0.0 --port $PORT -c $CTX -np $SLOTS $SERVER_FLAGS --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
  ok=0
  for _ in $(seq 1 90); do
    ssh -n -o ConnectTimeout=10 root@"$HOST" \
      "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
    sleep 10
  done
  if [ "$ok" != 1 ]; then
    # A draft head that will not load must not cost the whole arm. Retry without
    # it and record that this arm ran unspeculated, so the timing is not compared
    # against the arms that did.
    say "  $label: would not serve with MTP; retrying without the draft head"
    ssh -n -o ConnectTimeout=20 root@"$HOST" \
      "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
    sleep 4
    ssh -n -o ConnectTimeout=20 root@"$HOST" \
      "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m $model --host 0.0.0.0 --port $PORT -c $CTX -np $SLOTS $SERVER_FLAGS --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
    for _ in $(seq 1 90); do
      ssh -n -o ConnectTimeout=10 root@"$HOST" \
        "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && { ok=1; break; }
      sleep 10
    done
    [ "$ok" = 1 ] && say "  $label: serving WITHOUT MTP" || { say "FAIL $label: server never healthy"; continue; }
  fi
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "nvidia-smi --query-gpu=index,name,pci.bus_id --format=csv,noheader; \
     nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader" \
    > "$OUT/$label.device.txt" 2>&1
  say "START $label (concurrency $CONC)"
  python3 harness/run_llamacpp.py --model "$label" --gold "$GOLD" \
    --thinking --max-tokens 8192 --concurrency "$CONC" \
    --out "$pred" --base-url "http://$RIP:$PORT" >>"$OUT/$label.run.log" 2>&1
  got=$(wc -l < "$pred" 2>/dev/null || echo 0)
  [ "$got" -ge "$exp" ] && say "OK   $label rows=$got" || say "FAIL $label incomplete ($got/$exp)"
done <<< "$LADDER"
say "=== LADDER COMPLETE ==="
