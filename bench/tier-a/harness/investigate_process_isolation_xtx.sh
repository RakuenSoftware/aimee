#!/bin/bash
# Is it BATCHING that breaks determinism, or the GPU being busy at all?
#
# Everything so far conflates two different things:
#
#   shared forward pass -- 32 slots batches requests into the SAME GEMM, so the
#     reduction order depends on who else is in the batch. Structurally
#     non-deterministic, and confirmed: breaks at concurrency 2, unaffected by
#     --no-cont-batching or --kv-unified.
#
#   GPU contention -- two SEPARATE processes have separate CUDA contexts and
#     separate batches. Their arithmetic never mixes. They compete for SMs,
#     bandwidth and clocks, nothing more.
#
# If contention alone is harmless, then N single-slot servers on one card gives
# parallelism WITH repeatability, and most of the throughput written off earlier
# comes back. If contention alone also perturbs results, parallelism on a GPU is
# unavailable at any architecture, and the reason is deeper than llama.cpp: it
# would mean kernel selection or reduction order varies with occupancy.
#
# Design: the SAME server config (-np 1) and the SAME 60 notes, run three ways.
#
#   SOLO      one server, nothing else on the card         -> reference
#   SOLO2     the same again, to prove the reference is stable
#   CONTENDED the same server, while a SECOND server on another port hammers the
#             card with its own workload. Compared against SOLO.
#
# Only the second process differs between SOLO and CONTENDED. If SOLO ==
# CONTENDED, isolation is sufficient and the enemy is batching, not parallelism.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
N=${N:-60}
HOST=admin@192.168.1.254
BIN=/mnt/media/tierbench/bin/llama-b10210/llama-server
MODEL=/mnt/media/storage/models/gguf/gemma-4-E4B-it-UD-Q4_K_XL.gguf
PORT=8121        # the measured server
PORT2=8122       # the noisy neighbour
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/isolation_xtx.log"; }

head -n "$N" "$GOLD" > "$OUT/.iso_slice.jsonl"

tunnel() {  # $1 = port; .254 firewalls serving ports off-host
  pkill -f "ssh -N -L $1:" 2>/dev/null; sleep 1
  setsid nohup ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 \
    -L "$1:127.0.0.1:$1" "$HOST" >/dev/null 2>&1 </dev/null &
  for _ in $(seq 1 15); do
    curl -sf --max-time 4 "http://127.0.0.1:$1/health" >/dev/null 2>&1 && return 0
    sleep 2
  done
  return 1
}

start_server() {  # $1 = port
  ssh -n -o ConnectTimeout=25 "$HOST" \
    "nohup setsid $BIN -m $MODEL --host 0.0.0.0 --port $1 -c 8192 -np 1 --device Vulkan1 --no-webui --no-mmproj -ngl 99 > /tmp/iso-$1.log 2>&1 </dev/null &" >/dev/null 2>&1
  for _ in $(seq 1 90); do
    ssh -n -o ConnectTimeout=10 "$HOST" "curl -sf --max-time 5 http://127.0.0.1:$1/health" >/dev/null 2>&1 \
      && { tunnel "$1" && return 0; }
    sleep 10
  done
  return 1
}

kill_all() { ssh -n -o ConnectTimeout=20 "$HOST" "pkill -f llama-server" >/dev/null 2>&1 || true; sleep 5; }

measure() {  # $1 = out suffix
  local t0 t1
  t0=$(date +%s)
  python3 harness/run_llamacpp.py --model "iso.$1" --gold "$OUT/.iso_slice.jsonl" \
    --thinking --max-tokens 8192 --concurrency 1 \
    --out "$OUT/iso_$1.pred.jsonl" --base-url "http://127.0.0.1:$PORT" >/dev/null 2>&1
  t1=$(date +%s)
  say "  $1: $((t1-t0))s"
}

say "=== SOLO: one server, idle card"
kill_all; start_server "$PORT" || { say "server never healthy"; exit 1; }
measure solo1
say "=== SOLO2: same config again, to prove the reference is stable"
kill_all; start_server "$PORT" || { say "server never healthy"; exit 1; }
measure solo2

say "=== CONTENDED: same server, second isolated process hammering the card"
kill_all
start_server "$PORT"  || { say "server 1 never healthy"; exit 1; }
start_server "$PORT2" || { say "server 2 never healthy"; exit 1; }
# The neighbour runs its own workload in a loop for the duration, on its own
# context and its own batches. It shares only the hardware.
ssh -n -o ConnectTimeout=20 "$HOST" \
  "nohup setsid bash -c 'for i in \$(seq 1 400); do curl -sf --max-time 60 -X POST http://127.0.0.1:$PORT2/v1/chat/completions -H \"Content-Type: application/json\" -d \"{\\\"model\\\":\\\"x\\\",\\\"messages\\\":[{\\\"role\\\":\\\"user\\\",\\\"content\\\":\\\"Write a long detailed essay about distributed systems.\\\"}],\\\"max_tokens\\\":512,\\\"temperature\\\":0}\" >/dev/null 2>&1; done' >/dev/null 2>&1 </dev/null &" >/dev/null 2>&1
sleep 20
util=$(ssh -n -o ConnectTimeout=15 "$HOST" "timeout 5 rocm-smi --showuse 2>/dev/null | grep -i 'gpu use' | head -2" 2>/dev/null || echo "")
[ -n "$util" ] && say "  neighbour load: $util"
measure contended
ssh -n -o ConnectTimeout=20 "$HOST" "pkill -f 'seq 1 400'" >/dev/null 2>&1 || true
kill_all

python3 - "$OUT/iso_solo1.pred.jsonl" "$OUT/iso_solo2.pred.jsonl" "$OUT/iso_contended.pred.jsonl" \
  <<'PY' | tee -a "$OUT/isolation_xtx.log"
import json,sys
def L(p): return {json.loads(l)["id"]: json.loads(l) for l in open(p)}
s1,s2,c = L(sys.argv[1]), L(sys.argv[2]), L(sys.argv[3])
ids=[i for i in s1 if i in s2 and i in c]
def eq(a,b): return sum(1 for i in ids if a[i]["raw"]==b[i]["raw"])
r_ss, r_sc = eq(s1,s2), eq(s1,c)
print(f"\n{len(ids)} notes, -np 1 throughout\n")
print(f"  solo   vs solo (control)      : {r_ss}/{len(ids)}")
print(f"  solo   vs CONTENDED           : {r_sc}/{len(ids)}")
print()
if r_ss < len(ids):
    print("  Control failed -- the solo config is not even reproducible here, so")
    print("  nothing can be concluded about contention. Investigate that first.")
elif r_sc == len(ids):
    print("  ISOLATION IS SUFFICIENT. A second process saturating the same GPU does")
    print("  not change the answers. The enemy is a SHARED FORWARD PASS, not")
    print("  parallelism on a GPU.")
    print("  => N single-slot server processes on one card would give parallelism")
    print("     WITH repeatability. That is worth measuring for throughput next.")
else:
    print(f"  CONTENTION ALONE PERTURBS RESULTS: {len(ids)-r_sc} note(s) changed with")
    print("  no shared batch and no shared context. That points below llama.cpp --")
    print("  kernel/algorithm selection or reduction order varying with occupancy.")
    print("  => GPU parallelism is unavailable for this benchmark in ANY form.")
PY
rm -f "$OUT/.iso_slice.jsonl"
say "=== ISOLATION TEST DONE ==="
