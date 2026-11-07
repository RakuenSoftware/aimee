# Generic code-index source observer

The SQL source owner can stream each committed project snapshot to a separately configured module through [the observer ABI](../../src/modules/kb/include/aimee/kb/index_observer.h). The public owner supplies project identity, authoritative generation and actual indexed UTF-8 file contents. The observer owns its transport, authentication, opaque handles and storage format. No datastore schema or memory policy is defined by this ABI.

A selected integration calls `aimee_kb_index_observer_require()` before source initialization, independently of callback registration. This requirement is one-way for the process. Missing or unavailable callbacks then refuse source mutations and startup; removing callbacks does not restore the legacy path. An unselected owner retains its existing behavior.

For each outer project mutation, the owner takes a shared global catalog advisory lock, then an exclusive project advisory lock on the **same leased PostgreSQL session**. The project key matches the existing purge-fence transaction key. It calls `available` and `begin` before indexed-source SQL changes. Nested calls for the same project reuse that scope and connection. After SQL commits, it enumerates the complete current-generation file snapshot and calls `file` for every row, including empty files, then `ready` with the exact count. The lock remains held across commit, enumeration and successful publication. Failure after commit is reported as failure and leaves the observer unavailable; it does not undo an already committed SQL transaction or invent a successful publication.

Cleanup explicitly releases project and catalog locks before connection return. A transaction left open on an error path is rolled back. An unlock failure marks the connection for discard rather than pool reuse. Scope handles cannot be reused after a connection closes. A process crash releases PostgreSQL's session locks; the observer must retain its own durable unavailable state until a later complete publication succeeds.

Startup and schema application take the **exclusive** global catalog lock before reading the complete project set. This blocks concurrent creation/deletion as well as file changes. They fence every existing project before source-affecting initialization, retain all project locks through publication, and acquire project locks by actual PostgreSQL advisory-key order with a byte-order name tie break. Callback availability for an empty catalog is still checked with the configured observer. Missing/detached projects invoke `unavailable`, never `ready`.

Selected startup currently requires an already installed schema at `AIMEE_KB_STORE_SCHEMA_VERSION`, with readable complete project identity/generation/lifecycle columns. A fresh, old, unknown or incompletely readable catalog refuses startup. Perform initial schema installation or incompatible migrations offline before selecting the integration. The observer module and its authenticated dependencies must be ready before the SQL owner starts. There is no automatic fallback to an unfenced startup.

The guarded public paths include project upsert/reattach, raw file upsert/replacement, complete scan begin/seal and retractions, partial scan publication, the legacy filesystem scan, matching/hidden file purges, cross-project startup cleanup, project deletion, detach, confirmed purge and retained-generation GC. Staging/aborting a scan manifest does not publish indexed files. The independent legacy server user-code forwarding path refuses when the observer is selected; it does not participate in this source contract.

A standalone raw file upsert that creates a row without its contents cannot publish a complete snapshot. Source-owner code must group that upsert and replacement in an outer mutation scope, as the guarded scan paths do. Invalid legacy paths, missing content or an observer's own bounds also prevent ready publication rather than silently dropping rows.

This is an optional local integration qualification, separate from generic lint.
It requires PostgreSQL tools including `initdb`, a local C toolchain, libpq headers,
and the selected Ouroboros ring installation. Run its explicit Make target with:

```sh
make -C src code-index-observer-postgres-check \
  AIMEE_TEST_POSTGRES_BIN=/path/to/owned/postgresql/bin
```

For an explicit new evidence directory, invoke the same runner directly:

```sh
python3 scripts/validation/code-index-observer-postgres.py \
  --postgres-bin /path/to/owned/postgresql/bin \
  --directory /path/to/new/disposable/cache
```

The runner creates and stops its own PostgreSQL instance, applies the actual schema and tests actual C mutators under a restricted runtime role. It uses a real libpq test transport and event-recording observer callbacks. It qualifies SQL/session ordering and cleanup; authentication, the separately implemented observer's storage semantics and an actual provider-send guard require their own integration tests.

The [2026-10-25 local qualification](../validation/code-index-observer-2026-10-25/README.md) retains the safe evidence and its explicit integration limits.
