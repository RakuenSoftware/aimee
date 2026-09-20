#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
# The shared Go owner now implements retrieval and typed-context policy.
rg -q "edge_class <> 'semantic'|edge_class<>'semantic'" src/modules/db2/c src/modules/memory
rg -q 'assertion-search' src/kb/db2_adapters/kb_service_backend_context.c
rg -q 'const assertionPointOffset int64 = 2000000000000' server-go/modules/memory/assertion_search.go
rg -q 'h.raw < 0.20' server-go/modules/memory/assertion_search.go
rg -q 's.assertFact' server-go/modules/memory/css_conventions.go
if rg -n 'FROM typed_facts|JOIN typed_facts' server-go/modules/memory; then
  echo "semantic recall reads the compatibility table" >&2
  exit 1
fi
rg -q 'include_historical' server-go/modules/memory/assertion_search.go src/kb/kb_service_memory.c
rg -Fq 'flag("enabled", true)' server-go/modules/memory/typed_context.go
rg -Fq 'flag("enable_semantic_assertions", true)' server-go/modules/memory/typed_context.go
rg -Fq 'flag("enable_observations", true)' server-go/modules/memory/typed_context.go
rg -Fq 'flag("enable_approved_procedures", true)' server-go/modules/memory/typed_context.go
rg -Fq 'kb_client_memory_assemble_typed_context(query)' src/server/ingress_preinject.c
if rg -Fq 'temporal_on = 0;' src/server/ingress_preinject.c; then
  echo "default prompt mode suppresses temporal learning" >&2
  exit 1
fi
echo "semantic retrieval boundary: pass"
