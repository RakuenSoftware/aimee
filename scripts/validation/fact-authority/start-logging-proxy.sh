#!/bin/bash
# Start the upstream-capture proxy and repoint the chat provider through it.
# Run AS ROOT in the container.
set -u
pkill -f logging-proxy 2>/dev/null
rm -f /root/proxy-capture.jsonl
setsid nohup python3 /root/logging-proxy.py "${UPSTREAM:-http://192.168.1.100:8762}" \
  >/root/proxy.log 2>&1 < /dev/null &
sleep 2
curl -s -m 8 -o /dev/null -w "proxy /v1/models: %{http_code}\n" http://127.0.0.1:8798/v1/models

# Point the agent at the proxy instead of the model directly.
LLM_ENDPOINT="http://127.0.0.1:8798/v1" bash /root/install-chat-provider.sh >/dev/null 2>&1
grep -o '"endpoint": *"[^"]*"' /root/agents.json
bash /root/start-server.sh >/dev/null 2>&1
bash /root/install-memory-module-server.sh 2>&1 | grep -E 'state:'
