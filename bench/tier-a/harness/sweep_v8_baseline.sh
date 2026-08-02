#!/bin/bash
# The v8 baseline: E4B and E2B on corpus v5, sequentially on the 5080.
#
# Everything measured before this ran against 17 seed relations and the v4
# corpus, where "{service} runs on {host}" was labelled has_hostname. Both
# changed, so every earlier figure is stale even where the finding still holds.
#
# The falsifiable prediction this run exists to test: the novel-predicate rate
# was 22-24% of extracted facts under 17 relations. Seeding the seven the domain
# kept inventing should drop it materially. If it does not, defect 35's fix did
# not do what it claimed.
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

ARMS="\
/opt/hf/e4b-q4.gguf|E4B.UD-Q4_K_XL.v8
/opt/hf/e2b-q4.gguf|E2B.UD-Q4_K_XL.v8"

start_server() {
  # Separate ssh call, matched on the MODEL PATH not the port: `pkill -f "port
  # 8110"` matches the launching shell's own command line and kills the launcher.
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
  sleep 4
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m $1 --host 0.0.0.0 --port $PORT -c 8192 --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
  for _ in $(seq 1 90); do
    ssh -n -o ConnectTimeout=10 root@"$HOST" \
      "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && return 0
    sleep 10
  done
  return 1
}

say "=== v8 baseline start; gold=$GOLD expect=$EXPECT ==="
while IFS='|' read -r model label; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then say "SKIP $label"; continue; fi
  say "SERVE $model"
  start_server "$model" || { say "FAIL $label: server never healthy"; continue; }
  say "START $label"
  python3 harness/run_llamacpp.py --model "$label" --gold "$GOLD" \
    --thinking --max-tokens 8192 --out "$pred" \
    --base-url "http://$RIP:$PORT" >>"$OUT/$label.run.log" 2>&1
  got=$(wc -l < "$pred" 2>/dev/null || echo 0)
  if [ "$got" -lt "$EXPECT" ]; then say "FAIL $label: incomplete ($got/$EXPECT)"; continue; fi
  think=$(python3 -c "
import json
n=t=0
for l in open('$pred'):
    r=json.loads(l); n+=1; t+=(r.get('reasoning_chars') or 0)>0
print(f'{t}/{n}')" 2>/dev/null)
  say "OK   $label rows=$got thinking=$think"
done <<< "$ARMS"
say "=== BASELINE DONE ==="
