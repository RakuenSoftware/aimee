#!/bin/bash
# Drive a REAL chat turn so the memory module's two server-only stages run.
#
# aimee-server calls RERANK (5893) for the ingress confidence tier and RETRIEVE
# (5892) for the PII recall gate. Neither is reachable from the kb, and getting
# them to fire took three corrections:
#
#   - the turn must ask about a memory that EXISTS, because pre-injection only
#     builds an envelope when there is something to inject, and the confidence
#     tier is requested only while building one;
#   - it must go to /v1/messages, not /v1/chat/completions -- pre-injection hooks
#     the Anthropic-native ingress (anthropic_http.c), not the native chat route;
#   - ingress_preinject_anthropic_enabled must be ON. It is opt-in (P5 §2.3) and
#     has no env override, so start-server.sh writes it into the config.
#
# The control is the module itself: the same turn with the server-side module
# stopped. ingress_preinject_confidence() returns -1 when its provider fails, so
# the envelope loses its tier while the answer still comes back.
# Run AS ROOT in the container.
set -u
SOCK=/root/aimee-http.sock

ask() {  # $1 = user text
  curl -s -m 300 --unix-socket "$SOCK" -H 'content-type: application/json' \
    -X POST --data "{\"model\":\"qwen\",\"max_tokens\":96,\"messages\":[{\"role\":\"user\",\"content\":\"$1\"}]}" \
    http://localhost/v1/messages
}

Q="Where does the deployment runbook live and who owns it?"

echo "=== seed a memory the turn can recall ==="
curl -s -m 20 --unix-socket "$SOCK" -H 'content-type: application/json' \
  -X POST --data '{"key":"e2e-chat-fact","content":"The deployment runbook lives in docs/DEPLOY.md and Priya owns it.","tier":"L2","kind":"fact"}' \
  http://localhost/v1/memory/store | head -c 120
echo

mod() { pgrep -cf 'aimee-module-memory /root/server-module-bus.sock'; }

echo
echo "=== turn WITH the server-side memory module (module instances: $(mod)) ==="
ask "$Q" | head -c 400
echo

echo
echo "=== same turn, module STOPPED (control) ==="
pkill -f "aimee-module-memory /root/server-module-bus.sock" 2>/dev/null
sleep 2
echo "  module instances now: $(mod)"
ask "$Q" | head -c 400
echo

echo
echo "=== restore ==="
bash /root/install-memory-module-server.sh 2>&1 | grep -E 'state:'
