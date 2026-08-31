#!/usr/bin/env bash
# check_bus_go_no_cgo.sh — the Go reference client must be pure Go (suite
# invariant 12: no cgo boundary). A Go client that linked the C implementation
# would test the C code twice and prove nothing about the wire spec, defeating
# the conformance suite.
#
# Three independent assertions: the package has no cgo files, no `import "C"`,
# and it builds with CGO_ENABLED=0.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
packages=("./bus/..." "./modules/..." "./cmd/aimee-module")
gomod="$repo_root/server-go"

if [ ! -d "$gomod/bus" ]; then
   echo "check_bus_go_no_cgo: server-go/bus not present yet — skipping"
   exit 0
fi

cd "$gomod"

cgo_files=$(go list -f '{{.CgoFiles}}' ./bus 2>/dev/null || echo "[]")
if [ "$cgo_files" != "[]" ]; then
   echo "FAIL: the Go bus package has cgo files: $cgo_files" >&2
   exit 1
fi

if grep -rn 'import[[:space:]]*"C"' bus/ modules/ cmd/aimee-module/ >/dev/null 2>&1; then
   echo "FAIL: the Go bus/module runtime imports \"C\"" >&2
   grep -rn 'import[[:space:]]*"C"' bus/ modules/ cmd/aimee-module/ >&2
   exit 1
fi

if ! CGO_ENABLED=0 go build "${packages[@]}" >/dev/null 2>&1; then
   echo "FAIL: the Go bus/module runtime does not build with CGO_ENABLED=0" >&2
   exit 1
fi

echo "check_bus_go_no_cgo: ok — bus and migrated modules are pure Go"
