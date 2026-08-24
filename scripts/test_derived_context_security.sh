#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
make -C "$root/src" -j4 build/obj/tests/unit-test-typed-facts build/obj/tests/unit-test-kb-mining
"$root/src/build/obj/tests/unit-test-typed-facts"
"$root/src/build/obj/tests/unit-test-kb-mining"
rg -q 'authorization=\\"none\\"' "$root/src/db2/kb_service_backend_context.c"
rg -q 'deny-dominant scope inheritance' "$root/src/db2/kb_service_backend_context.c"
