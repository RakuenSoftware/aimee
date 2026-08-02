#!/bin/bash
# v5 vs v7 on ONE corpus, plus a second model, sequentially on the 5080.
#
# Three open items, one sweep:
#
#   E4B v5   } the clean prompt comparison. The existing v5 numbers came from a
#   E4B v7   } slice of gold_large and the v6 numbers from gold_small, so the
#            } two were never comparable -- different notes, different difficulty.
#            } Same corpus, same server, same quant, one variable.
#   E2B v7   } the polarity error rate was measured on ONE model. E2B never had
#            } the thinking defect and behaves differently on retractions, so it
#            } is the right second opinion on whether polarity leaks.
#
# v5 is reconstructed from the live template by prompt_versions.py rather than
# kept as a second copy, so it cannot drift from what production sends.
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

# model-path|label|prompt-version
ARMS="\
/opt/hf/e4b-q4.gguf|E4B.UD-Q4_K_XL.v5|v5
/opt/hf/e4b-q4.gguf|E4B.UD-Q4_K_XL.v7|live
/opt/hf/e2b-q4.gguf|E2B.UD-Q4_K_XL.v7|live"

start_server() {  # <model>
  # Cleanup is a SEPARATE ssh call matching the MODEL PATH, not the port:
  # `pkill -f "port 8110"` matches the launching shell's own command line, so the
  # launcher kills itself. Four incidents of that are in MEASUREMENT_LOG.md.
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

say "=== v5/v7 1k sweep start; gold=$GOLD expect=$EXPECT ==="
CUR=""
while IFS='|' read -r model label version; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then
    say "SKIP $label"; continue
  fi
  if [ "$model" != "$CUR" ]; then
    say "SERVE $model"
    start_server "$model" || { say "FAIL $label: server never healthy"; continue; }
    CUR="$model"
  fi
  say "START $label (prompt=$version)"
  python3 harness/run_llamacpp.py --model "$label" --gold "$GOLD" \
    --thinking --max-tokens 8192 --prompt-version "$version" \
    --out "$pred" --base-url "http://$RIP:$PORT" >>"$OUT/$label.run.log" 2>&1
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
say "=== SWEEP DONE ==="
