# Proposal: Recall economy progressive disclosure

- **State:** done
- **Completed:** 2026-06-09
- **Author:** JBailes
- **Date:** 2026-06-08
- **Charter roles:** Recall, Rewrite, Extract, Gate-Promote, Calibrate /
  Evaluate-Optimize

## Shipped work

This proposal is complete. Aimee's ingress context envelope now uses bounded
progressive disclosure for memory recall and exposes id-addressable follow-up
reads so the agent can pull full records only when needed.

Implemented behavior:

- `ingress_preinject_assembly_budget` bounds the final `<aimee-context>`
  envelope, including wrapper, code snippets, memory previews, footer, and
  truncation metadata.
- Memory recall inside the envelope renders concise preview records instead of a
  full opaque context block.
- Previews include stable `memory:<id>` handles when ids are available.
- The MCP surface exposes `memory_get {handle,id}`, backed by the existing
  `memory.get` chain, so every advertised memory handle is openable.
- Envelope footer guidance advertises `get_context_block` and `memory_get` as
  follow-up retrieval tools.
- The retrieval-shortcut store exists in DB2 and sqlite schemas, with lookup,
  observation, and promoted-shortcut use wired into memory search.
- Missing or unavailable headline previews fall back to bounded text snippets
  rather than full bodies.
- Tests cover budget enforcement, footer survival, truncation behavior, and
  memory-handle follow-up affordances.

## Verification evidence

- `src/server/ingress_preinject.c`
- `src/server/server_mcp.c`
- `src/mcp_tools.c`
- `src/server/kb_client_memory.c`
- `src/kb/kb_service_memory.c`
- `src/db2/kb_service_backend_memory.c`
- `src/db2/memory_query_bookkeeping.c`
- `src/db2/schema.sql`
- `src/db2/schema_sqlite.sql`
- `src/tests/test_ingress_preinject.c`
- `src/tests/test_mcp_tools_golden.inc`
