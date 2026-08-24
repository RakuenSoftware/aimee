#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
make -C "$root/src" -j4 build/obj/tests/unit-test-kb-mining
"$root/src/build/obj/tests/unit-test-kb-mining"
rg -q '"learning.record_application"' "$root/src/kb/kb_service.c"
rg -q 'prior_procedure_id' "$root/src/kb/db2_adapters/kb_service_backend_agent.c"
rg -q 'CASE WHEN pg_try_advisory_lock.*THEN 1 ELSE 0 END' "$root/src/modules/db2/c/mining.c"
rg -q 'CASE WHEN enabled THEN 1 ELSE 0 END' "$root/src/modules/db2/c/mining.c"
