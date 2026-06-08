# Proposal: graph-derived code-health audit (`aimee code audit`)

- **State:** draft — spun out of `context-preinjection-ingress.md` §G (P4)
- **Author:** JBailes
- **Date:** 2026-06-08

## Why this is separate

P1–P3 of the context-preinjection proposal (envelope + code-context + Claude
hooks + attention guard) are implemented and merged. §G (the code-health audit)
was always flagged "may spin out" — and on inspection it is genuinely a separate
subsystem, not a thin add-on, because **the data it needs does not exist yet**:

- **Dead exports / circular deps** need inbound/outbound edge traversal over the
  code graph. The graph tables (`entity_edges`, `code_projection_edges`) live in
  DB2 (kb-side), but there is **no `/v1` query** for "exported symbol with no
  inbound edge" or "import cycle" — those would be new kb handlers + endpoints.
- **Clone detection** needs a per-symbol `body_hash` grouping. The code index
  does **not** currently store a `body_hash` — the indexer (`kb_service_code_embed.c`
  / `db2/code_index*.c`) would need to compute and persist it.
- **Untested files / orphaned TODOs / DB-in-routes / missing error handling**
  are file/stem-level heuristics that could run client-side over the index, but
  they're only useful bundled with the graph-derived checks above.

So P4 requires kb-side schema + query work (new `body_hash` column + ingest;
new graph traversal endpoints), then a thin client command on top — a different
shape of change from P1–P3's ingress/hook glue.

## Sketch

1. **Indexer:** add `body_hash` (normalized-source hash) to the code-unit row;
   backfill on reindex.
2. **kb `/v1` queries (new handlers):**
   - `code.dead_exports` — exported symbols with no inbound `imports`/`references` edge.
   - `code.cycles` — DFS over import edges (cap ~10 cycles).
   - `code.clones` — group symbols by `body_hash`, span ≥ N lines, group size ≥ 2.
3. **Client command `aimee code audit [--json] [--fix]`:** runs the queries +
   the file/stem heuristics (untested, TODO/FIXME, DB-in-routes, missing
   try/catch around fetch/http), prints a debt score (0–100) + prioritized
   roadmap, and writes a short `AUDIT_CONTEXT` the ingress can pre-inject for a
   bounded window (ties back to P1: the agent starts each session knowing what
   to fix).

## Relationship to the shipped feature

This closes the loop on "Aimee as the single context surface": after P1–P3 make
the agent *explore* through Aimee, the audit makes Aimee *proactively surface*
code health into the pre-injection envelope. It is additive and independent — the
context-preinjection feature is complete and useful without it.
