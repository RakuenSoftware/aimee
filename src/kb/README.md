# aimee-kb

This directory owns the knowledge service: memory, documents, code
graph, retrieval, curation, and KB administration.

This is an optional instance separate from Server. It does not own Server personal memory,
runtime state, workflow state, thin-client paths, or another KB's corpus. See
[Server and KB](../../docs/SERVER_AND_KB.md).

## Boundaries

- database connections belong to the Go PostgreSQL module for both application roles;
- the native service uses its session transport and does not link libpq; the separate
  `aimee-kb-worm` process owns the shared SQLite WORM implementation;
- accepts typed `/v1` operations from server and authorized KB clients;
- owns knowledge transactions and background queue claim;
- owns embedding and synthesis role placement for this KB;
- standard Compose runs embedding and optional synthesis in separate model sidecars; external endpoints are configurable;
- degrades explicitly when an enabled role is unavailable;
- publishes KB-side memory and tool audit through its own event bus;
- treats scope as authorization, not a search filter applied after the query.

## Storage

The Compose stack provisions a separate PostgreSQL service. The PostgreSQL module
uses `AIMEE_STORE_URL` for runtime access and `AIMEE_STORE_MIGRATION_URL` for
bootstrap migrations. Both are vaulted; the native service opens no database
connections itself. There is no embedded-cluster or `AIMEE_DB2_URL` fallback.

HNSW is the normal dense index. Large corpus tables may use pgvectorscale's disk-backed index when
configured and available. Changing index type rebuilds the index; it does not re-embed source rows.

Workers claim knowledge queue rows with database locking so several KB workers do not process the same
item. Horizontal replicas still need sane database connection limits and one shared schema version.
The WORM worker is the exception: run exactly one instance against its persistent
SQLite file; a session lock rejects concurrent WORM consumers.

## Main areas

| Area | Responsibility |
| --- | --- |
| `http/` | public route boundary, auth, scopes, body limits, OpenAPI |
| ingest | content validation, staging, document/PDF pipeline, commit |
| memory adapters | transport to the Go memory owner for store, recall, temporal state, mutation and review |
| vectors | embedding records, index choice, reconcile and repair |
| code | extraction, symbols, calls, cross-repo edges, blast radius |
| curator | typed fact extraction, review, reflection, lifecycle |
| background | bounded PostgreSQL-backed workers and health |
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
