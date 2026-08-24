#!/bin/bash
# Prove the memory module's RETRIEVE stage (the §7 PII recall gate) is live in
# the kb, and that the kb is no longer falling back to its in-process cue list.
#
# The gate decides whether a PII-classed fact may be injected into a turn.
# fact_recall asks memory_pii_rel_sensitivity_batch() for each candidate's tier
# and memory_pii_turn_requests_sensitive() whether the turn asked for sensitive
# data; withheld candidates never reach the envelope.
#
# With the providers registered, a failed batch makes fact_recall abandon the
# candidates rather than inject them -- so stopping the kb-side module must make
# PII-classed facts disappear from the recall block, and restarting must bring
# them back. That difference is only possible if the module is answering.
# Run AS ROOT in the container.
set -u
B="$(cat /root/kb-bearer.txt)"

facts_for() {  # $1 = query -> the recalled facts block, or (none)
  curl -s -m 30 -H "Authorization: Bearer ${B}" -H 'content-type: application/json' \
    -X POST --data "{\"query\":\"$1\"}" \
    http://127.0.0.1:8741/v1/actions/memory.facts \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); f=(d.get("facts") or "").strip(); print(f if f else "(none)")' 2>/dev/null
}

kbmod() { pgrep -cf 'aimee-module-memory /root/.config/aimee/kb-module-bus.sock'; }

# `email` is SENS_PII in the seed ontology, so it is exactly what the gate rules on.
Q="what is my email address"

echo "=== kb-side module RUNNING (instances: $(kbmod)) ==="
echo "  facts: $(facts_for "$Q" | head -c 200)"

echo
echo "=== kb-side module STOPPED ==="
pkill -f "aimee-module-memory /root/.config/aimee/kb-module-bus.sock" 2>/dev/null
sleep 2
echo "  instances: $(kbmod)"
echo "  facts: $(facts_for "$Q" | head -c 200)"

echo
echo "=== restarted ==="
bash /root/start-memory-module.sh >/dev/null 2>&1
sleep 2
echo "  instances: $(kbmod)"
echo "  facts: $(facts_for "$Q" | head -c 200)"

echo
echo "A PII-classed fact present / absent / present tracks the module answering"
echo "RETRIEVE. Identical output in all three means the kb is still deciding"
echo "locally and the provider is not wired."
