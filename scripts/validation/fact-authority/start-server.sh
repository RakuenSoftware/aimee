#!/bin/bash
# Start aimee-server against the local kb. Run AS ROOT in the container.
set -u
# Restart, not "start a second one". Without this a re-run silently left the
# ORIGINAL process alive holding the old module policy, and a freshly installed
# grant looked like it was being ignored (module attach denied) when in fact
# nothing had reloaded it.
pkill -f aimee-server 2>/dev/null
sleep 2
export AIMEE_HOME=/root
export AIMEE_KB_API_URL="http://127.0.0.1:8741"
export AIMEE_KB_API_BEARER_TOKEN="$(cat /root/kb-bearer.txt)"
export AIMEE_DB1_URL="sqlite:///root/aimee.db"
export AIMEE_SERVER_HTTP_BIND=1
export AIMEE_DEPLOY_ENABLED=1
export AIMEE_API_BEARER_TOKEN="$(cat /root/server-bearer.txt)"
ulimit -S -s 65536 || true
cd /root
nohup /usr/local/bin/aimee-server --socket=/root/aimee-server.sock >/root/server.log 2>&1 &
echo $! > /root/server.pid
sleep 10
echo "server pid=$(cat /root/server.pid)"
ls -la /root/aimee-server.sock 2>&1
tail -6 /root/server.log
