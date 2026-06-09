# Proposal: graph-derived code-health audit (`aimee code audit`)

- **State:** done
- **Status refreshed:** 2026-06-09
- **Split lineage:** residual follow-up folded into this done record.

## Shipped

The core proposal is implemented.

- Code-unit indexing stores `body_hash` for clone grouping.
- The DB2 schema and pgvec transport include `body_hash` on code-unit vectors.
- `code.audit` is exposed as a kb-side graph query and assembled as JSON.
- `aimee code audit` has a graph-backed mode that reads the server-side `dead_exports`, `cycles`, `clones`, and `near_clones` payload.
- The CLI still has local file-health checks, but graph-derived checks now use the kb-side audit surface.

## Residual Follow-up

The residual follow-up is complete.

- `aimee code audit --fix` remains a non-mutating flag until safe, reviewable mechanical fixes exist. The CLI reports that no safe automatic fixes are available rather than editing files.
- Acceptance coverage now exercises an indexed-project-shaped KB payload: graph dead exports, bounded import cycles, exact clone grouping, clone minimum-line thresholds, legacy clone metadata handling, clone result limits, and empty pgvector near-clone results from the test shim.
- User-facing docs now describe `aimee code audit --graph`, the `aimee-server`/`aimee-kb` and code-index prerequisites for thin clients, and the graph payload fields.

## Verification Notes

Verified in-tree evidence: `src/kb/kb_curator_index_code_unit.c`, `src/db2/schema.sql`, `src/db2/schema_sqlite.sql`, `src/db2/pgvec_transport.c`, `src/kb/kb_service_graph.c`, `src/kb/kb_service.c`, `src/db2/kb_service_backend.h`, and `src/cli_code_audit.c`.
