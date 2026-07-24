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

echo "check_bus_perf_gate: ok — dispatch overhead within budget; memory rows pending"
