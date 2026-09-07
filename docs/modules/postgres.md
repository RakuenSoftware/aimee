# postgres module

## Purpose and non-goals

`postgres` is the generic PostgreSQL transport and transaction boundary used by domain modules. It
owns pools, prepared execution, migrations, and readiness without knowing domain business semantics.
KB and server use the same caller contract in `server-go/db`; see [Database](../DB.md).
It does not authorize users, invent schemas, expose arbitrary SQL to HTTP clients, or own knowledge policy.

## Public contracts

Principal `28` serves health at event `11265` stage `1` and bounded SQL/store operations at event
`11266` stage `2`. Requests name registered owners and operations with typed parameters; replies carry
typed status and bounded rows. Raw connection strings and credentials never cross the event bus.

## Dependencies and consumers

- `config`: supplies validated store and migration connection settings without exposing them to callers.
- `module-runtime`: grants principal `28`, supervises the process, and publishes stage readiness.
- `vault`: supplies persisted credentials and the volume-bound key for explicitly enabled LUKS storage.

The `aimee` and `memory` domain modules consume the shared database contract. Registered owners use the generic
transport only through reviewed catalogs; server routes and thin clients never call it directly.

## Providers and readiness

The Go implementation under `server-go/modules/postgres` provides health, checksum, migration, and SQL
execution. Readiness requires a live PostgreSQL connection and successful catalog checks. A reachable
server with the wrong schema or failed migration is degraded or unavailable, not reported healthy.

## Configuration and activation

- `runtime_toggle.supported`: `false`; both role compositions require the PostgreSQL process.

`AIMEE_STORE_URL` supplies runtime access and `AIMEE_STORE_MIGRATION_URL` supplies migration authority. Use
least-privilege roles and secret injection; filesystem DB paths and SQLite compatibility settings are ignored.

## Surfaces

The only supported surface is the authenticated module bus contract in `module_api.h` plus the Go
caller interface. Operators observe readiness through server health. There is no public REST SQL route,
interactive query console, or client flag that widens the registered operation catalog.

## Data and migrations

`postgres` stores no domain schema of its own. It serializes owner-scoped, checksummed migrations and
executes registered statements inside bounded transactions. Ledger creation and
migration/replay use the same transaction advisory lock as the native knowledge
bootstrap, including on an empty database. Runtime tables, including workflow rows, belong
to `aimee`; backups and restores use PostgreSQL-native `pg_dump` and `pg_restore` procedures.

### Container storage in 0.4.2

The module's `storage` package defaults to ordinary persistent Docker storage
(`AIMEE_POSTGRES_STORAGE=plain`, also the default when unset). It requires no
LUKS device access or storage administration capabilities. Host and volume-provider
encryption work transparently. LUKS2 is an explicit opt-in through
`compose.luks.yaml` for Server or `compose.kb.luks.yaml` for KB; each sets
`AIMEE_POSTGRES_STORAGE=luks` and supplies the required device and control-socket
configuration for the PostgreSQL container. Core's existing Vault is the only persistent key custodian;
its database-independent bootstrap must be available before PostgreSQL starts.
`AIMEE_POSTGRES_STORAGE_SOCKET` identifies the local control socket, not a key
source. The owner requests only its fixed, volume-bound Vault credential.

The optional LUKS bootstrap requires a Linux Docker host with device-mapper and loop
support. The deployment must supply the actual device-mapper block major and
allow the required storage administration operations; it must never fall back to
an unencrypted data directory. Key transport uses protected transient memory and
pipes. `AIMEE_POSTGRES_VOLUME_MIB` sizes a new encrypted volume; changing it on an
existing volume is refused until an explicit storage migration handles resizing.

An existing plain or encrypted store is never implicitly converted to the other
mode; selecting the wrong mode fails before serving it. Real-container regression
harnesses are `tests/e2e/postgres-plain-e2e.py` and `tests/e2e/postgres-luks-e2e.py`.
Implementation and release evidence are tracked in
[the storage validation report](../validation/release-0.4.2-postgres-luks-2026-09-06.md).

## Security and privacy

The `postgres` process keeps DSNs and database credentials local, separates migration from runtime authority, and
accepts only attested callers and registered operations. Parameters stay distinct from SQL text; logs
redact secrets and row content while retaining SQLSTATE, owner, operation, and transaction outcome.

## Supported journeys

At startup `aimee` asks `postgres` to verify and apply its ordered domain migrations, then uses typed store
operations for sessions and workflows. During a request the transport acquires a bounded pool lease,
executes one registered transaction, returns typed results, and releases or poisons the connection.

## Tests and failure behavior

Tests under `server-go/modules/postgres` cover health, checksums, pool behavior, malformed frames, and
database errors. Unknown owners or operations, checksum drift, timeouts, invalid rows, and lost
connections fail closed. Transactions roll back; the module never switches to a local substitute store.

## Operational diagnostics

Inspect principal `28` readiness, pool occupancy, migration owner/version/checksum, PostgreSQL SQLSTATE,
and connection latency. Correlate module request IDs with server health and database logs. Diagnostics
must not print `AIMEE_STORE_URL`, `AIMEE_STORE_MIGRATION_URL`, bind values, or returned domain rows.

## Compatibility

Events `11265` and `11266`, the stage numbers, owner identifiers, statement checksums, and wire versions
are compatibility boundaries. New fields must be additive and bounded. The removed DB1 SQLite file
contract, `AIMEE_DB1_PATH`, and direct domain SQL access have no 0.4.0 compatibility path.

## Extension and removal

Extend `postgres` by adding a versioned generic operation and cross-language fixtures, then register a
domain-owned catalog and tests. Do not add domain conditions to this module. Removal requires migrating
every declared consumer and preserving recorded owner/version/checksum history for audit and restore.
