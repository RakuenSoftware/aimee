#!/bin/bash
# Root-cause the 32-slot irreproducibility on the idle XTX, so the 5080 keeps
# the ladder to itself.
#
# Established on the 5080 (CUDA): two runs of -np 32 agree on 75/100 extracted
# facts, while concurrency 1 is bit-identical across restarts. "Batching" is a
# description, not a cause. Four candidates, separated by construction:
#
#   A. SLOT COUNT alone   -np 32, ONE request in flight
#   B. CONCURRENCY level  -np 32, 2 / 8 / 32 in flight
#   C. CONTINUOUS BATCHING  -np 32, 32 in flight, --no-cont-batching
#   D. KV LAYOUT          -np 32, 32 in flight, --kv-unified
#
# MTP is deliberately NOT used here: it is a second variable, and the question is
# what concurrency alone does.
#
# TWO CAVEATS that limit what a result here proves:
#
#   Different backend and build. .254 is Vulkan llama-b10210; the ladder runs
#   CUDA b10201-9-g0005475. A mechanism found here needs confirming on CUDA
#   before it is stated as the cause. What it CAN do is tell us which knob
#   matters, since the scheduling logic is backend-independent.
#
#   Device pinning is mandatory. Vulkan0 is a 16GB Phoenix iGPU and Vulkan1 the
#   7900 XTX; llama.cpp takes the first device by default, which is how this lane
#   spent a week measuring an integrated GPU (defect 30). Every trial re-verifies
#   which device is serving, because an earlier probe on this host silently hit a
#   stale server and returned meaningless numbers.
set -u
cd "$(dirname "$0")/.." || exit 1
GOLD=${GOLD:?set GOLD}
OUT=${OUT:?set OUT}
N=${N:-60}
HOST=admin@192.168.1.254
IP=127.0.0.1   # reached through the ssh forward below
BIN=/mnt/media/tierbench/bin/llama-b10210/llama-server
MODEL=/mnt/media/storage/models/gguf/gemma-4-E4B-it-UD-Q4_K_XL.gguf
PORT=8119

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

say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/np32_xtx.log"; }

head -n "$N" "$GOLD" > "$OUT/.xtx_slice.jsonl"

# $1 label  $2 extra server flags  $3 client concurrency
trial() {
  local label="$1" flags="$2" conc="$3"
  for pass in 1 2; do
    ssh -n -o ConnectTimeout=20 "$HOST" "pkill -f 'llama-server' " >/dev/null 2>&1 || true
    sleep 5
    ssh -n -o ConnectTimeout=25 "$HOST" \
      "HF_HOME=/mnt/media/tierbench/hf nohup setsid $BIN -m $MODEL --host 0.0.0.0 --port $PORT $flags --device Vulkan1 --no-webui --no-mmproj -ngl 99 > /tmp/xtx-$label-$pass.log 2>&1 </dev/null &" >/dev/null 2>&1
    local ok=0
    for _ in $(seq 1 90); do
      ssh -n -o ConnectTimeout=10 "$HOST" "curl -sf --max-time 5 http://127.0.0.1:$PORT/health" >/dev/null 2>&1 \
        && { tunnel && ok=1; break; }
      sleep 10
    done
    if [ "$ok" != 1 ]; then say "  $label pass $pass: never healthy"; return; fi
    # Verify it is OUR server on the right card, not a leftover.
    local got
    got=$(curl -sf --max-time 8 "http://$IP:$PORT/props" 2>/dev/null | python3 -c "
import json,sys; d=json.load(sys.stdin)
print((d.get('model_path') or '?').split('/')[-1], d.get('total_slots'))" 2>/dev/null)
    [ "$pass" = 1 ] && say "  $label: serving [$got]"
    python3 harness/run_llamacpp.py --model "xtx.$label" --gold "$OUT/.xtx_slice.jsonl" \
      --thinking --max-tokens 8192 --concurrency "$conc" \
      --out "$OUT/xtx_${label}_$pass.pred.jsonl" --base-url "http://$IP:$PORT" >/dev/null 2>&1
  done
  python3 - "$OUT/xtx_${label}_1.pred.jsonl" "$OUT/xtx_${label}_2.pred.jsonl" "$label" <<'PY' | tee -a "$OUT/np32_xtx.log"
import json,sys
def L(p): return {json.loads(l)["id"]: json.loads(l) for l in open(p)}
try: a,b = L(sys.argv[1]), L(sys.argv[2])
except FileNotFoundError: print(f"  {sys.argv[3]:28s} (missing output)"); raise SystemExit
ids=[i for i in a if i in b]
raw=sum(1 for i in ids if a[i]["raw"]==b[i]["raw"])
def tr(r): return {(str(f.get("subject","")).strip().lower(),str(f.get("relation","")).strip().lower(),
                    str(f.get("object","")).strip().lower()) for f in (r.get("pred_nofloor") or [])}
fac=sum(1 for i in ids if tr(a[i])==tr(b[i]))
v = "REPEATABLE" if raw==len(ids) else ("facts-stable" if fac==len(ids) else "NOT repeatable")
print(f"  {sys.argv[3]:28s} raw {raw:3d}/{len(ids)}  facts {fac:3d}/{len(ids)}  -> {v}")
PY
}

say "=== XTX (Vulkan1), llama-b10210, E4B UD-Q4, no MTP, $N notes, 2 passes each"
say "A. slot count alone"
trial "np32_conc1"       "-c 98304 -np 32"                     1
say "B. concurrency sweep"
trial "np32_conc2"       "-c 98304 -np 32"                     2
trial "np32_conc8"       "-c 98304 -np 32"                     8
trial "np32_conc32"      "-c 98304 -np 32"                    32
say "C. continuous batching off"
trial "np32_c32_nocb"    "-c 98304 -np 32 --no-cont-batching"  32
say "D. unified KV"
trial "np32_c32_kvu"     "-c 98304 -np 32 --kv-unified"        32
say "control: 4 slots, one in flight (the ladder's own config)"
trial "np4_conc1"        "-c 8192"                             1

rm -f "$OUT/.xtx_slice.jsonl"
say "=== XTX SWEEP DONE ==="
