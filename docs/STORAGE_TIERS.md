# Storage tiers

aimee has two product data tiers. They are ownership boundaries, not interchangeable backends.

| Tier | Owner | Engine | Contents |
| --- | --- | --- | --- |
| DB1 | `aimee-server` | SQLite | sessions, working memory, agent jobs, local policy/audit state, caches, same-user runtime data |
| DB2 | `aimee-kb` | PostgreSQL + pgvector | durable memories, documents, facts, evidence, code graph, embeddings, curation state |

The Go workflow control plane has its own SQLite store for definitions, immutable snapshots, work
items, artifacts, parks, and lifecycle events. That store is not DB1 or DB2 and has one writer:
`aimee-wfe`.

## Rules

- `aimee-server` never links libpq or sends SQL to DB2.
- `aimee-kb` never links SQLite or opens DB1.
- thin clients and browser clients open neither store.
- cross-tier work uses typed `/v1` operations.
- provider vocabulary and storage handles stop at the owning module.
- the shared WORM chain primitive may cross the boundary only while it remains pure hashing.

Build and dependency checks enforce these rules.

## Deployment

DB1 belongs to one server profile. DB2 can serve one user, a team, or a company, but its contents
must match that scope.

The default KB container runs a private PostgreSQL 18 cluster with pgvector and pgvectorscale. An
external PostgreSQL server is still DB2; changing its location does not change ownership. Use the KB
export helper or `pg_dump` before moving it.

Dense vectors live in DB2 beside their source rows. The old Qdrant sidecar is not part of the
current topology.
