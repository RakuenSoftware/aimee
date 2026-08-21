#!/bin/bash
# Drive a REAL chat turn so the memory module's two server-only stages run.
#
# aimee-server calls RERANK (5893) for the ingress confidence tier and RETRIEVE
# (5892) for the PII recall gate. Neither is reachable from the kb, and reaching
# them through a live turn needs every one of these, in order:
#
#   1. a chat provider  (install-chat-provider.sh)
#   2. the request on /v1/messages -- pre-injection hooks the Anthropic-native
#      ingress, not /v1/chat/completions
#   3. ingress_preinject_anthropic_enabled ON (opt-in, no env override)
#   4. an ACTIVE REPOSITORY -- ingress_preinject_build() returns NULL without a
#      project identity, so agent ingress cannot broaden to global recall
#      (make-scope-repo.sh)
#   5. the memory SCOPED TO THAT PROJECT. ingress_preinject_build calls
#      kb_client_memory_scope_context_set(workspace, project, 0) before
#      recalling, so an untagged memory is invisible to the turn however well it
#      scores in an unscoped query. This is the step that was missing.
#
# The control is the module itself: the same turn with the server-side module
# stopped, where ingress_preinject_confidence() returns -1.
# Run AS ROOT in the container.
set -u
SOCK=/root/aimee-http.sock
B="$(cat /root/kb-bearer.txt)"
Q="Where does the deployment runbook live and who owns it?"

kb() { curl -s -m 30 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
            -X POST --data "$2" "http://127.0.0.1:8741/v1/actions/$1"; }

ask() {
  curl -s -m 300 --unix-socket "$SOCK" -H 'content-type: application/json' \
    -X POST --data "{\"model\":\"qwen\",\"max_tokens\":96,\"messages\":[{\"role\":\"user\",\"content\":\"$Q\"}]}" \
    http://localhost/v1/messages
}

PROJ="$(cat /root/proj/.git/aimee-project-id 2>/dev/null)"
echo "active project identity: ${PROJ:-<none>}"
[ -n "$PROJ" ] || { echo "no project identity; run make-scope-repo.sh and restart the server" >&2; exit 1; }

echo
echo "=== seed a memory AND scope it to the active project ==="
ID="$(curl -s -m 20 --unix-socket "$SOCK" -H 'content-type: application/json' \
  -X POST --data '{"key":"e2e-chat-fact","content":"The deployment runbook lives in docs/DEPLOY.md and Priya owns it.","tier":"L2","kind":"fact"}' \
  http://localhost/v1/memory/store | sed 's/.*"id":\([0-9]*\).*/\1/')"
echo "  memory id=${ID}"
kb memory.tag_workspace "{\"memory_id\":${ID},\"workspace\":\"${PROJ}\"}" | head -c 120; echo
kb memory.tag_scope "{\"memory_id\":${ID},\"scope_type\":\"project\",\"scope_value\":\"${PROJ}\"}" | head -c 120; echo

mod() { pgrep -cf 'aimee-module-memory /root/server-module-bus.sock'; }
intok() { sed 's/.*"input_tokens":\([0-9]*\).*/\1/'; }

# input_tokens is the measurement. The envelope is prepended to the prompt, so a
# turn that got one is materially larger than the same turn that did not -- and
# ingress_preinject_confidence() fails without the module, which drops the tier
# and with it the envelope's confidence line.
echo
echo "=== turn WITH the server-side memory module (instances: $(mod)) ==="
WITH="$(ask)"
echo "  input_tokens: $(printf '%s' "$WITH" | intok)"
printf '%s' "$WITH" | head -c 300; echo

echo
echo "=== same turn, module STOPPED (control) ==="
pkill -f "aimee-module-memory /root/server-module-bus.sock" 2>/dev/null
sleep 2
echo "  module instances now: $(mod)"
WITHOUT="$(ask)"
echo "  input_tokens: $(printf '%s' "$WITHOUT" | intok)"
printf '%s' "$WITHOUT" | head -c 300; echo

echo
echo "=== verdict ==="
a="$(printf '%s' "$WITH" | intok)"; b="$(printf '%s' "$WITHOUT" | intok)"
if [ "${a:-0}" -gt "${b:-0}" ] 2>/dev/null; then
  echo "  prompt grew by $(( a - b )) tokens with the module running:"
  echo "  the envelope was built, so RERANK answered the confidence request."
else
  echo "  no measurable difference (with=$a without=$b) -- the envelope is not"
  echo "  reaching the prompt, so this does not demonstrate RERANK."
fi

echo
echo "=== restore ==="
bash /root/install-memory-module-server.sh 2>&1 | grep -E 'state:'
