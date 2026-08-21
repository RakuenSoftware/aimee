#!/bin/bash
# Start aimee-kb inside the validation container. Run AS ROOT in the container.
set -u
export AIMEE_HOME=/root/.config/aimee
export AIMEE_DB2_URL="postgresql://aimee:aimee-e2e@127.0.0.1:5432/aimee_shared"
export AIMEE_KB_HTTP_BIND=1
export AIMEE_KB_API_BEARER_TOKEN="$(cat /root/kb-bearer.txt)"
ulimit -S -s 65536 || true
cd /root
nohup /usr/local/bin/aimee-kb --http-port=8741 >/root/kb.log 2>&1 &
echo $! > /root/kb.pid
sleep 8
echo "kb pid=$(cat /root/kb.pid)"
tail -8 /root/kb.log
