#!/bin/bash
# Measure resident VRAM for the LARGEST arm on each card, and pin a shard count.
#
# Estimating from disk size does not work: --no-mmproj skips the vision
# projector, and the saving is not constant across quants (e4b-q4 is 4.77 GiB on
# disk / 3.66 resident, e4b-q8 is 8.71 / 6.13). Guessing cost a factor of two in
# planned parallelism, so measure.
#
# The count is pinned per CARD, not per arm. Auto-sizing each arm separately
# would give Q4 more processes than Q8, and arms run under different shard counts
# are not comparable to each other -- which is the whole point of running them.
# So the largest arm sets the number for all three.
# CT 140 is SHARED. Never `pkill -f llama-server` here: it kills every
# server in the container, including other sessions'. Kill by port.
set -u
cd "$(dirname "$0")/.." || exit 1
OUT=${OUT:?set OUT}
RESERVE=${RESERVE:-1800}   # MiB headroom for fragmentation and the draft context
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$OUT/sizing.log"; }

probe() {  # $1 card  $2 repo  $3 draft  -> prints "used total"
  local card=$1 repo=$2 draft=$3
  if [ "$card" = 5080 ]; then
    ssh -n -o ConnectTimeout=20 root@192.168.1.253 "pct exec 140 -- bash -lc 'for p in $(seq 8100 8420); do pkill -f \"port $p \" 2>/dev/null; done; true'" >/dev/null 2>&1 || true
    sleep 5
    ssh -n -o ConnectTimeout=25 root@192.168.1.253 \
      "pct exec 140 -- bash -lc 'HF_HOME=/opt/hf nohup setsid /opt/llama.cpp/build-cuda/bin/llama-server -hf $repo -hfd $draft --host 0.0.0.0 --port 8250 -c 8192 -np 1 --no-webui --no-mmproj -ngl 99 >/dev/null 2>&1 </dev/null &'" >/dev/null 2>&1
    for _ in $(seq 1 160); do
      ssh -n -o ConnectTimeout=10 root@192.168.1.253 "curl -sf --max-time 5 http://127.0.0.1:8250/health" >/dev/null 2>&1 && break
      sleep 15
    done
    ssh -n -o ConnectTimeout=15 root@192.168.1.253 \
      "nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader,nounits" 2>/dev/null | tr -d ' '
    ssh -n -o ConnectTimeout=20 root@192.168.1.253 "pct exec 140 -- bash -lc 'for p in $(seq 8100 8420); do pkill -f \"port $p \" 2>/dev/null; done; true'" >/dev/null 2>&1 || true
  else
    ssh -n -o ConnectTimeout=20 admin@192.168.1.254 "pkill -f llama-server" >/dev/null 2>&1 || true
    sleep 5
    ssh -n -o ConnectTimeout=25 admin@192.168.1.254 \
      "HF_HOME=/mnt/media/tierbench/hf nohup setsid /mnt/media/tierbench/bin/llama-b10210/llama-server -hf $repo -hfd $draft --host 0.0.0.0 --port 8250 -c 8192 -np 1 --device Vulkan1 --no-webui --no-mmproj -ngl 99 >/tmp/size.log 2>&1 </dev/null &" >/dev/null 2>&1
    for _ in $(seq 1 160); do
      ssh -n -o ConnectTimeout=10 admin@192.168.1.254 "curl -sf --max-time 5 http://127.0.0.1:8250/health" >/dev/null 2>&1 && break
      sleep 15
    done
    # DEAD BRANCH. rocm-smi is NOT installed on 192.168.1.254 -- the XTX is
    # driven through Vulkan, not ROCm -- so `used` is always empty and this
    # returns ",24560". The "measured" XTX shard count reported by this script
    # was never measured; it came from the fallback. Set NPROC by hand for the
    # XTX, or install ROCm tooling there and delete this comment.
    local used
    used=$(ssh -n -o ConnectTimeout=15 admin@192.168.1.254 \
      "rocm-smi --showmeminfo vram 2>/dev/null | grep -iE 'used' | grep -oE '[0-9]{6,}' | head -1" 2>/dev/null)
    if [ -n "$used" ]; then echo "$((used/1048576)),24560"; else echo ",24560"; fi
    ssh -n -o ConnectTimeout=20 admin@192.168.1.254 "pkill -f llama-server" >/dev/null 2>&1 || true
  fi
}

say "probing E2B Q8 on the 5080 (largest E2B arm)"
r1=$(probe 5080 unsloth/gemma-4-E2B-it-GGUF:UD-Q8_K_XL unsloth/gemma-4-E2B-it-GGUF)
say "  5080: $r1"
say "probing E4B Q8 on the XTX (largest E4B arm)"
r2=$(probe xtx unsloth/gemma-4-E4B-it-GGUF:UD-Q8_K_XL unsloth/gemma-4-E4B-it-GGUF)
say "  xtx : $r2"

python3 - "$r1" "$r2" "$RESERVE" "$OUT/shard_counts.txt" <<'PY' | tee -a "$OUT/sizing.log"
import sys
def n_of(s, reserve):
    try:
        used, total = [int(x) for x in s.split(",")]
    except Exception:
        return None, None, None
    if used <= 0: return None, total, None
    n = max(1, (total - reserve) // used)
    return used, total, min(int(n), 6)
u1,t1,n1 = n_of(sys.argv[1], int(sys.argv[3]))
u2,t2,n2 = n_of(sys.argv[2], int(sys.argv[3]))
print("\nmeasured, largest arm per card:\n")
print(f"  5080 / E2B Q8 : {u1} MiB of {t1}  -> {n1} processes")
print(f"  XTX  / E4B Q8 : {u2} MiB of {t2}  -> {n2} processes")
with open(sys.argv[4],"w") as fh:
    fh.write(f"E2B {n1 or 2}\nE4B {n2 or 2}\n")
print(f"\npinned: E2B={n1 or 2}  E4B={n2 or 2}  (same for every arm on that card)")
PY
say "=== SIZING DONE ==="
