#!/bin/bash
# Pull results out of the bench CT into the working tree. Run from the repo root.
# Raw per-note predictions come back too, not just the scores, so a disputed
# number can be traced to the exact model output that produced it.
set -eu
CT=${CT:-140}
HOST=${HOST:-root@192.168.1.253}
DEST=bench/tier-a/results

mkdir -p "$DEST"
ssh "$HOST" "pct exec $CT -- tar czf - -C /opt/tierA/bench/tier-a results 2>/dev/null" \
  > /tmp/tierA-results.tgz
tar xzf /tmp/tierA-results.tgz -C bench/tier-a
echo "synced:"
find "$DEST" -type f | sort
