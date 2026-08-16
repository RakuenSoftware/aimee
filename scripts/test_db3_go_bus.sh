#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
make -C "$repo_root/src" --no-print-directory db3-go-host
harness="$repo_root/src/build/obj/tests/db3-go-host"
if [ ! -x "$harness" ]; then
   harness=$(find "$repo_root/src" -name db3-go-host -type f -perm -u+x 2>/dev/null | head -1)
fi
[ -x "$harness" ] || { echo "FAIL: DB3 Go host harness not built" >&2; exit 1; }

cd "$repo_root/server-go"
DB3_GO_HOST="$harness" CGO_ENABLED=0 \
   go test ./modules/db2 -run TestDB3GoProvidersOperateOverAuthenticatedCBus -v -timeout 60s
