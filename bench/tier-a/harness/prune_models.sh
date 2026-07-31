#!/bin/bash
# Drop cached model weights once a model's predictions are committed.
#
# Predictions and scores are the durable artefacts; weights are ~50GB each and
# re-downloadable. Keeping them filled the bench CT twice mid-run — once at 98%,
# which failed a download and cost a retry.
#
# Refuses to touch a model that llama-server currently has open, so this is safe
# to run while a sweep is in flight.
set -u
HF=${HF_HOME:-/opt/hf}/hub
KEEP=${KEEP:-}

in_use() {  # is any live process holding a file under this dir?
  local d=$1
  for pid in $(pgrep -f 'llama-server|run_hf.py|run_llamacpp.py' 2>/dev/null); do
    if ls -l "/proc/$pid/fd" 2>/dev/null | grep -q "$(basename "$d")"; then return 0; fi
  done
  return 1
}

freed=0
for d in "$HF"/models--*; do
  [ -d "$d" ] || continue
  name=$(basename "$d")
  if [ -n "$KEEP" ] && echo "$name" | grep -qiE "$KEEP"; then
    echo "keep   $name"
    continue
  fi
  if in_use "$d"; then
    echo "IN USE $name — skipping"
    continue
  fi
  sz=$(du -sm "$d" 2>/dev/null | cut -f1)
  rm -rf "$d" && { echo "pruned $name (${sz}MB)"; freed=$((freed + sz)); }
done
echo "freed ~${freed}MB"
df -h / | tail -1
