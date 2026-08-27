# aimee-kb

This directory owns the knowledge service and DB2: PostgreSQL, pgvector, memory, documents, code
graph, retrieval, curation, and KB administration.

It does not own DB1, workflow state, thin-client paths, or another KB's corpus.

## Boundaries

- the main service links libpq, never SQLite; the separate `aimee-kb-worm`
  process links the shared SQLite WORM implementation;
- accepts typed `/v1` operations from server and authorized KB clients;
- owns every DB2 transaction and background queue claim;
- owns embedding and synthesis role placement for this KB;
- runs a selected role inside the KB container or calls its configured remote endpoint;
- degrades explicitly when an enabled role is unavailable;
- publishes KB-side memory and tool audit through its own event bus;
- treats scope as authorization, not a search filter applied after the query.

## Storage

The default container starts private PostgreSQL 18 with pgvector and pgvectorscale when
`AIMEE_DB2_URL` is unset. External PostgreSQL uses the same schema and ownership.

HNSW is the normal dense index. Large corpus tables may use pgvectorscale's disk-backed index when
configured and available. Changing index type rebuilds the index; it does not re-embed source rows.

Workers claim DB2 queue rows with database locking so several KB workers do not process the same
item. Horizontal replicas still need sane database connection limits and one shared schema version.
The WORM worker is the exception: run exactly one instance against its persistent
SQLite file; a session lock rejects concurrent WORM consumers.

## Main areas

| Area | Responsibility |
| --- | --- |
| `http/` | public route boundary, auth, scopes, body limits, OpenAPI |
| ingest | content validation, staging, document/PDF pipeline, commit |
| memory | store, recall, dedupe, contradiction, temporal state, promotion/decay |
| vectors | embedding records, index choice, reconcile and repair |
| code | extraction, symbols, calls, cross-repo edges, blast radius |
| curator | typed fact extraction, review, reflection, lifecycle |
| background | bounded DB2-backed workers and health |
| audit bridge | PII-safe mutation identity and tool outcome publication |

## Checks

```bash
make -C src kb
make -C src check-linking
make -C src kb-target-isolation-check
make -C src kb-container-packaging-check
make -C src api-conformance-check
make -C src unit-tests
```

See [Knowledge](../../docs/KNOWLEDGE.md), [Storage tiers](../../docs/STORAGE_TIERS.md), and
[Public API](../../docs/PUBLIC_API.md).
