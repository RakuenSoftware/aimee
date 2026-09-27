#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
: "${AIMEE_KB_STORE_REPLAY_URL:?Set AIMEE_KB_STORE_REPLAY_URL to a disposable database with the packaged KB_STORE schema}"
cd "$root/server-go"
go test ./modules/memory -run 'TestMemoryRuntimeRoleReplay|TestTypedContext|TestTypedFactCompatibilityGate|TestCSSConventionsHostBoundary|TestFactMutationRuntimeReplay|TestFactMaintenanceHostBoundary' -count=1
