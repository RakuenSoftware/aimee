#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
make -C "$root/src" -j4 build/obj/tests/unit-test-kb-mining
"$root/scripts/test_temporal_assertion_retrieval.sh"
"$root/src/build/obj/tests/unit-test-kb-mining"
rg -Fq 'authorization="none"' "$root/server-go/modules/memory/typed_context.go"
rg -q 'LEFT JOIN memories' "$root/server-go/modules/memory/assertion_search.go"
