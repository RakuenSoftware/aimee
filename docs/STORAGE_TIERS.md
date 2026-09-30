# Storage ownership

Server and KB are independent instances. Both use PostgreSQL and the same Go storage provider;
each retains its own database, credentials, Vault, and backup boundary. Personal memory is durable
without a KB. See [Server and KB](SERVER_AND_KB.md) for deployment and request selection.

| Data domain | Behavior owner | Store | Contents |
| --- | --- | --- | --- |
| Server runtime, historically DB1 | `aimee`; `aimee-wfe` owns workflow lifecycle | Server PostgreSQL | Sessions, working memory, jobs, workflows, policy, caches |
| Personal memory and private code | Server placement of `memory` | Server PostgreSQL + pgvector | `user_memories`, retained revisions, correction proposals, private code and vectors |
| Shared knowledge | KB placement of `memory` and KB knowledge domains | KB PostgreSQL + pgvector | `memories`, facts, documents, shared code, evidence, curation queues |
| Server WORM | Server audit owner | Separate SQLite ledger | Append-only evidence chain and checkpoints |
| KB WORM | `aimee-kb-worm` | Separate SQLite ledger | Append-only KB evidence chain and checkpoints |

The table lists ownership domains, not five database containers. Standard Compose gives each
instance one PostgreSQL service. The Server runtime and personal memory use that Server store.
The optional KB has a different store even when both run on the same machine.

## Database access

The Go `postgres` module owns database connections, pools, transactions, and migration transport in
both roles. Memory calls it through the local event bus. Native KB knowledge algorithms use the
PostgreSQL session transport; they no longer own a libpq driver or pool. Thin clients and browser
clients never open either database. Server-to-KB requests use authenticated typed `/v1` operations.

The former DB2 process, driver, and namespace are retired. The knowledge schema is now
[`src/modules/kb/c/schema.sql`](../src/modules/kb/c/schema.sql). The retained DB1 migration-owner
identifier and old report names are compatibility/history details. They do not require a numbered
database architecture or authorize sharing a store between independent instances.

`AIMEE_STORE_URL` supplies runtime access and `AIMEE_STORE_MIGRATION_URL` supplies separate migration
authority. Both profiles are vaulted. The restricted WORM worker uses `AIMEE_WORM_POSTGRES_URL`
through the same provider implementation. See [Database](DB.md) and [PostgreSQL](modules/postgres.md).

## Memory and vector placement

Personal rows use instance-local user scope. KB rows use global, workspace, and project scopes.
The same Go executable enforces both placements. A scope or record ID cannot change the owner;
ordinary memory operations require explicit `store=kb` to leave Server's personal store.

Dense vectors remain with their source rows. Each instance configures its own embedder and optional
synthesis endpoint. Go memory uses versioned embedding generations; KB document/code vector
maintenance has its own schema and rebuild contract. See [Retrieval](retrieval-stack.md).

## Audit evidence

KB mutations submit an immutable PostgreSQL outbox intent in the row transaction. The separately
credentialed WORM worker appends committed intents to its SQLite chain and records delivery.
PostgreSQL commit and completed chain delivery are separate milestones. A worker outage leaves
pending intents; a failure to submit the required transactional intent fails the mutation.

The Server and KB worker share the WORM implementation but keep separate files, keys, and process
compartments. Detailed provenance and immutable content-free audit metadata have different retention
contracts. The [Atlas review](reviews/agent-memory-atlas-2026-09-29.md) records the remaining runtime
privilege discrepancy for rejection records; do not infer least privilege from a role's name.

## Deployment and recovery

Standard Server and KB Compose projects each provision a separate PostgreSQL container with
pgvector. Ordinary persistent storage is the default; LUKS is opt-in. An external database changes
location, not ownership. Never reuse a Server home or database volume for a KB role.

Back up each instance's home, Vault, PostgreSQL data, workspaces, and audit evidence together.
Use consistent PostgreSQL dumps or coordinated snapshots, and preserve the original identity when
restoring. Follow [Deployment](DEPLOYMENT.md#volumes-and-backup) and [WORM worker](WORM_WORKER.md).
