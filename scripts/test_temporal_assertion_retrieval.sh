#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
: "${AIMEE_DB2_REPLAY_URL:?Set AIMEE_DB2_REPLAY_URL to a disposable database with the packaged DB2 schema}"
cd "$root/server-go"
go test ./modules/memory -run 'TestMemoryRuntimeRoleReplay|TestTypedContext|TestTypedFactCompatibilityGate|TestCSSConventionsHostBoundary|TestFactMutationRuntimeReplay' -count=1
