#!/bin/bash
# Six 10k arms across two GPUs, unattended.
#
#   5080 / CUDA   (.253 container, via tunnel :8110)   E4B Q4 -> Q6 -> Q8
#   7900 XTX      (.254 Vulkan1,   via tunnel :8111)   E2B Q4 -> Q6 -> Q8
#
# SEQUENTIAL WITHIN EACH HOST, PARALLEL ACROSS THEM. Measured, not assumed:
# two models on one card gave 1.13x, because token generation at batch=1 is
# memory-bandwidth-bound and co-resident models contend for the same bandwidth.
# Client-side batching reached 1.83x but CHANGED OUTPUTS on 3 of 20 notes
# (has_role vs works_for; one note where batching returned {"facts":[]} and
# sequential extracted a triple), so it cannot be used for a benchmark trying to
# resolve ~0.01 effects. Two separate cards are genuinely parallel hardware.
#
# Each family stays on ONE host so the comparisons that matter — E2B quants
# against each other, E4B quants against each other — are same-host, same-
# backend, one variable. Cross-family comparison across hosts is NOT valid and
# nothing here produces one.
#
# Restartable: an arm whose prediction file already has the full row count is
# skipped, so re-running after a crash resumes rather than restarts.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD to the corpus}
OUT=${OUT:?set OUT to the results dir}
EXPECT=$(wc -l < "$GOLD")
mkdir -p "$OUT"
LOG="$OUT/driver.log"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" >> "$LOG"; }

# host|port|remote-ip|model-path|label
ARMS_5080="\
192.168.1.253|8110|192.168.0.5|/opt/hf/e4b-q4.gguf|E4B.UD-Q4_K_XL
192.168.1.253|8110|192.168.0.5|/opt/hf/e4b-q6.gguf|E4B.UD-Q6_K_XL
192.168.1.253|8110|192.168.0.5|/opt/hf/e4b-q8.gguf|E4B.UD-Q8_K_XL"
ARMS_XTX="\
192.168.1.254|8111|127.0.0.1|/mnt/media/storage/models/gguf/gemma-4-E2B-it-UD-Q4_K_XL.gguf|E2B.UD-Q4_K_XL
192.168.1.254|8111|127.0.0.1|/mnt/media/storage/models/gguf/gemma-4-E2B-it-UD-Q6_K_XL.gguf|E2B.UD-Q6_K_XL
192.168.1.254|8111|127.0.0.1|/mnt/media/storage/models/gguf/gemma-4-E2B-it-UD-Q8_K_XL.gguf|E2B.UD-Q8_K_XL"

start_server() {  # <host> <remote-ip> <port> <model> <kind>
  local host=$1 rip=$2 port=$3 model=$4 kind=$5
  # CLEANUP IS A SEPARATE SSH CALL, and it matches on the MODEL PATH rather than
  # the port. Both details are load-bearing. Killing in the same command that
  # launches meant `pkill -f "port 8110"` matched the launching shell's own
  # command line — which contains "port 8110" — so the launcher killed itself
  # before starting anything, and both lanes sat at "loading" forever with no
  # server running. MEASUREMENT_LOG.md records three earlier incidents of exactly
  # this self-match; this was the fourth.
  if [ "$kind" = "pct" ]; then
    ssh -n -o ConnectTimeout=20 root@"$host" \
      "pct exec 140 -- pkill -f 'llama-server -m /opt/hf/'" >/dev/null 2>&1 || true
    sleep 4
    ssh -n -o ConnectTimeout=20 root@"$host" "pct exec 140 -- bash -lc 'nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -m $model --host 0.0.0.0 --port $port -c 8192 --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
  else
    # Vulkan1 is the discrete 7900 XTX. Vulkan0 is an 8GB Phoenix iGPU, and
    # llama.cpp takes the FIRST device by default — which is how this lane spent
    # a week measuring an integrated GPU (defect 30). The pin is load-bearing.
    ssh -n -o ConnectTimeout=20 admin@"$host" \
      "pkill -f 'llama-server -m /mnt/media/storage'" >/dev/null 2>&1 || true
    sleep 4
    ssh -n -o ConnectTimeout=20 admin@"$host" "HF_HOME=/mnt/media/tierbench/hf nohup setsid /mnt/media/tierbench/bin/llama-b10210/llama-server -m $model --host 0.0.0.0 --port $port -c 8192 --no-webui --no-mmproj -ngl 99 --device Vulkan1 >/dev/null 2>&1 </dev/null &" >/dev/null 2>&1
  fi
  local user=root; [ "$kind" = pct ] || user=admin
  for _ in $(seq 1 90); do
    if [ "$kind" = pct ]; then
      ssh -n -o ConnectTimeout=10 root@"$host" "curl -sf --max-time 5 http://$rip:$port/health" >/dev/null 2>&1 && return 0
    else
      ssh -n -o ConnectTimeout=10 admin@"$host" "curl -sf --max-time 5 http://127.0.0.1:$port/health" >/dev/null 2>&1 && return 0
    fi
    sleep 10
  done
  return 1
}

tunnel() {  # <host> <remote-ip> <port> <user>
  local host=$1 rip=$2 port=$3 user=$4
  pkill -f "ssh -N -L $port:" 2>/dev/null; sleep 1
  setsid nohup ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 \
    -o ServerAliveCountMax=3 -L "$port:$rip:$port" "$user@$host" \
    >/dev/null 2>&1 </dev/null &
  for _ in $(seq 1 15); do
    curl -sf --max-time 4 "http://127.0.0.1:$port/health" >/dev/null 2>&1 && return 0
    sleep 2
  done
  return 1
}

run_lane() {  # <arm-list> <kind> <user>
  # Every ssh here uses -n. Without it ssh reads the while-loop's stdin, which is
  # the arm list itself: the first ssh swallowed the remaining two lines, each
  # lane ran exactly ONE arm, and the driver reported "LANE DONE" as though it
  # had finished all three.
  local arms=$1 kind=$2 user=$3
  while IFS='|' read -r host port rip model label; do
    [ -n "${label:-}" ] || continue
    local pred="$OUT/$label.pred.jsonl"
    if [ -s "$pred" ] && [ "$(wc -l < "$pred")" -ge "$EXPECT" ]; then
      say "SKIP $label (already $EXPECT rows)"; continue
    fi
    say "START $label on $host"
    if ! start_server "$host" "$rip" "$port" "$model" "$kind"; then
      say "FAIL $label: server never healthy"; continue
    fi
    if ! tunnel "$host" "$rip" "$port" "$user"; then
      say "FAIL $label: tunnel down"; continue
    fi
    # Retry once: a dropped tunnel mid-run is the likeliest overnight failure,
    # and run_llamacpp.py rewrites the file from scratch so a partial is safe to
    # discard.
    for attempt in 1 2; do
      python3 harness/run_llamacpp.py --model "$label" --gold "$GOLD" \
        --thinking --max-tokens 8192 --out "$pred" \
        --base-url "http://127.0.0.1:$port" >>"$OUT/$label.run.log" 2>&1
      local got; got=$(wc -l < "$pred" 2>/dev/null || echo 0)
      if [ "$got" -ge "$EXPECT" ]; then break; fi
      say "RETRY $label (got $got/$EXPECT, attempt $attempt)"
      tunnel "$host" "$rip" "$port" "$user" || true
    done
    local got; got=$(wc -l < "$pred" 2>/dev/null || echo 0)
    if [ "$got" -ge "$EXPECT" ]; then
      python3 harness/score.py --gold "$GOLD" --pred "$pred" \
        --json-out "$OUT/$label.score.json" >/dev/null 2>&1
      if [ -s "$OUT/$label.score.json" ]; then
        say "OK   $label  F1=$(python3 -c "import json;print('%.4f'%json.load(open('$OUT/$label.score.json'))['strict']['f1'])" 2>/dev/null)"
      else
        say "FAIL $label: scorer refused"
      fi
    else
      say "FAIL $label: incomplete ($got/$EXPECT)"
    fi
  done <<< "$arms"
  say "LANE DONE ($kind)"
}

say "=== overnight 10k start; gold=$GOLD expect=$EXPECT ==="
run_lane "$ARMS_5080" pct root &
P1=$!
run_lane "$ARMS_XTX" ssh admin &
P2=$!
wait $P1 $P2
say "=== ALL LANES DONE ==="
