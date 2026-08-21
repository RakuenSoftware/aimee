#!/bin/bash
# Is ir_stage_memory running at all?
#
# The stage does two separable things. On a SESSION START (no assistant turn in
# the history yet) it prepends a fixed guidance block. On any turn it prepends
# the retrieval envelope, when ingress_preinject_build returns one.
#
# So two turns isolate it, and input_tokens is the measurement:
#   A. one user message            -> session start: guidance (+ envelope if any)
#   B. user/assistant/user history -> NOT a session start: envelope only
# If A is materially larger than B, the stage is live and the guidance is
# landing. If A == B, the stage is not injecting anything and the problem is
# upstream of ingress_preinject_build.
# Run AS ROOT in the container.
set -u
SOCK=/root/aimee-http.sock
Q="Where does the deployment runbook live and who owns it?"

post() { curl -s -m 300 --unix-socket "$SOCK" -H 'content-type: application/json' \
              -X POST --data "$1" http://localhost/v1/messages; }
intok() { sed 's/.*"input_tokens":\([0-9]*\).*/\1/'; }

A="$(post "{\"model\":\"qwen\",\"max_tokens\":8,\"messages\":[{\"role\":\"user\",\"content\":\"$Q\"}]}" | intok)"
B="$(post "{\"model\":\"qwen\",\"max_tokens\":8,\"messages\":[{\"role\":\"user\",\"content\":\"$Q\"},{\"role\":\"assistant\",\"content\":\"ok\"},{\"role\":\"user\",\"content\":\"$Q\"}]}" | intok)"

echo "session-start turn      input_tokens=$A"
echo "mid-session turn        input_tokens=$B"
echo
if [ "${A:-0}" -gt "$(( ${B:-0} + 50 ))" ] 2>/dev/null; then
  echo "  ir_stage_memory IS live: the session-start guidance block is landing."
  echo "  (a mid-session turn carries MORE history yet a SMALLER prompt)"
else
  echo "  ir_stage_memory is NOT injecting: no guidance on session start."
  echo "  The problem is upstream of ingress_preinject_build -- the stage itself"
  echo "  is being skipped or never reached on this route."
fi
