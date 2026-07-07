# Storage Tiers

aimee uses two pinned storage tiers. They are not user-selectable runtime
backends.

| Tier | Ownership | Contents |
|------|-----------|----------|
| DB1 | `src/db1/` | Local user and session information, credentials, checkpoints, workflow sessions, local caches, local interaction/learning signals, and other same-user state. Backed by SQLite, owned by the local `aimee-server`. |
| DB2 | `src/db2/` | Durable knowledge for the configured KB scope: memories, rules, KB metadata, task and decision records, code index metadata, coordination records, and the dense vector indexes for that knowledge (pgvector extension, in-process). Backed by Postgres, owned by `aimee-kb`, and deployable as either a local single-user KB or a shared KB. |

Code outside a tier must call typed APIs instead of depending on a tier's
storage handles or provider vocabulary. The tier-boundary check in
`scripts/check_tier_deps.sh` enforces the public surface that has been cleaned
up so far.

Operationally, DB1 is local to the user profile and belongs to one
`aimee-server` process. It is where private runtime state and local evidence are
captured first. DB2 is the durable knowledge source for project, workspace, and
global facts plus their dense embeddings; it may be local to the same machine or
shared by multiple servers. Shared deployments should receive only information
appropriate for that shared scope, through the memory, learning, reflection, and
KB APIs that promote or reflect local server evidence into durable knowledge.

The vector tier was originally a separate Qdrant sidecar; it was folded
into DB2 as a pgvector extension in #1575, vectors live in the same Postgres
instance as the rest of DB2 and need no separate connection or service.
