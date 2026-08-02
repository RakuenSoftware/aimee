#!/bin/bash
# Drop cached model weights once a model's predictions are committed.
#
# Predictions and scores are the durable artefacts; weights are ~50GB each and
# re-downloadable. Keeping them filled the bench CT twice mid-run — once at 98%,
# which failed a download and cost a retry.
#
# Refuses to touch a model that llama-server currently has open, so this is safe
# to run while a sweep is in flight.
# ARCHIVE FIRST, THEN DELETE. Deleting a weight that exists nowhere else turns a
# 30-second LAN copy into a WAN re-download, and one afternoon paid that ~30GB:
# a six-arm quant sweep re-fetched every arm, E4B was unavailable hours later
# because an earlier sweep had pruned it, and moving E2B Q6 between hosts needed
# a 4.7GB copy over two SSH hops. Disk pressure on a 240GB bench CT is real, but
# the answer is a durable copy elsewhere rather than no copy at all.
#
# ARCHIVE points at the shared store (bench/MODEL_STORE.md). If it is unset or
# unreachable, this script now REFUSES to delete rather than silently destroying
# the only copy — a prune that cannot archive is a prune that should not run.
set -u
HF=${HF_HOME:-/opt/hf}/hub
KEEP=${KEEP:-}
ARCHIVE=${ARCHIVE:-/mnt/models/gguf}
ARCHIVE_REQUIRED=${ARCHIVE_REQUIRED:-1}

archive_ok() {
  [ -d "$ARCHIVE" ] && [ -w "$ARCHIVE" ]
}

archive_gguf() {  # <dir> - copy any GGUF under this model dir into the store
  local d=$1
  find "$d" -name '*.gguf' -size +100M 2>/dev/null | while read -r f; do
    local base; base=$(basename "$f")
    if [ ! -s "$ARCHIVE/$base" ]; then
      cp "$f" "$ARCHIVE/.$base.part" 2>/dev/null && mv "$ARCHIVE/.$base.part" "$ARCHIVE/$base" \
        && echo "archived $base"
    fi
  done
}

if [ "$ARCHIVE_REQUIRED" = 1 ] && ! archive_ok; then
  echo "prune_models: ARCHIVE '$ARCHIVE' missing or unwritable - refusing to delete." >&2
  echo "prune_models: set ARCHIVE to the shared store, or ARCHIVE_REQUIRED=0 to override." >&2
  exit 0
fi

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
  archive_gguf "$d"
  rm -rf "$d" && { echo "pruned $name (${sz}MB)"; freed=$((freed + sz)); }
done
echo "freed ~${freed}MB"
df -h / | tail -1
