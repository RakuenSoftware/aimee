# Proposal: put DB2 behind a module boundary, then port it to Go

- **State:** PENDING — the C source boundary and separately buildable C process shell are registered;
  the descriptor owns the generated health contract, typed C client, and declaration-review source;
  all 1,351 external C declarations are mechanically inventoried, lifecycle health is reviewed as a
  retained-DB2 wire operation, and provider-specific pgvector declarations are explicitly
  retained/private. The typed client/handler path passes through the real event bus, but production
  remains on direct calls until the standalone C backend is complete.
- **Date:** 2026-08-15.
- **Charter roles:** Constrain-Verify / Gate-Promote.
- **Thesis:** DB2 was created as a portable source boundary and now resides intact at
  `src/modules/db2/c`. Preserve its behavior by putting that C implementation behind a KB-local
  module process first. Once
  `aimee-kb` has no direct DB2 linkage, replace that module's internals with pure Go without
  changing its event contract.

## 1. Decision

DB2 moves in two ownership transfers:

1. **C library to C module.** The relocated `src/modules/db2/c` implementation becomes the private
   implementation of a separately supervised `aimee-module-db2` process placed with
   `aimee-kb`. The C code, schema, SQL, transaction behavior, pool, and tests move together.
   `aimee-kb` reaches the process only through typed, versioned bus events and no longer links
   DB2 or libpq.
2. **C module to Go module.** A pure-Go implementation under `server-go/modules/db2` is built
   against the already-frozen event contract and proven against the C module. The provider
   executable then changes from C to Go as one release operation. Callers and public surfaces
   do not change again.

There is no cgo bridge. The module boundary is language-neutral: phase one uses the C bus
client; phase two uses the Go bus client. Production has one authoritative DB2 state owner,
one schema owner, and one implementation of every declared DB2 operation in both phases.

The existing read-only `server-go/modules/postgres` health process is evidence that Go can open
PostgreSQL and serve a KB-local event, but changing or retiring it is not part of this migration.
It owns no DB2 schema or mutation. A later cleanup may fold that bounded observer into DB2 after
the requested two-step move; this proposal neither depends on nor blocks on that cleanup.

This is a physical and language relocation, not a database redesign. PostgreSQL remains DB2;
`AIMEE_DB2_URL`, table identities, durable row IDs, tenant semantics, vector dimensions, and
the public KB HTTP/CLI contracts keep their meanings.

## 2. Why the existing boundary is portable

The C implementation already hid its PostgreSQL handle inside the former `src/db2` boundary. That
tree has been relocated intact to `src/modules/db2/c`; callers use typed
headers; code outside the boundary is not supposed to receive a libpq handle or issue SQL.
Schema, lifecycle, pooling, tenant transactions, query implementations, and pgvector transport
are all present under the same directory. That is the unit to package behind the process
boundary before changing its language.

Measured on `origin/testing` at `0916c09472`:

| Measure | Count |
| --- | ---: |
| C translation units under `src/modules/db2/c` | 141 |
| Headers under `src/modules/db2/c` | 137 |
| SQL files under `src/modules/db2/c` | 6 |
| C, header, and SQL lines | 92,852 |
| Files outside `src/modules/db2/c` that include a DB2 header | 297 |
| — production files | 147 |
| — test files | 150 |
| Direct DB2-header include directives outside the boundary | 967 |
| — production directives | 532 |
| Lines across the six SQL files | 16,349 |

The physical move also exposed the other side of the source boundary. At the relocation merge,
DB2's C and header files contain 187 project or vendored-header include directives that resolve
outside `src/modules/db2/c`: 122 host APIs, 45 private APIs from other modules, 17 vendored cJSON
includes, two public module APIs, and the generated schema header. The initial process-boundary
slice promoted the three shared management-authority contracts and their token-public dependency
out of private `src/kb` ownership;
the boundary now rejects any direct DB2 import from that private tree. A
directory boundary therefore exists, but the process build is not yet a standalone dependency
closure. `tests/baselines/db2/source-boundary-v2.json` accounts for every one of these imports and
the boundary gate rejects growth. Phase one must remove, invert, relocate, or explicitly replace
them with portable core/system dependencies before the standalone C process can activate; linking
the monolithic core into the DB2 executable does not satisfy the boundary.

The inventory is reproducible from the cited revision with these bounded commands (the include
pattern is intentionally the same for file and directive counts):

```sh
git checkout 0916c09472
rg --files src/modules/db2/c -g '*.c' | wc -l
rg --files src/modules/db2/c -g '*.h' | wc -l
rg --files src/modules/db2/c -g '*.sql' | wc -l
wc -l src/modules/db2/c/*.[ch] src/modules/db2/c/*.sql | tail -1
rg -l '#include [<"](?:\.\./)?db2/|#include [<"][^">]*db2[^">]*\.h' \
  src --glob '!src/modules/db2/c/**' | wc -l
rg -n '^#include [<"](?:\.\./)?db2/|^#include [<"][^">]*db2[^">]*\.h' \
  src --glob '!src/modules/db2/c/**' | wc -l
```

These planning-time counts are evidence, not a frozen migration manifest. The S2 declaration-audit
slice reruns the commands at its merge base and accounts for the full surface before
`catalog_complete` can become true; completeness gates use that current ledger rather than the
numbers above.

The include count is the client-adapter inventory, not a reason to repartition DB2. Those
consumers already meet a typed source boundary. Phase one preserves their operation semantics
while replacing local calls with generated request/reply codecs. Pointer-bearing structs,
callbacks, borrowed buffers, and transaction handles cannot cross a process boundary, so the
inventory classifies each public operation as a bounded request/reply, a module-internal helper,
or one composite transaction.

The authoritative PostgreSQL schema contains 202 literal table creations, 231 function
creations, 218 index creations, 74 trigger creations, 70 policy creations, and 10 view
creations. Phase one packages and applies those same SQL files from the C module. Phase two
embeds the unchanged files in Go. Neither phase creates a parallel schema.

## 3. Stable process contract

One event stage per C function would turn an implementation inventory into a permanent wire API
and exhaust the module's 255-stage space. DB2 instead exposes a small set of domain stages. Each
stage carries a closed operation discriminator and a versioned, bounded payload:

| Stage family | Initial responsibility |
| --- | --- |
| `lifecycle` | schema version, readiness, capability and embedding-dimension evidence |
| `tenancy` | team, project, membership, principal scope, and transaction-local tenant setup |
| `memory` | memories, facts, relations, lifecycle, recall bookkeeping, and export |
| `index` | vector, code, graph, CSS, corpus, artifact, and cross-repository storage |
| `learning` | signals, proposals, feedback, outcomes, rules, and synthesis persistence |
| `organization` | catalog, budget, rate, spend, egress, telemetry, and server registry rows |
| `custody` | DB2-backed vault metadata, management journals, WORM audit, and witness records |
| `maintenance` | migrations, re-embedding, calibration, mining, cleanup, and repair jobs |

These are storage-operation families, not new feature authorities. `memory`, `learning`,
`governance`, `audit`, and `vault` retain their policy decisions and public journeys. DB2 owns
the transaction and durable state beneath them. Grants remain meaningful because a caller may be
authorized for one family without receiving a generic database capability.

There is no raw-SQL event, prepared-statement event, connection lease event, callback-pointer
event, or transaction token. An operation that must be atomic is one request whose C handler in
phase one, and Go handler in phase two, opens, commits, or rolls back its own transaction. The
module never holds caller-owned state between bus calls.

The contract is frozen after the C cutover. Go must implement it; Go does not get a second
contract merely because its internal types differ. This is what separates the risky process
move from the language port and prevents every Go translation from reopening all KB callers.

### 3.1 Operation catalog and body envelope

`src/modules/db2/eventcontract/operations.json` is the phase-one source of truth. Each row has
`stage`, numeric `operation`, request/reply field schemas, maximum encoded sizes, scope class,
transaction class, idempotency class, and allowed result codes. Numeric stage/operation pairs are
never reused. The descriptor owns that file and the contract generator emits:

- `src/modules/db2/include/aimee/db2/module_api.h`, with constants and C codecs;
- `src/modules/db2/client/generated.c`, with typed KB-side calls;
- `src/modules/db2/runtime/generated_dispatch.c`, with validation and C handler dispatch;
- `server-go/modules/db2/contract_generated.go`, used unchanged by the Go provider; and
- `tests/baselines/modules/db2-wire-v1.json`, containing the contract fingerprint and positive
  and negative vectors.

The catalog bootstrap owns and generates the already-registered lifecycle-health operation first.
It reserves family IDs and event kinds `11521` through `11528`, but only lifecycle kind `11521` is
active in `process-contracts.json`; reserved inactive kinds are not grants. `catalog_complete` stays
false until the declaration and consumer audit accounts for the complete DB2 call surface. CI
rejects generated-header or vector-baseline drift and rejects a premature completeness claim.

The declaration gate is separate from codec generation. A checked-in review binds a symbol and its
normalized-signature hash to a closed disposition, family, DB3 placement, and reason. The generated
ledger cross-references the exact frozen consumer set, records harmless identical declarations, and
rejects conflicting signatures or unsupported C constructs rather than silently omitting them.
Test-only and unconsumed declarations do not become wire operations. The 61 currently externally
referenced `pgvec_*` declarations are explicitly `private-db2`/`retained-db2`; this prevents provider
names and PostgreSQL mechanics from leaking into the later DB3 contract without claiming that their
provider-neutral logical operations are ineligible. `catalog_complete` cannot pass while the ledger
contains an `audit-pending` production declaration.

The bus already carries event kind, correlation, deadline, cancellation, and fragmentation.
The DB2 request body therefore has only a 24-byte little-endian header followed by the declared
payload: `magic:u32`, `version:u16`, `header_len:u16`, `operation:u32`, `flags:u32`,
`payload_len:u32`, and `reserved:u32`. Replies use the same shape with reply magic and replace
`flags` with a closed result code (`ok`, `not_found`, `conflict`, `denied`, `retryable`, or
`invalid_state`). Unknown bits, nonzero reserved bytes, length mismatches, undeclared results,
and noncanonical unused tails fail as invalid requests. Transport cancellation, deadline, and
internal failure remain module-runtime statuses rather than domain replies.

Contract version 1 freezes when phase one activates. A later additive operation increments the
catalog without reusing identifiers; an incompatible field change requires a new version and a
dual-version compatibility window. The C and Go codec generators must reproduce the checked-in
fingerprint byte-for-byte.

### 3.2 DB2 keeps pgvector; DB3 observers take the portable surface

Pgvector remains part of DB2. It uses DB2's PostgreSQL pool, tenant transaction, schema migration,
backup, and readiness path; the C-module move preserves `pgvec_transport`, and the Go port uses the
same DB2 `pgx` pool. DB2 always retains enough canonical vector state and pgvector behavior to serve
correctly with no DB3 module installed.

DB3 is a separate, provider-neutral module contract for the subset of vector work that does not need
to execute inside DB2's PostgreSQL transaction. A Qdrant, Milvus, remote pgvector, or other adapter is
an implementation/observer of the logical `db3` module, not a new KB API. More than one admitted DB3
observer may be active at once.

The split is by correctness requirement, not filename or table:

| Stays in DB2/pgvector | May be served by DB3 when present |
| --- | --- |
| canonical vector copy, content hash, model/version/dimension, generation, tombstone, and outbox | provider-native ANN indexes and collection layout |
| schema and halfvec migrations, dimension/model drift refusal, and re-embed inventory | idempotent batch upsert/delete from committed outbox records |
| mutations coupled to canonical rows, project lifecycle, WORM evidence, or tenant transactions | bounded top-K candidate search over memory, document, PDF, curator, and code collections |
| SQL/RLS joins and final tenant/project/classification checks | provider-native prefilters expressed through the closed DB3 filter grammar |
| pgvector-specific code-audit/near-duplicate queries whose meaning depends on relational joins | index creation/tuning, point count, latency, health, scan, and generation backfill |
| authoritative rebuild source, exact integrity verification, and recovery fallback | optional candidate generation for DB2-validated audit/analytics operations |

This table is enforced by the operation catalog. An operation is DB3-eligible only when its contract
can be expressed with opaque point IDs, vectors, a typed scope/filter expression, and bounded results.
No DB3 event carries SQL, pgvector table names/operators, raw provider query JSON, DB2 connection
handles, or authority-bearing source rows. A DB3 hit is only a candidate: DB2 rehydrates it and repeats
the authoritative scope, lifecycle, quarantine, and classification checks before returning it.

### 3.3 DB3 observer contract and routing

DB2 is the sole publisher of DB3 mutation and search events. DB3 implementations are multiple
authorized observers of the same logical module contract:

- `db3.capabilities`: metric, maximum dimension/batch/top-K, supported filter nodes, and provider
  generation;
- `db3.apply`: idempotent committed upsert/delete/tombstone batches with operation and generation IDs;
- `db3.applied`: per-observer acknowledgement, durable watermark, lag, and typed failure;
- `db3.search`: query ID, collection, vector, metric, typed filter, top-K, bounded relative timeout,
  and required provider generation; and
- `db3.results`: query ID, generation, status, and bounded point IDs, distances, and ranks, with
  provider identity bound from bus admission rather than trusted from the body.

The proposed version 1 assigns notification kinds `11777` through `11781` in that order. Each DB3
notification uses a 40-byte bounded chunk envelope above the event body because valid vectors and
result batches can exceed one negotiated inline bus slot, while the core bus fragments only correlated
request/reply streams. The envelope binds wire kind, stream ID, total length, chunk count/index, and
offset. Reassembly is keyed by the host-stamped provider principal, kind, and stream ID; it rejects
out-of-order, duplicate, cross-provider, oversized, and noncanonical chunks. DB3 kinds retain the
bus's default blocking overflow policy so a fan-out never produces a silently partial message.

Mutation events use the bus's observer fan-out directly. Each admitted DB3 instance applies the same
committed outbox operation and publishes its own acknowledgement. Search is correlated scatter/gather:
DB2 snapshots the configured ready observers, publishes one query, accepts at most one result per
admission-bound provider identity until the deadline, and records missing/late observers explicitly.
The provider identity comes from module admission, not a self-asserted payload string.
The route snapshot, expected-observer set, and overall deadline remain DB2 correlation state. A
bounded relative timeout in each search body limits provider work because notification fan-out has
no request/reply cancellation stream; providers cannot rewrite that budget in a result body.

The logical `db3` module owns the result event kinds even when several admitted implementations
publish them; instances have distinct provider identities and executable attestations under that
module contract. A provider cannot observe a tenant or collection unless its grant and DB2 route both
allow it. All observers of a routed event are therefore authorized to see its vector and bounded
filter metadata.

Default routing is deterministic:

1. with no selected ready DB3 observer, portable operations use DB2 pgvector;
2. with exactly one selected observer, portable writes fan out to it and portable searches use it;
3. with several observers, committed writes may fan out to every routed observer, while search uses
   exactly one explicitly selected primary; and
4. an unavailable selected DB3 uses pgvector only when the declared route permits fallback. Fallback
   is never silent: readiness, trace evidence, and result provenance identify it.

Multiple installed observers do not imply score fusion, comparison, or multi-provider reads. Those
would be separate product contracts. Changing the selected serving provider requires matching model,
dimension, metric, collection content hash, and acknowledged generation; otherwise the route remains
on pgvector.

DB3 runtime work cannot activate before the phase-one C DB2 process owns the real serving path. The
provider-neutral package, descriptors, and observer runners land only with a C DB2 adapter that
publishes committed outbox work, performs pgvector fallback, and rechecks real DB2 candidates. This
preserves the requested sequence and prevents an unintegrated DB3 substrate from becoming the
headline deliverable ahead of its owner.

The DB3 contract is stable across the C-to-Go DB2 port. Provider endpoints and secrets stay inside
their DB3 processes. Adding a DB3 implementation changes its provider artifact/descriptor and the
conformance matrix, not the KB route, DB2 public stage, or caller grant.

### 3.4 Ordered implementation slices

The migration lands in independently testable slices, but activation remains atomic:

1. **S1 — boundary and process shell.** Relocate the C tree, inventory both sides of the source
   boundary, register principal 29/event 11521, compile a standalone C runtime bundle, freeze the
   lifecycle-health codec, and return `capability_absent` until the real backend closure is linked.
   Exit: the bundle builds, malformed wire vectors fail closed, and DB2 imports no private KB header.
2. **S2 — catalog and C closure.** Classify every external C declaration and consumer, generate the
   eight family dispatch surfaces, and remove or promote every dependency that prevents the complete
   descriptor-owned C source set from linking standalone. Exit: exhaustive catalog and no monolithic
   core link.
3. **S3 — replayable C process.** Package schema, DSN, pool, tenancy, and every catalog handler in the
   disabled C process; add reference-vs-process replay, schema, concurrency, cancellation, and fault
   fixtures. Exit: byte and database-effect parity while the KB still serves local calls.
4. **S4 — atomic C activation.** Start DB2 before consumers, move the DSN and every caller to generated
   bus clients, remove DB2/libpq from the KB link, and make failed DB2 readiness fail closed. Exit:
   only `aimee-module-db2` owns DB2 in the image.
5. **S5 — DB3 contract and providers.** Land the provider-neutral observer contract, pgvector default
   adapter, deterministic selection/fallback, committed outbox fan-out, candidate revalidation, and
   conformance tests for a fake and optional external providers. Exit: one selected external provider
   can replace every eligible operation without moving retained PostgreSQL-coupled work.
6. **S6 — pure-Go parity.** Implement the frozen DB2 catalog in `server-go/modules/db2`, embed the same
   SQL, and pass C-vs-Go replay, schema, tenant, concurrency, vector, DB3, durability, and performance
   gates. Exit: a descriptor/runtime switch selects Go without caller or wire changes.
7. **S7 — C retirement.** Deploy only the Go provider, remove the C tree and C-only shims after the
   compatibility window, and prove no C DB2 object, stale grant, or fallback executable ships.

Material changes to operation ownership, fallback semantics, observer selection, or the activation
boundary return to roundtable review. Mechanical catalog additions follow the frozen rules above.

## 4. Phase one: put the existing C boundary behind the module

Phase one is deliberately mechanical. It changes placement and call transport, not DB2 query or
schema semantics.

### 4.1 Physical layout and build support

Relocate the directory without flattening it:

```text
src/modules/db2/
  module.yaml
  eventcontract/operations.json
  include/aimee/db2/module_api.h
  client/{client.c,client.h,generated.c}
  runtime/{main.c,module_adapter.c,generated_dispatch.c}
  c/                         # the current src/modules/db2/c tree, paths preserved below this point
docs/modules/db2.md
tests/db2/{replay.c,fixtures/,schema_inventory.sql}
server-go/modules/db2/       # created in phase two; absent from the phase-one runtime closure
src/modules/db3/              # provider-neutral observer contract; no provider required
  eventcontract/db3-v1.json
  include/aimee/db3/module_api.h
tests/db3/{conformance/,observer_fanout/,routing/}
docs/modules/db3.md
```

`module.yaml` declares the C sources/private headers, generated public contract header, contract
source, tests, and docs. `src/modules/process-contracts.json` declares required process execution,
runtime `c`, KB placement, a newly allocated principal reference, and the eight stage event
kinds. The repository lock and canonical module inventory record `aimee-module-db2`; DB2 is required
for KB readiness and has no runtime-disable fallback.

The current C exporter only links `module_adapter.c` for a C process. Phase one first extends it
to compile every descriptor-owned C source, preserve relative include roots, and accept declared
system link requirements for libpq, pthreads, crypto, compression, and the remaining measured DB2
closure. The exported process pins the same `aimee-core` version as the repository lock and uses
`aimee_module_process_run` plus the canonical C bus client from that exact core package. It does
not copy or fork the bus implementation.

### 4.2 Catalog, codecs, and placement audit

Generate the operation catalog from externally visible declarations in the 137 headers, then
classify every symbol before writing codecs. Internal-only symbols disappear from the catalog.
Pointer-bearing operations are reshaped explicitly: input pointers become length-delimited fields,
output buffers become bounded replies, callbacks become complete bounded result sets or named
follow-up operations, and read-modify-write sequences become one composite transactional operation.
No generator infers ownership or serializes raw struct padding.

The vector entries are cataloged under provider-neutral operation names. `pgvec_*` symbols remain
private implementation targets during phase one and cannot appear in the generated KB client API.

Audit the 297 consumers by runtime placement before conversion:

1. the six `src/server` consumers must use the existing KB service/API or lose accidental DB2 type
   dependencies; they cannot attach to the KB-local DB2 bus;
2. the 49 `src/modules` consumers are split by declared placement—KB processes receive generated
   clients, while server processes use their owning KB contract;
3. the 79 `src/kb` consumers and 11 root/two tool consumers migrate by lifecycle, tenancy, reads,
   writes, maintenance, then custody/shutdown order; and
4. the 150 test consumers either test private C implementation units or replay public wire vectors.

The compatibility headers keep existing typed function signatures only while a consumer group is
being converted. They contain codecs, never SQL or connection access. A generated manifest accounts
for all 967 old include directives and fails on an unclassified or newly introduced one.

### 4.3 Supervision and atomic activation

Development may land the exporter, catalog, module binary, and adapters in reviewable commits, but
the C module remains disabled and the old in-process owner remains authoritative until the activation
commit. The activation commit performs one production ownership switch:

1. provision the DB2 grant and executable in the KB runtime bundle;
2. pass `AIMEE_DB2_URL` only to that executable and start it before KB routes/workers;
3. wait for lifecycle readiness after the existing schema, dimension, advisory-lock, and pool init;
4. enable every generated client and remove DB2 objects/schema/libpq from the KB link.

There is no runtime switch back to local calls. Failure to attach, initialize, or become ready keeps
the KB unready and fail-closed. Deployment rollback installs the preceding complete image.

The phase-one acceptance condition is structural: the same C DB2 code is running, but it is no
longer in the KB address space. No production binary other than `aimee-module-db2` can open
`AIMEE_DB2_URL`, apply DB2 SQL, acquire a DB2 connection, or link libpq.

Because the boundary already exists, adapter generation must be exhaustive rather than
hand-selected. CI derives the public-operation and outside-consumer catalog, rejects an unclassified
symbol, and rejects new direct DB2 includes while the compatibility adapter allowlist may only
shrink.

## 5. Phase-two: port the private implementation to Go

After phase one, the C module remains the sole production DB2 owner while the Go replacement is
built. Porting proceeds behind the frozen event vectors in dependency order:

1. lifecycle, schema embedding, pool configuration, health, cancellation, and shutdown;
2. tenancy resolution and transaction-local scope setup;
3. memory, vector, code-index, graph, corpus, and artifact reads;
4. ingest, content-row lifecycle mutations, learning, organization, custody, witness, and
   maintenance writes;
5. re-embedding, migration, repair, and multi-replica coordination paths.

The Go tree contains `handler.go`, `pool.go`, `schema.go`, `pgvector_store.go`, `db3_client.go`,
one package file per stage family, `contract_generated.go`, and `schema/*.sql`. `pgxpool` is the
only PostgreSQL transport used by DB2. Pool hooks
set and clear transaction-local tenant state; handlers receive caller scope from decoded contract
fields and never from process globals.

At the start of phase two, the six canonical SQL files move to `server-go/modules/db2/schema/`.
`schema.go` embeds them with `go:embed`; the differential C build consumes a generated byte header
from the same files. A checksum gate rejects edited generated bytes or a second authoritative SQL
copy. Schema ordering, substitution, advisory-lock keys, and recorded metadata remain explicit tests.

`tests/db2/replay` launches the C and Go executables separately against two disposable PostgreSQL
databases cloned from the same clean template. For each operation family it sends the checked-in
wire fixtures, compares status and canonical response bytes, then compares the affected schema rows.
Failure fixtures inject cancellation, deadline, reconnect, serialization conflict, ambiguous commit,
and tenant-pool reuse. Neither provider is connected to a production database during parity runs.

Each family can merge into the nonselected Go implementation after its replay group and every
predecessor group in the list above pass; the C module remains the sole deployed owner. The provider
switch is one descriptor/contract/lock change
from runtime `c` to `go`, permitted only when every catalog operation has a Go dispatch target, the
contract fingerprint is unchanged, all section 6 gates pass against both providers, and runtime-bundle
tests start only the Go binary. Rollback installs the prior C artifact; it is not an in-image fallback.

After one compatibility release, remove `src/modules/db2/c`, the C runtime/adapter targets, C-only
test shims, and C system-link declarations. Source and artifact scans reject a DB2 C object, libpq
reference outside the Go executable's dependencies, or a stale C provider grant.

Go may improve private structure, pooling, and type safety, but cannot reinterpret the event contract,
schema, scope, or failure semantics during the port. Semantic improvements are later, separately
reviewed changes.

## 6. Correctness gates

Phase one proves relocation parity; phase two proves language parity. Every applicable gate runs
against both module implementations:

- **Wire completeness:** every production DB2 call maps to a declared operation; malformed version,
  discriminator, length, enum, string, and unused-tail bytes fail closed.
- **Schema parity:** apply the C-packaged and Go-embedded schemas to clean PostgreSQL instances and
  compare tables, columns, constraints, functions, triggers, policies, indexes, grants, and recorded
  schema metadata.
- **Behavior parity:** replay shared success, empty-result, malformed-input, timeout, cancellation,
  serialization-conflict, reconnect, and rollback fixtures against the original library, C module,
  and Go module as applicable.
- **Tenant isolation:** exercise two principals and two projects on every scoped operation; a missing,
  stale, or mismatched scope fails closed and a returned pool connection carries no previous tenant
  state.
- **Vector compatibility:** preserve halfvec dimensions, distance operators, index choices, model
  identity, re-embedding locks, and source/vector version coherence. Run the provider-neutral
  conformance corpus against pgvector, a deterministic fake, and every optional provider; verify
  external-provider outbox lag, tombstones, backfill, comparison, and cutover behavior.
- **Concurrency:** run multi-replica startup, schema locking, pool saturation, retry, shutdown, and
  cancellation tests against the supported PostgreSQL versions.
- **Durability:** inject failures before statement execution, before commit, after ambiguous commit,
  and during reply publication. Retrying an idempotent request must not duplicate a durable effect.
- **Boundary enforcement:** the KB executable does not link libpq, no non-DB2 participant reads the
  DSN, no SQL crosses the bus, and no production file includes a private DB2 implementation header.
- **Performance:** compare ingest throughput, recall latency, vector-search plans, pool wait time, and
  bus overhead against the in-process baseline with explicit regression budgets before phase one;
  compare the same measures again before the Go provider switch.

Before phase-one activation, record the workload, hardware class, sample count, percentile method,
and approved numeric regression threshold for each performance measure in the checked-in baseline.
The same method and thresholds govern the Go comparison unless a separately reviewed baseline change
explains why the environment or product requirement changed.

The SQLite DB2 shim is not a second supported runtime. In phase one it remains private to C-module
tests. During the Go port, tests that require PostgreSQL semantics move to isolated PostgreSQL
fixtures; pure encoding and policy tests remain database-free.

The concrete CI surface is `db2-contract` (catalog/codegen/fingerprint and negative vectors),
`db2-boundary` (include, DSN-reader allowlist, ELF/import, SQL-location, and runtime-bundle scans), `db2-schema`
(clean apply plus catalog comparison), `db2-replay` (behavior/tenant/durability vectors),
`db2-concurrency` (multi-replica and pool reuse), `db2-vector` (dimension/operator/plan fixtures),
`db3-conformance` (provider contract, multiple-observer fan-out, scope recheck, outbox, generation,
scatter/gather, routing, and fallback fixtures), and
`db2-performance` (recorded in-process, C-process, and Go-process budgets). These are new deliverables
of this proposal and become named Make targets before activation; CI invokes the same targets.

## 7. Security and operational invariants

- The DSN and driver errors remain private to the active DB2 process and are never logged or returned
  over the bus.
- Runtime and migration roles retain their separation; the runtime role does not acquire DDL or
  superuser capability after startup.
- TLS verification, tenant predicates, RLS policies, advisory-lock keys, WORM append rules, and Vault
  custody checks retain their current fail-closed meanings.
- Readiness distinguishes transport reachability, schema readiness, vector compatibility, and tenant
  enforcement. A reachable but incomplete database is not healthy.
- Graceful shutdown stops new requests, drains bounded in-flight transactions, closes the pool, and
  only then reports the module stopped. A forced stop relies on PostgreSQL rollback, never on a later
  compensating write.
- Rolling deployment supports only explicitly versioned request/reply pairs. An incompatible KB/module
  pair refuses readiness; it does not fall back to in-process DB2 access.

## 8. Compatibility and non-goals

Both transfers preserve the current KB routes, CLI JSON, schema objects, row identifiers, timestamps,
embedding versions, tenant visibility, audit chain, and configuration names. Internal C struct layout
and function names cease to be compatibility contracts after phase one; the event vectors replace
them as the implementation-independent contract.

This proposal does not require a DB3 in the default deployment, remove pgvector from DB2, rename DB2,
split tenant authority by module, redesign the canonical schema, send SQL or provider query syntax
over the bus, enable cgo, or move KB HTTP ownership into a database process. DB3 observers may hold
multiple rebuildable serving indexes, but they do not become canonical knowledge stores; DB2 and its
pgvector state remain the integrity/recovery authority.

## 9. Definition of done

### Phase one acceptance

1. `db2-contract` accounts for every catalog operation and old include, reproduces contract version 1
   and its fingerprint, and passes C positive/negative vectors.
2. `db2-replay` returns the same canonical results and database effects for the in-process reference
   and C module, including tenant, cancellation, conflict, and durability cases.
3. `db2-schema`, `db2-concurrency`, `db2-vector`, and `db3-conformance` pass against the packaged C
   process; DB2 pgvector remains transactional and the no-DB3 route is behaviorally unchanged.
4. `db2-boundary` proves that the KB has no DB2 object or libpq import, only the DB2 executable and
   the pre-existing read-only PostgreSQL health process may read `AIMEE_DB2_URL`, no SQL crosses the
   bus, and no production consumer includes a private DB2 header.
5. Runtime-bundle tests prove DB2 starts before KB consumers, a failed DB2 keeps KB unready, the grant
   serves only declared kinds, and no local-call fallback exists.
6. `db2-performance` stays inside the approved process-boundary budgets.

### Phase two acceptance

1. Every catalog operation has a Go dispatch target; `db2-contract` reproduces the phase-one
   fingerprint from both languages.
2. `db2-schema`, `db2-replay`, `db2-concurrency`, `db2-vector`, `db3-conformance`, and
   tenant/durability fault suites pass against C and Go on isolated PostgreSQL databases.
3. `db2-performance` stays inside the approved Go-provider budgets and records query-plan parity.
4. The provider-swap change is limited to the descriptor, process contract/runtime bundle, generated
   repository lock, and C-removal closure; public KB and schema compatibility baselines are unchanged.
5. Runtime-bundle tests launch only the Go provider and prove the same startup/failure/shutdown order.
6. `db2-boundary` proves the C source, objects, shims, grants, and link declarations are gone and no
   fallback executable ships.

Operators change neither their database nor their public API configuration between the two phases.
