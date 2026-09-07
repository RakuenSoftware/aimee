# Legacy DB1 storage boundary

The canonical direction is the [shared database](DB.md). This page describes the
remaining server-domain boundary; DB1 is a legacy name, not the target architecture.

DB1 is the server's PostgreSQL data tier. In 0.4.0, `aimee` owns its domain behavior and `postgres`
owns database access.

## Ownership

Two Go modules divide the work:

- **`aimee` owns domain behavior.** Its typed families define server sessions, conversations,
  agent work, workflows, identity, telemetry, policy state, lifecycle state, PKI state, and schema.
- **`postgres` owns database access.** It holds the DSN, connection pools, transactions, migration
  connection, and bounded SQL transport.

The `aimee` module opens no database. It calls `postgres` over the server event bus. The C server
uses generated typed clients under `src/db1_client/` and links no PostgreSQL driver.

See [aimee](modules/aimee.md) for the domain stages and [postgres](modules/postgres.md) for the
database transport.

## Boundaries

DB1 contains server-local and same-user state. It includes sessions, working memory, agent jobs,
workflow rows, checkpoints, policy and audit state, caches, and management state.

`aimee-kb` owns DB2, a separate PostgreSQL and pgvector tier for shared knowledge. The server reaches
DB2 through typed `/v1` requests. The server and KB also keep separate SQLite WORM evidence stores
outside DB1 and DB2.

## Configuration and migrations

`AIMEE_STORE_URL` supplies the runtime PostgreSQL role. `AIMEE_STORE_MIGRATION_URL` supplies a
separate migration role. The `postgres` module rejects a migration DSN that uses the runtime role.

The `aimee` module applies its schema before advertising DB1 stages. It waits for the PostgreSQL
transport during concurrent module startup, then refuses to serve if the store remains unavailable.

Back up DB1 with PostgreSQL tools or the deployment's export procedure. DB1 data lives outside
`~/.config/aimee/`, which contains configuration and local runtime assets.

## Compatibility

Older source and proposals may use `db1` as the name of the former C/SQLite module. Those documents
record their implementation point in time. Current code uses the `aimee` domain contract and the
`postgres` transport contract.
