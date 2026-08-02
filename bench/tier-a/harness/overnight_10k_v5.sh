#!/bin/bash
# The v5 10k pair: E4B and E2B, both on .253/CUDA, sequential.
#
# Two things differ from overnight_10k.sh and both are deliberate.
#
# 1. PROMPT v5. Every E4B figure in this benchmark was taken with reasoning
#    suppressed by the prompt while the run recorded thinking:true (defect 31),
#    so no existing E4B number is comparable to an E2B one. This run replaces
#    them.
#
# 2. BOTH FAMILIES ON ONE HOST. overnight_10k.sh put E2B on .254 and E4B on
#    .253 to get parallelism, and was explicit that cross-family comparison
#    across hosts is not valid. Defect 30 then showed .254 had been serving from
#    an 8GB iGPU the whole time, so those E2B numbers were not merely
#    incomparable, they were taken on undisclosed hardware.
#
#    Running both families on the same card, same backend, same llama.cpp build
#    makes E2B-vs-E4B a controlled comparison for the first time. That is the
#    comparison the article rests on, so it is worth the wall-clock: the arms run
#    sequentially at roughly 4.5h each rather than in parallel at ~5h total.
#
# Restartable, same as the original: an arm whose prediction file already has the
# full row count is skipped, so re-running after a crash resumes.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD to the corpus}
OUT=${OUT:?set OUT to the results dir}
EXPECT=$(wc -l < "$GOLD")
mkdir -p "$OUT"
LOG="$OUT/driver.log"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" >> "$LOG"; }

HOST=192.168.1.253
RIP=192.168.0.5
PORT=8110

# model-path|label. Q4 for both, matching the v4 10k baseline this replaces, so
# the v4->v5 delta is the only thing moving.
ARMS="\
/opt/hf/e4b-q4.gguf|E4B.UD-Q4_K_XL
/opt/hf/e2b-q4.gguf|E2B.UD-Q4_K_XL"

start_server() {  # <model>
  local model=$1
  # Cleanup is a SEPARATE ssh call and matches on the model path, not the port:
  # `pkill -f "port 8110"` matches the launching shell's own command line, so the
  # launcher kills itself and the lane sits at "loading" forever. That has
  # happened four times in this harness; see MEASUREMENT_LOG.md.
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
  sleep 4
  ssh -n -o ConnectTimeout=20 root@"$HOST" \
    "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m $model --host 0.0.0.0 --port $PORT -c 8192 --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
  for _ in $(seq 1 90); do
    ssh -n -o ConnectTimeout=10 root@"$HOST" \
      "curl -sf --max-time 5 http://$RIP:$PORT/health" >/dev/null 2>&1 && return 0
    sleep 10
  done
  return 1
}

tunnel() {
  pkill -f "ssh -N -L $PORT:" 2>/dev/null; sleep 1
  setsid nohup ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 \
    -o ServerAliveCountMax=3 -L "$PORT:$RIP:$PORT" root@"$HOST" \
    >/dev/null 2>&1 </dev/null &
  for _ in $(seq 1 15); do
    curl -sf --max-time 4 "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && return 0
    sleep 2
  done
  return 1
}

say "=== v5 10k pair start; gold=$GOLD expect=$EXPECT prompt=v5 ==="

# Every ssh below uses -n. Without it ssh reads the while-loop's stdin, which is
# the arm list itself, and each lane runs exactly one arm while reporting DONE.
while IFS='|' read -r model label; do
  [ -n "${label:-}" ] || continue
  pred="$OUT/$label.pred.jsonl"
  if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then
    say "SKIP $label (already $EXPECT rows)"; continue
  fi
  say "START $label"
  if ! start_server "$model"; then say "FAIL $label: server never healthy"; continue; fi
  if ! tunnel; then say "FAIL $label: tunnel down"; continue; fi

  for attempt in 1 2; do
    python3 harness/run_llamacpp.py --model "$label" --gold "$GOLD" \
      --thinking --max-tokens 8192 --out "$pred" \
      --base-url "http://127.0.0.1:$PORT" >>"$OUT/$label.run.log" 2>&1
    got=$(wc -l < "$pred" 2>/dev/null || echo 0)
    [ "$got" -ge "$EXPECT" ] && break
    say "RETRY $label (got $got/$EXPECT, attempt $attempt)"
    tunnel || true
  done

  got=$(wc -l < "$pred" 2>/dev/null || echo 0)
  if [ "$got" -ge "$EXPECT" ]; then
    # A run that came back without reasoning is the defect-31 failure recurring,
    # and it is silent by nature: valid JSON, clean parse, a plausible score. It
    # is checked here rather than left for whoever reads the number later.
    think=$(python3 -c "
import json,sys
n=t=0
for l in open('$pred'):
    r=json.loads(l); n+=1; t+= (r.get('reasoning_chars') or 0)>0
print(f'{t}/{n}')" 2>/dev/null)
    say "THINK $label $think"
    python3 harness/score.py --gold "$GOLD" --pred "$pred" \
      --json-out "$OUT/$label.score.json" >/dev/null 2>&1
    if [ -s "$OUT/$label.score.json" ]; then
      say "OK   $label  F1=$(python3 -c "import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null)"
    else
      say "FAIL $label: scorer refused"
      rm -f "$pred"
    fi
  else
    say "FAIL $label: incomplete ($got/$EXPECT)"
  fi
done <<< "$ARMS"

say "=== v5 PAIR DONE ==="
