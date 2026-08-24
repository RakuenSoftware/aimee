# postgres module

## Purpose and non-goals

`postgres` owns PostgreSQL, for whichever module owns the rows.

Two stages. Health answers whether the store is usable: connectivity, the
current-schema `memories` table, the `pg_trgm` extension, and the KB runtime
tables. Storage is the database itself -- statements, transactions with an
owner, and versioned schema migration -- on behalf of a caller that brings its
own SQL and its own meaning.

Two stages rather than two modules because they share one pool. A health probe
that opened its own connection would be reporting on a pool nobody serves from.

It owns no domain. There is nothing in it about memories, documents, grants,
corpora or sessions, and there must not be: a module that knew what a row meant
would be two modules. Content authorization, identity resolution and team
membership belong to whoever owns the tables.

## Public contracts

Principal ref 28. Event kinds follow the registry rule,
`4097 + 256 * ref + (stage - 1)`.

| stage | name | event kind |
|---|---|---|
| 1 | `postgres-health` | 11265 |
| 2 | `postgres-sql` | 11266 |

**Health** takes a magic and a version and answers readiness bits. No SQL,
connection URL, credential, subject or content crosses it.

**Storage** takes a statement and answers rows. Seven operations: `EXEC`,
`QUERY`, `BEGIN`, `COMMIT`, `ROLLBACK`, `MIGRATE`, `CURRENT_VERSION`.

Values are typed rather than rendered -- null, text, int, float, bool, text
array, bytes -- because a column set to NULL and a column set to empty are
different facts, and reading a `BOOLEAN` back as text is how every mining job
came to report itself disabled.

Statuses are integers on the wire and pinned by tests on both ends: OK 0,
InvalidRequest 1, Unsupported 2, LimitExceeded 3, StatementFailed 4,
Unavailable 5, MigrationFailed 6. Failures carry the SQLSTATE, because a caller
cannot retry sensibly without it: a unique violation is often an expected
answer, a foreign key violation is a caller error, and a lost connection is an
outage.

A statement PostgreSQL rejected and a statement it never received are different
answers. If the server rejected it, it sent an ErrorResponse and the reply
carries that SQLSTATE. No SQLSTATE means nothing was rejected, so a caller-side
deadline reports 57014 and a failed connection reports 08006 with
`Unavailable` -- the one failure worth retrying unchanged.

Every bound refuses rather than truncates: 1 MiB statements and cells, 4096
arguments, 4096 reply rows, 4096 reply columns, 64-byte statement ids. A caller
handed exactly the ceiling cannot tell a complete answer from a capped one.

**Transactions are owned.** A handle is minted from crypto/rand rather than a
counter, is bound to the principal and attachment that opened it, and is refused
for anyone else: the handle names a transaction, it does not confer the right to
drive one. Idle transactions are reclaimed, and the caller learns on its next
statement rather than at a commit that loses its writes.

**Migrations are namespaced by owner**, not by module. DB1 at version 7 while
DB2 is at 31 is a normal state. An owner is chosen once and never changed:
renaming one orphans its history, since the rows stay under the old name and the
next migration applies against a database that already has its tables. One
module may own several namespaces, and a namespace outlives the module that
created it.

## Dependencies and consumers

- `config` supplies the existing `AIMEE_DB2_URL` process secret.
- `module-runtime` authenticates the exact executable, UID, principal, and
  event-kind grant on the KB-local bus.

The health stage's consumer is the KB health response, including its
`db2_kb_tables_ok` field. Bootstrap and the local CLI doctor keep their existing
C probe because they run before the module boundary exists.

**Two modules reach PostgreSQL through the storage stage in a running
deployment**, and this section names them because a commit message is not a
deployment.

`control-plane` applies its own schema on every KB start and writes a row it
reads back. Its health stage reports storage from that evidence -- a migration
and a version read that both crossed the codec -- rather than from a store
having been bound, which would be equally true of one pointing at nothing.

`db2` serves its operations from here rather than from a pool of its own. Its
process is Go; the choice between this and a local pool is made once at startup
from whether a bus socket exists, and logged either way, because a module that
silently opened its own pool when the bus was unavailable would be the
architecture becoming optional without saying so.

**The C data layer is unchanged and stays.** It is a LIBRARY linked into the KB
as well as a module process: 168 files outside the module call `db2_*` directly,
765 distinct symbols from `src/kb` alone, and none of them goes through a module
process. What the flip changed is which binary answers db2's active event kind
on the bus and where that binary gets its connections. Moving those callers is
the C purge, and it is untouched by this.

**Only the active family is served.** Seven of db2's eight families are
catalogued and deliberately inactive; the contract's process-activation rule
refuses a grant for a kind the catalogue has not switched on. Activating one
grants callers its operations and is a catalogue decision with its own review --
not a consequence of changing which binary serves.

## Providers and readiness

The physical provider is `aimee-module-postgres`, a separately supervised Go
process placed with both `aimee-server` and `aimee-kb` -- one module, two
deployments, because DB1 and DB2 hold different schemas on different machines
and neither should own a second opinion about PostgreSQL. Readiness requires a successful bounded query.
The result independently reports whether the base `memories` table, `pg_trgm`
extension, and both KB runtime tables exist, so a reachable but incomplete store
is not reported as ready.

## Configuration and activation

The module is default-on during the staged migration and uses the same
`AIMEE_DB2_URL` already supplied to the KB container. No second DSN or credential
surface is introduced. Its optional-inventory classification preserves existing
principal references while the migration is incomplete; disabling it makes the
KB health dependency degrade rather than falling back to duplicate C policy.

- `runtime_toggle.supported`: `true`; changing the selection requires a process
  restart, and disabling this default-on migration stage degrades KB health.

## Surfaces

The only surface in this slice is the fixed-size `AIMEE_POSTGRES_EVENT_HEALTH`
event on the KB-local Unix-domain module bus. The process is colocated with the
KB and is not a network-reachable Aimee service. There is deliberately no
generic query event, HTTP listener, CLI endpoint, or management API.

## Data and migrations

This stage is read-only and performs no schema migration. Schema initialization
remains in the C `db2_init` startup substrate until a later slice can move it
without making startup depend on a process that requires the initialized store.

## Security and privacy

The `AIMEE_DB2_URL` DSN remains process-local and is neither logged nor returned. Driver errors
are collapsed to a typed internal failure so connection details cannot cross the
bus. PostgreSQL TLS behavior remains pinned by the deployed DSN; this module does
not weaken or add a parallel transport mode. The local event bus retains its
existing executable, UID, principal, and event-grant admission controls.

This local process boundary does not replace or reinterpret the existing
inter-service trust model. Network links such as `aimee-thinclient` to
`aimee-server` and `aimee-server` to `aimee-kb` retain mTLS, rotating bearer
tokens, and PAM or federated OIDC identity as configured. Each mTLS peer pair
must retain its own independently rotated certificate; in particular, the
server-to-thinclient credential is distinct from the server-to-KB credential.
Those controls terminate at their existing service boundary and are not
duplicated as a fourth identity layer on this KB-local module bus.

The distinct-key invariant is already enforced fail closed by
`identity_distinct_from_server()` in `kb_client_mtls.c`, with missing-pair and
key-collision coverage in `test_kb_http_routes.c`; deployment and independent
rotation remain specified in `docs/UPGRADING.md`. This slice neither replaces
nor bypasses that guard.

`AIMEE_DB2_URL` is read until a pool is initialized successfully. After that,
credential rotation requires restarting the module process; graceful shutdown
closes the cached pool before the replacement process starts.

## Supported journeys

On KB startup the module supervisor starts `aimee-module-postgres` from the
default manifest. A health request proves the store is reachable and reports the
two required capabilities. A missing process, invalid request, timeout, or query
failure degrades KB health visibly.

## Tests and failure behavior

Go tests cover every `StageHealth` capability-bit combination, malformed magic/version/length,
wrong stages, query failure, and expired invocation deadlines. The registry and
contract tests pin principal 28/event 11265. Failures never echo a request or
connection detail and never fall back to a second implementation in the KB
health path. Failed pool initialization is retried on a later health request,
and every attempt remains bounded by the caller's bus deadline.

## Operational diagnostics

Operators see the existing `db2_ok` and schema-dependent health verdicts. The
module emits no DSN or raw driver error. Bootstrap and doctor diagnostics remain
unchanged until their owning startup phase moves.

## Compatibility

The existing health JSON fields retain their meaning. `AIMEE_DB2_URL` remains the
configuration surface. The wire format is versioned independently so later
PostgreSQL stages do not reinterpret this health request.
Specifically, `db2_ok` means that the PostgreSQL store is reachable through the
module and has the base schema; the C `db2_health_probe` is no longer the source
of that health-path verdict.

## Extension and removal

**This document previously said a generic SQL-over-bus stage is not an extension
point, and stage 2 is exactly that.** The reversal is recorded rather than
quietly made, because the original reasoning was sound and someone will wonder
what happened to it.

The original position was that each operation should be typed and bounded with
its own wire contract, and each C caller moved in the same change. That is right
when the module owns a domain. It is wrong when the module owns a database: a
typed operation per statement would put every caller's meaning into this module,
which is the thing it must not know. Four hundred and forty-five typed
operations for one caller's schema, and a second set for the next caller's, is
two modules wearing one name.

So the generic stage is the boundary, and the discipline moved rather than
disappeared. The caller names its operation (`statement_id`) so this module can
say what a caller was doing without parsing SQL. Every bound is enforced here
rather than trusted from the sender. Transactions have owners. Migrations have
namespaces. What is refused is refused by this module, not by convention.

The remaining `db2_health_probe` can be removed only after every pre-module
bootstrap and doctor consumer has an equivalent ordered boundary.

Related: [vectordb](vectordb.md) is the optional contract for an external vector
store; [control-plane](control-plane.md) owns what is specific to aimee-kb.
