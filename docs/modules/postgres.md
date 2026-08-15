# postgres module

## Purpose and non-goals

`postgres` is the KB-local process boundary for PostgreSQL operations as they move
out of the C data layer. The first bounded stage owns generic store health and
capability evidence: connectivity, the current-schema `memories` table, and the
`pg_trgm` extension. It does not own content authorization, identity resolution,
team membership, schema bootstrap, migrations, or arbitrary SQL dispatch.

## Public contracts

The Go process serves principal 28/event `11265` on the KB bus. Its fixed-size
health request contains only a magic and version; its response contains only
schema and extension bits. SQL, connection URLs, credentials, subjects, and
content never cross the bus. Malformed requests fail closed and query failures
return a typed module failure without a response body.

## Dependencies and consumers

- `config` supplies the existing `AIMEE_DB2_URL` process secret.
- `module-runtime` authenticates the exact executable, UID, principal, and
  event-kind grant on the KB-local bus.

The first consumer is the KB health response. Bootstrap and the local CLI doctor
keep their existing C probe because they execute before the module boundary is
available; later slices can move operations only after their startup ordering and
state ownership are explicit.

## Providers and readiness

The physical provider is `aimee-module-postgres`, a separately supervised Go
process placed with `aimee-kb`. Readiness requires a successful bounded query.
The result independently reports whether the base `memories` table and
`pg_trgm` extension exist, so a reachable but incomplete store is not reported
as ready.

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

Future slices should add typed, bounded operations with explicit wire contracts
and move each C caller in the same change. A generic SQL-over-bus stage is not an
extension point. The remaining `db2_health_probe` can be removed only after every
pre-module bootstrap and doctor consumer has an equivalent ordered boundary.
