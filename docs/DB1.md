# Legacy DB1 storage boundary

The [database contract](DB.md) describes shared implementation. This page describes the
Server runtime domain still named DB1 in migration history and native interfaces.
[Server and KB](SERVER_AND_KB.md) remain separate instance and storage boundaries.

DB1 is the retained name of the server runtime domain. In the current composition, `aimee`
owns its domain behavior and `postgres` owns database access. Durable personal memory uses the
Server placement of the memory owner in that instance's PostgreSQL store.

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

KB owns a separate PostgreSQL/pgvector corpus through its knowledge and Go memory domains.
Both roles use the same Go PostgreSQL provider implementation, with distinct deployment stores.
Server-to-KB access uses typed `/v1`, never cross-instance SQL. The former KB_STORE/DB2 process
and native libpq pool are retired. Server audit and the separately credentialed KB WORM worker
keep distinct SQLite evidence chains outside their canonical stores.

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
