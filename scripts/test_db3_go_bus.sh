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
# Both DB3 bus proofs. The first attaches providers as goroutines inside the
# test; the second starts the SHIPPED provider binary as its own process under a
# grant naming its executable, applies points through the wire, and searches
# them back. Only the second exercises a deployment, which is how the provider
# came to have no runnable process while every test of it passed.
#
# CGO_ENABLED is not forced here: the process test builds the multicall itself.
DB3_GO_HOST="$harness" \
   go test ./modules/db2 \
   -run 'TestDB3GoProvidersOperateOverAuthenticatedCBus|TestTheShippedProviderBinaryServesOverARealBus' \
   -v -timeout 180s || exit 1

# The postgres module's own routing, over the same bus: a provider answers, and
# PostgreSQL is not touched for a routed search.
DB3_GO_HOST="$harness" \
   go test ./modules/postgres -run TestThePostgresModuleRoutes -v -timeout 180s
