#!/bin/bash
# Does speculation HURT under load, or merely stop helping?
#
# The claim that MTP + 32 slots is "slower than slots alone" (4.34x vs 4.54x)
# does not survive inspection: those came from different sample sizes, and two
# runs of the SAME 32-slot config took 71 s and 61 s -- a 16% spread that is
# wider than the gap being claimed. There is currently no resolution to tell
# them apart, and reporting it as a slowdown was wrong.
#
# There IS a mechanism by which speculation can lose at high batch, which is why
# this is worth measuring rather than assuming either way:
#
#   at batch=1  the GPU is bandwidth-bound with compute idle; drafting fills it
#   at batch=32 the pipeline is far closer to saturated, and speculation ADDS
#               compute -- a draft pass per sequence per step, plus verifying
#               n+1 positions instead of 1. If acceptance is low, that is spent
#               for nothing.
#
# So the outcome hinges on ACCEPTANCE RATE, which has never been measured here.
#
# 2x2, three repeats per cell, because the noise floor is ~16%. One repeat per
# cell would reproduce exactly the mistake this is correcting.
#
# On the XTX so the 5080 keeps the ladder. Vulkan1 pinned -- Vulkan0 is a 16GB
# Phoenix iGPU and llama.cpp takes the first device by default (defect 30).
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
N=${N:-60}
REPEATS=${REPEATS:-3}
HOST=admin@192.168.1.254
IP=127.0.0.1   # reached through the ssh forward below
BIN=/mnt/media/tierbench/bin/llama-b10210/llama-server
PORT=8120
REPO=unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL
DRAFT=unsloth/gemma-4-E4B-it-GGUF

# .254 firewalls its serving ports off-host: ssh is reachable, 8119/8120 are not,
# and the earlier sweep failed every trial because the health check curled the LAN
# address from this machine. Forward the port instead, the way overnight_10k.sh
# already does for this host.
tunnel() {
  pkill -f "ssh -N -L $PORT:" 2>/dev/null; sleep 1
  setsid nohup ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 \
    -L "$PORT:127.0.0.1:$PORT" "$HOST" >/dev/null 2>&1 </dev/null &
  for _ in $(seq 1 15); do
    curl -sf --max-time 4 "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && return 0
    sleep 2
  done
  return 1
}

say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/mtp_matrix_xtx.log"; }

say "waiting for the XTX determinism sweep to finish"
until grep -q "XTX SWEEP DONE" "$OUT/np32_xtx.log" 2>/dev/null; do sleep 60; done
say "sweep done; starting the speed matrix"

head -n "$N" "$GOLD" > "$OUT/.mx_slice.jsonl"
: > "$OUT/.mx_results"

# $1 label  $2 use_mtp(0|1)  $3 slots  $4 concurrency
cell() {
  local label="$1" mtp="$2" slots="$3" conc="$4"
  local spec_arg="" ctx=8192
  [ "$mtp" = 1 ] && spec_arg="-hfd $DRAFT"
  [ "$slots" -gt 4 ] && ctx=$((slots*3072))
  for r in $(seq 1 "$REPEATS"); do
    ssh -n -o ConnectTimeout=20 "$HOST" "pkill -f llama-server" >/dev/null 2>&1 || true
    sleep 5
    ssh -n -o ConnectTimeout=25 "$HOST" \
      "HF_HOME=/mnt/media/tierbench/hf nohup setsid $BIN -hf $REPO $spec_arg --host 0.0.0.0 --port $PORT -c $ctx -np $slots --device Vulkan1 --no-webui --no-mmproj -ngl 99 > /tmp/mx-$label-$r.log 2>&1 </dev/null &" >/dev/null 2>&1
    local ok=0
    for _ in $(seq 1 120); do
      ssh -n -o ConnectTimeout=10 "$HOST" "curl -sf --max-time 5 http://127.0.0.1:$PORT/health" >/dev/null 2>&1 \
        && { tunnel && ok=1; break; }
      sleep 15
    done
    [ "$ok" = 1 ] || { say "  $label r$r: never healthy"; continue; }
    if [ "$r" = 1 ]; then
      local spec
      spec=$(curl -sf --max-time 8 "http://$IP:$PORT/slots" 2>/dev/null | python3 -c "
import json,sys; d=json.load(sys.stdin); s=d[0] if d else {}
print('slots=%s speculative=%s' % (len(d), s.get('speculative')))" 2>/dev/null)
      say "  $label: $spec"
    fi
    local t0 t1
    t0=$(date +%s)
    python3 harness/run_llamacpp.py --model "mx.$label" --gold "$OUT/.mx_slice.jsonl" \
      --thinking --max-tokens 8192 --concurrency "$conc" \
      --out "$OUT/mx_${label}_$r.pred.jsonl" --base-url "http://$IP:$PORT" >/dev/null 2>&1
    t1=$(date +%s)
    echo "$label $((t1-t0))" >> "$OUT/.mx_results"
    say "    r$r: $((t1-t0))s"
  done
  # Acceptance rate, if the build reports it at all -- never measured here before.
  ssh -n -o ConnectTimeout=15 "$HOST" \
    "grep -iE 'accept|draft.*n_|stats' /tmp/mx-$label-1.log 2>/dev/null | tail -4" \
    2>/dev/null | sed 's/^/      /' | tee -a "$OUT/mtp_matrix_xtx.log"
}

cell "nomtp_c1"   0 4  1
cell "mtp_c1"     1 4  1
cell "nomtp_c32"  0 32 32
cell "mtp_c32"    1 32 32

python3 - "$OUT/.mx_results" <<'PY' | tee -a "$OUT/mtp_matrix_xtx.log"
import sys, statistics, collections
d=collections.defaultdict(list)
for line in open(sys.argv[1]):
    k,v = line.split(); d[k].append(int(v))
print("\nwall seconds per cell (lower is better)\n")
print(f"  {'cell':14s} {'runs':>22s}  {'median':>7s}  {'spread':>7s}")
for k in ("nomtp_c1","mtp_c1","nomtp_c32","mtp_c32"):
    if k not in d: continue
    v=d[k]; med=statistics.median(v)
    spread = (max(v)-min(v))/max(med,1)*100
    print(f"  {k:14s} {str(v):>22s}  {med:7.0f}  {spread:6.0f}%")
def med(k): return statistics.median(d[k]) if d.get(k) else None
b1,m1,b32,m32 = med("nomtp_c1"), med("mtp_c1"), med("nomtp_c32"), med("mtp_c32")
print()
if b1 and m1: print(f"  MTP at concurrency 1  : {b1/m1:.2f}x")
if b32 and m32: print(f"  MTP at concurrency 32 : {b32/m32:.2f}x")
if b1 and b32: print(f"  32 slots, no MTP      : {b1/b32:.2f}x")
print()
if b32 and m32:
    if m32 > b32 * 1.05:
        print("  Speculation is a NET LOSS under load: it costs more than it saves once")
        print("  the pipeline is full, which is only possible if acceptance is low.")
    elif m32 < b32 * 0.95:
        print("  Speculation still pays at 32 slots. The earlier 'slower' claim was noise.")
    else:
        print("  Indistinguishable at 32 slots given this spread -- speculation neither")
        print("  helps nor hurts once the pipeline is full. Reporting either would be")
        print("  reading past the resolution of the measurement.")
PY
rm -f "$OUT/.mx_slice.jsonl"
say "=== MATRIX DONE ==="
