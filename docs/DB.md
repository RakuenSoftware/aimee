# Database

The target is one database used by KB and server, not numbered database tiers.
Runtime/session state and shared knowledge are domain responsibilities, not separate
database products. Shared storage does not imply shared authorization: caller identity,
workspace/project scope, runtime roles, and migration authority remain explicit.

## Shared contract

`server-go/db` is the caller-side database contract for both placements. It owns
driver-neutral rows, query/transaction interfaces, SQLSTATE classification, bounded
SQL wire encoding, and migration requests. It does not open a connection, hold a DSN,
serve a module stage, or own domain schema.

The `postgres` module remains the transport provider. SQL stays at principal 28,
stage 2, event 11266, with unchanged opcodes, typed values, checksums, and transaction
handles. Moving code does not grant a caller migration privileges.

The server-domain module and KB/server memory use this same client. Existing
server-domain API types alias the shared types while their domain callers migrate;
there is no duplicate implementation or error sentinel. Memory does not import
the server-domain module for database access.

## Remaining consolidation

This first extraction does **not** merge existing databases or rewrite deployment
credentials. The tree still has legacy paths and contracts named `db1` and `db2`,
separate schema bootstrap paths, and separate configuration surfaces:
`AIMEE_STORE_URL` / `AIMEE_STORE_MIGRATION_URL` and the KB's vaulted
`AIMEE_DB2_URL`. The KB also retains native PostgreSQL ownership during its
remaining implementation migration.

The next cuts must:

1. Reconcile the schemas under explicit domain migration ownership. Validate both
   on one PostgreSQL database, including existing-data upgrades, overlapping table
   definitions, RLS, runtime grants, and independent server identities.
2. Consolidate connection/bootstrap configuration and migration ordering without
   exposing vaulted credentials or granting runtime roles DDL authority.
3. Retire the numbered client/provider names with their generators, descriptors,
   deployment scripts, and tests together. Preserve wire IDs and recorded migration
   history unless a separately tested protocol/data migration replaces them.

Do not point two existing installations at one DSN as a substitute for this migration.
No database contents, volumes, certificates, or production credentials are removed
by the shared-contract extraction.
