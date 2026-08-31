#!/bin/bash
# Start aimee-server with remote_writes=full, so an authorized TCP bearer holds
# CAPS_ALL and genuinely reaches the memory/facts write paths.
#
# Without this the default (off) refuses TCP mutations at the write-tier grant
# gate, several layers ABOVE the authority decision — so a test run against the
# default proves only that the outer gate works, not that the authority mapping
# does. This is the configuration in which "a bearer clears CAP_MEMORY_ADMIN"
# is actually true, which is the case the fix exists for.
# Run AS ROOT in the container.
set -u
pkill -f aimee-server 2>/dev/null
sleep 2
export AIMEE_HOME=/root
export AIMEE_KB_API_URL="http://127.0.0.1:8741"
export AIMEE_KB_API_BEARER_TOKEN="$(cat /root/kb-bearer.txt)"
export AIMEE_DB1_URL="sqlite:///root/aimee.db"
export AIMEE_SERVER_HTTP_BIND=1
export AIMEE_DEPLOY_ENABLED=1
export AIMEE_API_REMOTE_WRITES=full
export AIMEE_API_BEARER_TOKEN="$(cat /root/server-bearer.txt)"
ulimit -S -s 65536 || true
cd /root
nohup /usr/local/bin/aimee-server --socket=/root/aimee-server.sock >/root/server.log 2>&1 &
echo $! > /root/server.pid
sleep 10
echo "server pid=$(cat /root/server.pid) (remote_writes=full)"
grep -iE "remote_writes|HTTP /v1 on" /root/server.log | tail -3
