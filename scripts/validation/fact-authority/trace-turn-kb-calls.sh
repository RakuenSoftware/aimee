#!/bin/bash
# What does aimee-server ask the kb for during one chat turn?
#
# ingress_preinject_build gathers its envelope over the kb action API
# (memory.diagnose_scoped, memory.facts, code context). The kb logs every
# request, so the calls made during a single turn say exactly how far
# pre-injection got -- which beats inferring it from prompt size.
# Run AS ROOT in the container.
set -u
SOCK=/root/aimee-http.sock
Q="Where does the deployment runbook live and who owns it?"

before="$(wc -l < /root/kb.log)"
curl -s -m 300 --unix-socket "$SOCK" -H 'content-type: application/json' \
  -X POST --data "{\"model\":\"qwen\",\"max_tokens\":8,\"messages\":[{\"role\":\"user\",\"content\":\"$Q\"}]}" \
  http://localhost/v1/messages >/dev/null
sleep 1

echo "=== kb actions during the turn ==="
tail -n +"$((before + 1))" /root/kb.log | grep -oE 'path=/v1/actions/[a-z_.]+' | sort | uniq -c | sort -rn
echo
echo "=== ingress-context decisions logged by the server ==="
grep -a "ingress-context" /root/server.log | tail -3
echo "(none above means first_task_turn never fired -- the code-context packet only)"
