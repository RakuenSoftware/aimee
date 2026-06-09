# Proposal: graph-derived code-health audit (`aimee code audit`)

- **State:** done
- **Status refreshed:** 2026-06-09
- **Split from:** `docs/proposals/pending/code-health-audit.md`

## Shipped

The core proposal is implemented.

- Code-unit indexing stores `body_hash` for clone grouping.
- The DB2 schema and pgvec transport include `body_hash` on code-unit vectors.
- `code.audit` is exposed as a kb-side graph query and assembled as JSON.
- `aimee code audit` has a graph-backed mode that reads the server-side `dead_exports`, `cycles`, `clones`, and `near_clones` payload.
- The CLI still has local file-health checks, but graph-derived checks now use the kb-side audit surface.

## Verification Notes

Verified in-tree evidence: `src/kb/kb_curator_index_code_unit.c`, `src/db2/schema.sql`, `src/db2/schema_sqlite.sql`, `src/db2/pgvec_transport.c`, `src/kb/kb_service_graph.c`, `src/kb/kb_service.c`, `src/db2/kb_service_backend.h`, and `src/cli_code_audit.c`.
