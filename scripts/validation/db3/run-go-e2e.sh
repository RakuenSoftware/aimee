#!/bin/bash
# The Go half, plus the end-to-end proof that DB2's router reaches a real Go
# provider over the real authenticated C bus.
#
# scripts/test_db3_go_bus.sh is the one that matters here: everything else tests
# one half against a fake of the other. `make go-unit-tests` now drives it, so
# this script exists to run it against a container's toolchain rather than the
# developer's, and to keep its output where the record can quote it.
#
# Run AS ROOT inside the verification container. Do not run it at the same time
# as run-pg-tests.sh -- they share one build tree, and two makes in it at once
# corrupt each other's objects.
set -uo pipefail
export LC_ALL=C LANG=C
export GOCACHE="${GOCACHE:-/work/.gocache}"
export GOPATH="${GOPATH:-/work/.gopath}"
TREE="${TREE:-/work/aimee}"
LOG="${LOG:-/work/go-e2e.log}"

exec 3>&1
exec >"$LOG" 2>&1

failed=0
cd "$TREE/server-go" || { echo "no tree at $TREE/server-go" >&3; exit 1; }

echo "=== go build ==="
go build ./... || failed=1
echo "=== go vet ==="
go vet ./... || failed=1
echo "=== go test ./... ==="
go test ./... -count=1
test_rc=$?
[ $test_rc -eq 0 ] || failed=1

echo "=== vectordb, verbose ==="
go test ./modules/vectordb -count=1 -v
vector_rc=$?
[ $vector_rc -eq 0 ] || failed=1

echo "=== DB3 providers over the authenticated C bus ==="
cd "$TREE"
bash scripts/test_db3_go_bus.sh
bus_rc=$?
[ $bus_rc -eq 0 ] || failed=1

{
   echo "GO-ANY-FAILURE=$failed"
   echo "go-test=$test_rc vectordb=$vector_rc db3-go-bus=$bus_rc"
   echo "failing packages:"
   grep -E '^(FAIL|--- FAIL)' "$LOG" | sort -u | sed 's/^/  /'
   echo "db3 bus verdict:"
   grep -E 'TestDB3GoProvidersOperateOverAuthenticatedCBus|^(ok|FAIL)\s' "$LOG" \
      | tail -4 | sed 's/^/  /'
} >&3
