#!/bin/bash
# Stage the Tier-B harness into the bench CT. Excludes results/ for the same
# reason the Tier-A stager does: pushing them back overwrote in-progress runs.
set -eu
CT=${CT:-140}
HOST=${HOST:-root@192.168.1.253}
TGZ=$(mktemp /tmp/tierB-XXXX.tgz)
tar czf "$TGZ" --exclude='bench/tier-b/results' --exclude='__pycache__' \
    bench/tier-b src/modules/kb-synthesis/kb_curator_synthesize.c
scp -q "$TGZ" "$HOST:/tmp/tierB-stage.tgz"
ssh "$HOST" "pct push $CT /tmp/tierB-stage.tgz /opt/tierA/stageB.tgz >/dev/null && \
             pct exec $CT -- tar xzf /opt/tierA/stageB.tgz -C /opt/tierA"
rm -f "$TGZ"
echo "tier-b staged"
