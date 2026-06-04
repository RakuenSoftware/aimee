# aimee-kb

This directory is the ownership boundary and implementation home for the
`aimee-kb` service, the knowledge tier that owns **DB2** (Postgres + pgvector)
and everything derived from it. For the system-level picture see
[`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md); for the code-level module map
see the [Technical Reference](../README.md).

## What aimee-kb is

`aimee-kb` is a single C11 process that owns the DB2 schema and all knowledge
operations: the memory inference pipeline (dedup, contradiction detection,
temporal versioning, promotion/decay), the vector collections, the code index,
document ingest, and the curator. `aimee-server` holds **no** DB2 SQL; it reaches
all of this through the typed KB client over a Unix socket or HTTP. KB service
entry, dispatcher, background workers, mining, curator, ingest, and the HTTP
surface live here (`kb_main.c`, `kb_service*.c`, `kb_ingest_workers.c`,
`kb_curator_*.c`, `kb_background.c`, `http/`); remaining KB-owned flat modules
move here incrementally behind the same build targets.

It links `-lpq` (plus `-lzstd`/`-lssl`/`-lcrypto`) and is built with
`-DAIMEE_DB1_DISABLED -DAIMEE_DISABLE_DB2_SQLITE_SHIM`: it **never** links SQLite.
The boundary is compile-enforced (`make check-linking`, `readelf` symbol checks,
`scripts/check_tier_deps.sh`).

## Scaling contract: the many-user, horizontally-scalable tier

Where `aimee-server` is strictly 1:1 with a user, **`aimee-kb` is the shared tier
that scales out to serve many users at once.** Three properties make this work:

1. **All durable state lives in DB2 (Postgres).** A KB process keeps only
   transient state: connection pools, in-flight request buffers, worker loops.
   The knowledge itself (memories, rules, code index, tasks, embeddings) is in
   Postgres. A KB instance is therefore an effectively **stateless request server
   over a shared database**.
2. **It is reachable over the network.** Besides the local Unix socket, KB serves
   its full contract over HTTP on `:8741` (`AIMEE_KB_API_URL`, optional bearer
   token). Many independent `aimee-server` instances (i.e. many users on many
   machines) can point at one KB endpoint.
3. **Concurrent instances are safe with no external coordinator.** The background
   pipeline (ingest, curator, embedding, index-update) claims work straight off
   DB2 queues using `FOR UPDATE SKIP LOCKED`
   (`db2_kb_ingest_queue_claim_next`, `kb_curator_extract_code.c`), so two workers
   never claim the same row. Postgres *is* the coordinator.

Consequently you scale the KB the way you scale any stateless web tier: **run N
`aimee-kb` replicas behind a load balancer**, all pointed at one Postgres. Read
and query traffic (search, recall, index lookups, the hot path the servers hit)
fans out across replicas freely; the background workers on each replica drain the
shared DB2 queues cooperatively.

### The database is just standard Postgres

There is no bespoke datastore. DB2 is one ordinary Postgres database
(`aimee_shared` by default) with two extensions: `pg_trgm` (lexical) and `vector`
(pgvector, for dense embeddings). You scale it with the standard Postgres
playbook (vertical sizing, connection pooling such as PgBouncer, and read
replicas for query fan-out), and `db2_pool_size` (1-32, default 8) bounds each KB
instance's connection pool.

Vector search rides inside the same Postgres, so it scales with it:

- **pgvector HNSW is the default** for memory vectors (always) and for all
  small-to-medium corpora. No extra extension required.
- **pgvectorscale (StreamingDiskANN) is the opt-in scale-up** for large corpora.
  It is *additive*: built on top of pgvector, same `vector` column type and same
  distance operators, adding only the `USING diskann` index access method, which
  is disk-backed with bounded build memory. Selected per corpus table by
  `db2.vector.corpus_index` (`auto` | `hnsw` | `diskann`); `auto` flips to diskann
  once a table exceeds `db2.vector.corpus_diskann_threshold` (default 1,000,000
  rows) *and* the extension is present, and falls back to HNSW with a notice when
  it is not (`pgvec_corpus_index_choose` in `db2/pgvec_transport.c`).
- **Switching is a reindex, not a migration.** Data and queries are identical
  across index types; `aimee kb repair --reindex-corpus` rebuilds corpus indexes
  to the configured/auto type with no re-embedding and no query rewrite. Hot-path
  `memory_embeddings` / `kb_embeddings` stay HNSW unconditionally.

## Responsibilities

| Area | Modules | Notes |
|------|---------|-------|
| Service dispatch | `kb_service*.c`, `kb_main.c`, `http/` | Unix-socket + HTTP `/v1` request handling (~43 endpoints). |
| Memory pipeline | `kb_memory_embed.c`, `memory_*` (KB-linked), `db2/` | Store/recall/search, dedup, contradiction, temporal versioning, promotion/decay. |
| Vectors | `db2/pgvec_transport.c`, `kb_*_embed.c` | pgvector collections, index strategy (HNSW/diskann), repair/reconcile. |
| Code index | `kb_service_index.c`, `kb_service_code_embed.c`, extractors | Symbol extraction, `find_symbol`, callers, structure, blast radius. |
| Ingest + curator | `kb_ingest_*.c`, `kb_curator_*.c` | Staged document ingest, structured-knowledge extraction, review queue. |
| Background workers | `kb_background.c`, `kb_ingest_workers.c`, `kb_service_workers.c` | Drain DB2-backed queues via `FOR UPDATE SKIP LOCKED`. |
| Learning / mining | `kb_learning_synth.c`, `kb_mining.c`, `kb_reflection.c`, `kb_calibrate.c` | Implicit learning, reflection, calibration. |

## Boundary rules

Server code must not include KB internals directly; it speaks only the KB client
contract. KB code must not link SQLite or reach DB1. The build gates
(`make check-linking`, `make kb-target-isolation-check`,
`make kb-container-packaging-check`, `scripts/check_tier_deps.sh`) enforce both
directions.
