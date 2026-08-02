#!/bin/bash
# Sample which model is serving and how much VRAM it actually holds, once a
# minute. File size overstates it: --no-mmproj skips the vision projector, so
# e4b-q4 is 4.77 GiB on disk and 3.66 GiB resident. The co-residency question
# (can Q4 E2B and Q6 E4B share one 16GB card?) needs the resident number.
cd "$(dirname "$0")/.." || exit 1
OUT=${OUT:?set OUT}
while true; do
  m=$(ssh -n -o ConnectTimeout=10 root@192.168.1.253 \
        "pct exec 140 -- pgrep -a llama-server" 2>/dev/null | grep -oP '(?<=-m )\S+' | head -1)
  v=$(ssh -n -o ConnectTimeout=10 root@192.168.1.253 \
        "nvidia-smi --query-compute-apps=used_memory --format=csv,noheader" 2>/dev/null | head -1)
  [ -n "$m" ] && echo "$(date -u +%H:%M:%SZ) $m $v" >> "$OUT/vram.log"
  sleep 60
done
