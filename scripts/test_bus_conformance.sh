#!/usr/bin/env bash
# test_bus_conformance.sh — the event-bus conformance suite (slice 10).
#
# Runs the three legs that keep the wire spec honest:
#   1. Vectors: the C codec and the Go codec both produce and accept the exact
#      committed bytes (each in its own unit test).
#   2. Interop: the single in-source C host, exposed on a Unix socket, driven by
#      a real Go client across a process boundary — both directions, plus
#      capability_absent.
#   3. Single-host: the mechanical D8 check (one host, Go is client-only).
#
# Everything is bounded by timeouts so the suite cannot hang.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

echo "== 1. wire vectors: C codec =="
make -C src --no-print-directory unit-test-bus-wire

echo "== 1. wire vectors: Go codec =="
( cd server-go && CGO_ENABLED=0 go test ./bus/... -run 'TestWireVectors|TestValidationAxes|TestRing|TestControl' )

echo "== 1. migrated Go module handlers =="
( cd server-go && CGO_ENABLED=0 go test ./modules/... ./cmd/aimee-module )
python3 -I scripts/check_go_module_runtime_bundle.py

echo "== 2. cross-language interop: C host <-> Go client =="
make -C src --no-print-directory bus-conformance-host
harness="$repo_root/src/build/obj/tests/bus-conformance-host"
if [ ! -x "$harness" ]; then
   # honour a non-default TESTPREFIX/OBJDIR if the caller set one
   harness=$(find "$repo_root/src" -name bus-conformance-host -type f -perm -u+x 2>/dev/null | head -1)
fi
[ -x "$harness" ] || { echo "FAIL: conformance host harness not built" >&2; exit 1; }
( cd server-go && BUS_CONFORMANCE_HOST="$harness" \
   CGO_ENABLED=0 go test ./bus/... -run TestCrossLanguageConformance -v -timeout 60s )

echo "== 2. module runtime interop: C host/core caller <-> Go module process =="
make -C src --no-print-directory "build/obj/tests/unit-test-module-runtime"
module_harness="$repo_root/src/build/obj/tests/unit-test-module-runtime"
go_module="$repo_root/src/build/obj/tests/go-module-runtime-helper"
( cd server-go && CGO_ENABLED=0 go build -o "$go_module" ./bus/testdata/module_helper )
timeout 60s "$module_harness" "$go_module"

echo "== 2. migrated module interop: C core caller <-> shipped Go processes =="
go_multicall="$repo_root/src/build/obj/tests/aimee-module-go"
( cd server-go && CGO_ENABLED=0 go build -trimpath -o "$go_multicall" ./cmd/aimee-module )
# Derive the list from the contract rather than restating it. A hardcoded list
# drifts silently: a module hosted by another program is no longer a spawned
# multicall binary, and a newly migrated one would never be exercised here.
module_ids=$(python3 -I -c '
import json, sys
contract = json.load(open("src/modules/process-contracts.json"))
print(" ".join(
    component["id"]
    for component in contract["components"]
    if component.get("runtime") == "go" and not component.get("hosted_by")
))')
for module_id in $module_ids; do
   executable="$repo_root/src/build/obj/tests/aimee-module-$module_id"
   install -m 0755 "$go_multicall" "$executable"
   timeout 60s "$module_harness" "$executable" "$module_id"
done

echo "== 3. single-host (D8) =="
"$repo_root/scripts/check_bus_single_host.sh"

echo "test_bus_conformance: ok"
