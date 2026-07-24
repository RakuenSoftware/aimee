#!/usr/bin/env bash
# check_bus_perf_gate.sh — the event-bus performance budget gate (slice 12, D4 /
# shared invariant 15).
#
# It builds and runs the dispatch benchmark, and fails if the measured per-event
# dispatch overhead exceeds the committed ceiling in bench/bus_baseline.json. A
# red gate blocks the merge, so "within budget" always names a real, checkable
# number rather than a slogan.
#
# It also asserts the memory round-trip rows are still marked pending — they can
# only be measured against a pre-migration baseline in the memory-migration
# slice, and this gate must not let a fabricated number slip in.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

baseline="bench/bus_baseline.json"
[ -f "$baseline" ] || { echo "FAIL: $baseline not found" >&2; exit 1; }

# 1. The memory round-trip rows must be pending, not fabricated.
pending=$(python3 - "$baseline" <<'PY'
import json, sys
b = json.load(open(sys.argv[1]))
m = b["metrics"]
ok = all(m.get(k, {}).get("status") == "pending"
         for k in ("memory_roundtrip_p50_ns", "memory_roundtrip_p99_ns"))
print("yes" if ok else "no")
PY
)
if [ "$pending" != "yes" ]; then
   echo "FAIL: the memory round-trip rows must be marked pending until the" >&2
   echo "      memory-migration slice measures them against a real baseline." >&2
   exit 1
fi

ceiling=$(python3 -c "import json;print(json.load(open('$baseline'))['metrics']['dispatch_overhead_ns']['ceiling_ns'])")

# 2. Build and run the benchmark.
echo "building the dispatch benchmark..."
make -C src --no-print-directory bus-bench >/dev/null
bench=$(find "$repo_root/src" -name bus-bench -type f -perm -u+x 2>/dev/null | head -1)
[ -x "$bench" ] || { echo "FAIL: benchmark not built" >&2; exit 1; }

# A shorter run in the gate keeps it quick; the measurement is a median so it is
# stable. The ceiling's headroom absorbs CI variance.
measured=$("$bench" 500000 2>/dev/null | sed -n 's/^dispatch_overhead_ns=//p')
if [ -z "$measured" ]; then
   echo "FAIL: benchmark produced no dispatch_overhead_ns" >&2
   exit 1
fi

echo "dispatch overhead: ${measured} ns/event (ceiling ${ceiling} ns)"
if [ "$measured" -gt "$ceiling" ]; then
   echo "" >&2
   echo "FAIL: dispatch overhead ${measured} ns exceeds the committed ceiling" >&2
   echo "      ${ceiling} ns. Either a real regression landed, or the ceiling" >&2
   echo "      needs a reviewed change in ${baseline}." >&2
   exit 1
fi

# 3. The audit migration (delivery step 3, the first module on the bus): enforce
#    the committed enqueue-overhead ceiling AND the exactly-once durability
#    invariant. Both are measured by the real durability test, which emits rows
#    through the real producer/consumer and reads the real ledger back.
audit_ceiling=$(python3 -c "import json;print(json.load(open('$baseline'))['metrics']['audit_enqueue_overhead_ns']['ceiling_ns'])")
max_dropped=$(python3 -c "import json;print(json.load(open('$baseline'))['metrics']['audit_durability']['max_dropped'])")

echo "measuring audit-on-bus (durability + enqueue overhead)..."
audit_out=$(make -C src --no-print-directory unit-test-bus-audit-durability 2>&1 || true)
dropped=$(printf '%s\n' "$audit_out" | sed -n 's/.*dropped \([0-9][0-9]*\).*/\1/p' | tail -1)
p50=$(printf '%s\n' "$audit_out" | sed -n 's/.*enqueue.*p50=\([0-9][0-9]*\) ns.*/\1/p' | tail -1)

if ! printf '%s\n' "$audit_out" | grep -q "test_bus_audit_durability: OK"; then
   echo "" >&2
   echo "FAIL: the audit-on-bus durability test did not pass — the exactly-once" >&2
   echo "      invariant the migration depends on is not holding." >&2
   printf '%s\n' "$audit_out" | tail -5 >&2
   exit 1
fi
if [ -z "$dropped" ] || [ "$dropped" -gt "$max_dropped" ]; then
   echo "FAIL: audit rows dropped ('${dropped:-?}' > max ${max_dropped}) — durability violated." >&2
   exit 1
fi
if [ -z "$p50" ]; then
   echo "FAIL: could not read audit enqueue overhead from the durability test." >&2
   exit 1
fi
echo "audit enqueue overhead: ${p50} ns (ceiling ${audit_ceiling} ns); dropped ${dropped}"
if [ "$p50" -gt "$audit_ceiling" ]; then
   echo "" >&2
   echo "FAIL: audit enqueue overhead ${p50} ns exceeds the committed ceiling" >&2
   echo "      ${audit_ceiling} ns. Either a real regression landed (e.g. the async" >&2
   echo "      publish became synchronous), or the ceiling needs a reviewed change" >&2
   echo "      in ${baseline}." >&2
   exit 1
fi

# 4. The vault credential-access audit trail (delivery step 3, another module on
#    the bus): a functional check that vault access flows through the REAL server
#    bridge -> obs_bus -> ledger with the fields mapped and NO secret leaked.
#    Run here (like the durability test) because it needs the same special bus
#    link set the standard unit-tests build does not assemble.
echo "checking vault-access-on-bus audit trail..."
vault_out=$(make -C src --no-print-directory unit-test-bus-vault-audit 2>&1 || true)
if ! printf '%s\n' "$vault_out" | grep -q "test_bus_vault_audit: OK"; then
   echo "" >&2
   echo "FAIL: the vault-access-on-bus audit test did not pass — credential access" >&2
   echo "      is not being recorded correctly through the bridge, or a secret leaked." >&2
   printf '%s\n' "$vault_out" | tail -8 >&2
   exit 1
fi
echo "vault-access audit: ok (access -> bridge -> bus -> ledger; no secret leak)"

# 5. The sandbox degraded-isolation audit trail: a guarded exec that ran
#    unsandboxed (or was refused) because the sandbox was unavailable flows
#    through the REAL server bridge -> obs_bus -> ledger, fields mapped, no
#    secret leaked. Same special-link reason as the vault test.
echo "checking sandbox degraded-isolation audit trail..."
sbx_out=$(make -C src --no-print-directory unit-test-bus-sandbox-audit 2>&1 || true)
if ! printf '%s\n' "$sbx_out" | grep -q "test_bus_sandbox_audit: OK"; then
   echo "" >&2
   echo "FAIL: the sandbox-on-bus audit test did not pass — a degraded-isolation" >&2
   echo "      exec is not being recorded correctly through the bridge, or a secret leaked." >&2
   printf '%s\n' "$sbx_out" | tail -8 >&2
   exit 1
fi
echo "sandbox audit: ok (degraded isolation -> bridge -> bus -> ledger; no secret leak)"

# 6. The server-side memory-mutation audit trail: a memory change the server
#    requested via kb_client flows through the REAL bridge -> obs_bus -> ledger,
#    identity recorded, memory content never present. Same special-link reason.
echo "checking server-side memory-mutation audit trail..."
mem_out=$(make -C src --no-print-directory unit-test-bus-memory-audit 2>&1 || true)
if ! printf '%s\n' "$mem_out" | grep -q "test_bus_memory_audit: OK"; then
   echo "" >&2
   echo "FAIL: the memory-on-bus audit test did not pass — a memory mutation is not" >&2
   echo "      being recorded correctly through the bridge, or content leaked." >&2
   printf '%s\n' "$mem_out" | tail -8 >&2
   exit 1
fi
echo "memory audit: ok (mutation -> bridge -> bus -> ledger; no content)"

# 7. The KB store-side memory audit: memory_core_crud fires the hook on every
#    mutation (insert / merge / update / reject / delete) and, end-to-end, the REAL
#    aimee-kb bridge lands a fingerprinted, content-free row in the KB ledger.
echo "checking KB store-side memory-audit hook (end to end)..."
kbmem_out=$(make -C src --no-print-directory unit-test-memory-audit-hook 2>&1 || true)
if ! printf '%s\n' "$kbmem_out" | grep -q "test_memory_audit_hook: OK"; then
   echo "" >&2
   echo "FAIL: the KB store-side memory-audit test did not pass — memory_core_crud is" >&2
   echo "      not firing the hook correctly, or the KB bridge/ledger path regressed." >&2
   printf '%s\n' "$kbmem_out" | tail -8 >&2
   exit 1
fi
echo "KB memory audit: ok (memory_core_crud -> hook -> KB bridge -> bus -> ledger)"

echo "check_bus_perf_gate: ok — dispatch + audit within budget; audit durability holds; vault-access + sandbox + memory (server + kb) audit hold; memory rows pending"
