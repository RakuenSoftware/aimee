# db2 module

## Purpose and non-goals

`db2` is the KB-local process boundary for the shared PostgreSQL knowledge store. The first
increment registers a separately buildable C process and freezes its lifecycle-health wire format
while the existing implementation remains authoritative in the KB process. It does not activate a
second store owner, send SQL over the bus, or claim that the C implementation has already been
carved out of the KB link.

## Public contracts

The C process owns principal 29 and event `11521`. Its descriptor owns
`eventcontract/operations.json`, which generates the public C header and the fingerprinted positive
and negative wire vectors. The fixed eight-byte request carries only magic and wire version. The
fixed sixteen-byte response carries schema, `pg_trgm`, and KB-table evidence; unknown flags and
non-zero reserved bytes fail closed. Until the descriptor includes the complete DB2 C closure, an
exported standalone process returns typed `capability_absent` instead of reporting false readiness.

## Dependencies and consumers

- `config`: owns deployment configuration and the existing DB2 configuration surface.
- `module-runtime`: owns authenticated event-bus process admission and lifecycle transport.

No production consumer is switched in this increment. The existing in-process health callers remain
authoritative until the atomic activation slice moves every lifecycle caller and the DB2 DSN together.

## Providers and readiness

The transitional provider is `aimee-module-db2`, built as an independent C process from the
descriptor. DB2 continues to own PostgreSQL and pgvector. A future DB3 provider may serve portable
vector capabilities, but transactional and relationally coupled pgvector operations remain here.
The process is not ready merely because it attached: successful backend health evidence is required.

## Configuration and activation

The module is optional and disabled by default while the old in-process implementation remains the
single production owner. This avoids concurrent schema, pool, and migration ownership. Activation
is a later atomic image change that passes `AIMEE_DB2_URL` only to this process, enables generated
clients, and removes DB2/libpq objects from `aimee-kb`; there is no in-image local-call fallback.

- `runtime_toggle.supported`: `false`; the disabled process shell is a build artifact, not a
  production runtime choice. Activation changes the complete image contract atomically.

## Surfaces

The only current surface is `AIMEE_DB2_EVENT_HEALTH` on the KB-local Unix-domain module bus. There
is no HTTP listener, network service, generic query operation, raw SQL payload, or provider-secret
field. The catalog reserves the eight family identities and event kinds `11521` through `11528`, but
only lifecycle is active and granted. Later operations must be typed, bounded catalog entries.

## Data and migrations

This increment performs no reads, writes, schema changes, or migrations in `aimee-module-db2`.
The existing C DB2 owner retains those responsibilities until its complete source and dependency
closure is packaged and replay-tested behind this process boundary.

## Security and privacy

The wire contains capability bits only. DSNs, SQL, row contents, identities, and driver errors do
not cross the bus. Runtime admission continues to pin the executable path, UID, principal class,
principal reference, and event-kind grant. The `AIMEE_DB2_URL` secret remains with the current owner
until the activation image transfers it exclusively to the module process.

## Supported journeys

Build tooling exports and compiles `aimee-module-db2` from its descriptor. A test backend can prove
the complete health encode-handler-decode path. A production bundle without the still-unmigrated C
closure returns `capability_absent`, making partial packaging visible and non-authoritative.

## Tests and failure behavior

Focused C tests cover every response flag combination, malformed magic/version/length, unknown
flags, reserved bytes, wrong stage, undersized output, cancellation, missing callbacks, backend
failure, and successful encode-handler-decode. Runtime-bundle tests compile the descriptor-owned C
process. Catalog tests mutate every closed field, process/descriptor binding, resource limit, and
generated artifact. Boundary tests prohibit any direct import from `src/modules/db2/c` into private
`src/kb`.

## Operational diagnostics

Before activation this process is a packaging and contract probe only. `capability_absent` means the
standalone runtime has not yet acquired the complete DB2 backend; it is not a database-health
verdict. Existing KB health remains unchanged.

## Compatibility

No public KB route, CLI response, schema object, configuration key, or database behavior changes in
this increment. The event contract is versioned independently so the C and future Go providers can
share byte-for-byte replay fixtures. `AIMEE_DB2_EVENT_HEALTH` is additive and has no active caller
until the complete process cutover is ready.

## Extension and removal

Next increments complete the operation catalog, generate C client/dispatch, package the complete C
source closure, and add replay gates before activation. After parity, a pure-Go implementation
replaces the C process behind the same contract. The `src/modules/db2/c` tree is removed only after
the Go runtime is the sole deployed provider and every boundary test proves the old link and fallback
are gone.
