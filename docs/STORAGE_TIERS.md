# Storage tiers

aimee has two product data tiers. They are ownership boundaries, not interchangeable backends.

| Tier | Owner | Engine | Contents |
| --- | --- | --- | --- |
| DB1 | `aimee` domain module through `postgres` | PostgreSQL | sessions, working memory, agent jobs, workflows, local policy/audit state, caches, same-user runtime data |
| Server memory and code | `memory` module through `postgres` | PostgreSQL + pgvector | personal memories, private repository source, definitions, call edges, local embeddings |
| DB2 | `aimee-kb` | PostgreSQL + pgvector | durable memories, documents, facts, evidence, code graph, embeddings, curation state |
| Server WORM | `aimee-server` | SQLite | append-only server evidence chain and checkpoints |
| KB WORM | `aimee-kb-worm` | SQLite | append-only KB evidence chain and checkpoints |

The Go workflow control plane uses DB1 for definitions, immutable snapshots, work items, artifacts,
parks, and lifecycle events. `aimee-wfe` owns workflow behavior and is the only workflow writer.

## Rules

- `aimee-server` sends SQL to nothing. Both stores are other processes: DB1 through the
  store module over the event bus, DB2 through typed `/v1` calls.
- `aimee-kb` never opens DB1.
- thin clients and browser clients open neither store.
- cross-tier work uses typed `/v1` operations.
- provider vocabulary and storage handles stop at the owning module.
- the two WORM owners share `modules/audit/audit_worm.c`, but use separate
  processes, files, and keys; the KB main binary still links no SQLite.

Build and dependency checks enforce these rules.

## Deployment

DB1 belongs to one server profile, and its module is told where to find it with
`AIMEE_STORE_URL`. Being PostgreSQL does not make it shareable: one profile, one database.
DB2 can serve one user, a team, or a company, but its contents
must match that scope.

The default KB container runs a private PostgreSQL 18 cluster with pgvector and pgvectorscale. An
external PostgreSQL server is still DB2; changing its location does not change ownership. Use the KB
export helper or `pg_dump` before moving it.

DB2 PostgreSQL contains an immutable KB audit outbox and delivery ledger, not the
WORM chain. The separately deployed KB WORM worker persists that chain in its
own SQLite volume.

Dense vectors live beside their source rows in the owning instance: local memory and code vectors on the server, shared knowledge vectors on the KB. The old Qdrant sidecar is not part of the
current topology.
