#!/bin/bash
# Leave the deployment with a reachable embedder, so `aimee status` reports the
# KB's real state instead of "degraded: no embedder configured".
#
# The probes start the stub and their own control leg kills it, which is correct
# for the probe and leaves the box without one afterwards. That is not a defect
# -- "no embedder configured is a failure, not a mode" is deliberate, and
# memory_core_scope_embed.c explains why: there used to be a lexical fallback
# that let an unconfigured kb answer every search with keyword matching while
# reporting itself healthy.
#
# The vectors this stub produces carry no semantic meaning, so nothing about
# RETRIEVAL QUALITY can be concluded from a system left in this state. What it
# gives is a deployment whose embed path is wired end to end.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
PORT="${1:-8799}"
DIM="${2:-384}"

pkill -f stub-embedder.py 2>/dev/null
sleep 1
setsid nohup python3 /root/stub-embedder.py "$PORT" "$DIM" >/root/stub-embedder.log 2>&1 </dev/null &
sleep 2
curl -s -m 8 "http://127.0.0.1:$PORT/health" | grep -q serving_id || {
  echo "FAIL: the stub did not come up; leaving the kb as it was" >&2; exit 1; }

export EMBEDDER_URL="http://127.0.0.1:$PORT"
bash /root/start-kb.sh >/dev/null 2>&1
bash /root/smm.sh >/dev/null 2>&1
bash /root/install-postgres-module.sh >/dev/null 2>&1
sleep 5

export AIMEE_HOME=/root AIMEE_API_ENDPOINT=unix:/root/aimee-http.sock
/usr/local/bin/aimee status 2>&1 | head -12
