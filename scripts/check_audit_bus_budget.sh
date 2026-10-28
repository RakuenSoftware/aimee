#!/usr/bin/env bash
# check_audit_bus_budget.sh -- the audit row's budget on the ouroboros ring.
#
# Runs test_bus_audit_durability, which emits rows through the real producer
# and consumer and reads the real ledger back, and fails if a row was dropped
# or the caller-side enqueue cost is over the ceiling in
# bench/audit_bus_budget.json. The ring's own dispatch budget is checked in the
# ring's repository, not here.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

budget="bench/audit_bus_budget.json"
[ -f "$budget" ] || { echo "FAIL: $budget not found" >&2; exit 1; }
ceiling=$(python3 -c "import json;print(json.load(open('$budget'))['metrics']['audit_enqueue_overhead_ns']['ceiling_ns'])")
max_dropped=$(python3 -c "import json;print(json.load(open('$budget'))['metrics']['audit_durability']['max_dropped'])")

echo "measuring the audit row on the ring (durability + enqueue overhead)..."
audit_out=$(make -C src --no-print-directory unit-test-bus-audit-durability 2>&1 || true)
dropped=$(printf '%s\n' "$audit_out" | sed -n 's/.*dropped \([0-9][0-9]*\).*/\1/p' | tail -1)
p50=$(printf '%s\n' "$audit_out" | sed -n 's/.*enqueue.*p50=\([0-9][0-9]*\) ns.*/\1/p' | tail -1)

if ! printf '%s\n' "$audit_out" | grep -q "test_bus_audit_durability: OK"; then
   echo "FAIL: test_bus_audit_durability did not pass; the exactly-once drain is not holding." >&2
   printf '%s\n' "$audit_out" | tail -5 >&2
   exit 1
fi
if [ -z "$dropped" ] || [ "$dropped" -gt "$max_dropped" ]; then
   echo "FAIL: audit rows dropped ('${dropped:-?}' > max ${max_dropped})." >&2
   exit 1
fi
if [ -z "$p50" ]; then
   echo "FAIL: could not read the enqueue overhead from the durability test." >&2
   exit 1
fi
echo "audit enqueue overhead: ${p50} ns (ceiling ${ceiling} ns); dropped ${dropped}"
if [ "$p50" -gt "$ceiling" ]; then
   echo "FAIL: ${p50} ns is over the ${ceiling} ns ceiling. Either a regression landed" >&2
   echo "      (for example the publish became synchronous) or the ceiling needs a" >&2
   echo "      reviewed change in ${budget}." >&2
   exit 1
fi
echo "check_audit_bus_budget: ok"
