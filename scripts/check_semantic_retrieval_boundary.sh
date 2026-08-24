#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
rg -q "edge_class <> 'semantic'|edge_class<>'semantic'" src/db2 src/modules/memory
rg -q "db2_semantic_assertion_search" src/db2/typed_facts.c src/db2/kb_service_backend_context.c
rg -q "SEMANTIC_ASSERTION_VECTOR_POINT_OFFSET" src/db2/typed_facts.h
rg -q 'vector_scores\[i\] < 0\.20' src/db2/kb_service_backend_context.c
rg -q "fact_mutation" src/db2/typed_facts.c
if sed -n '/int db2_semantic_assertion_search/,/int db2_semantic_assertion_index_list/p' \
    src/db2/typed_facts.c | rg -n "FROM typed_facts|JOIN typed_facts"; then
  echo "semantic recall reads the compatibility table" >&2
  exit 1
fi
rg -q "include_historical" src/db2/typed_facts.c src/kb/kb_service_memory.c
echo "semantic retrieval boundary: pass"
