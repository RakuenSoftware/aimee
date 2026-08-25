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
# The DB3 proof: the postgres module reads a provisioned grant, routes a search
# over its own bus attachment to the shipped provider binary running as its own
# process, and gets scored candidates back.
#
# There used to be two more tests here, against a second DB3 router that lived
# in modules/db2. That router had no production caller -- postgres owns routing
# now -- so its tests were exercising a path no deployment takes, which is the
# shape this work kept finding. They went with it.
DB3_GO_HOST="$harness" \
   go test ./modules/postgres -run TestThePostgresModuleRoutes -v -timeout 180s
