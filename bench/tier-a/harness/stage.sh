#!/bin/bash
# Stage the harness into the bench CT. Run from the repo root.
#
# EXCLUDES results/. An earlier version of this tarball carried bench/tier-a
# wholesale, so every harness re-stage overwrote the CT's results directory with
# whatever stale copy happened to be in the working tree -- including, once, an
# in-progress prediction file, which silently truncated an E4B run to 27 of 70
# notes and still exited 0. Results flow CT -> repo (sync_results.sh) and never
# back.
set -eu
CT=${CT:-140}
HOST=${HOST:-root@192.168.1.253}
TGZ=$(mktemp /tmp/tierA-stage-XXXX.tgz)

tar czf "$TGZ" --exclude='bench/tier-a/results' --exclude='__pycache__' \
    bench/tier-a src/rel_types.c src/kb/kb_memory_facts.c
scp -q "$TGZ" "$HOST:/tmp/tierA-stage.tgz"
ssh "$HOST" "pct push $CT /tmp/tierA-stage.tgz /opt/tierA/stage.tgz >/dev/null && \
             pct exec $CT -- tar xzf /opt/tierA/stage.tgz -C /opt/tierA"
rm -f "$TGZ"
echo "staged (results/ untouched)"
