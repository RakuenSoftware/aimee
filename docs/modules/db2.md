# db2 module

## Purpose and non-goals

`db2` is the KB-local process boundary for the shared PostgreSQL knowledge store. The first
increment registers a separately buildable C process and freezes its lifecycle-health wire format
while the existing implementation remains authoritative in the KB process. It does not activate a
second store owner, send SQL over the bus, or claim that the C implementation has already been
carved out of the KB link.

## Public contracts

The C process owns principal 29 and event `11521`. Its descriptor owns
`eventcontract/operations.json`, which generates the public C codec, transport-agnostic typed C
client, the shared `server-go/db2` Go caller contract, and fingerprinted positive and negative wire
vectors. Both clients accept a narrow bus-call interface rather than depending on a daemon global,
so C, Go, `aimee-kb`, and parity harnesses exercise the same bytes and contract fingerprint. The Go
package is caller-side only during phase one: it does not read the DB2 DSN, open PostgreSQL, serve a
stage, or become a second DB2 owner. The fixed eight-byte request carries only magic and wire version. The
fixed sixteen-byte response carries schema, `pg_trgm`, and KB-table evidence; unknown flags and
non-zero reserved bytes fail closed. Until the descriptor includes the complete DB2 C closure, an
exported standalone process returns typed `capability_absent` instead of reporting false readiness.

## Dependencies and consumers

- `config`: owns deployment configuration and the existing DB2 configuration surface.
- `module-runtime`: owns authenticated event-bus process admission and lifecycle transport.

No production consumer is switched in this increment. `aimee-kb` now compiles the generated client
behind `kb_module_db2_health_probe`, but the existing in-process health callers remain authoritative
until the atomic activation slice moves every lifecycle caller and the DB2 DSN together.
Go consumers may use `server-go/db2.Client` to call that same C-served stage. The eventual
`server-go/modules/db2` provider imports the shared contract rather than defining a second wire
format. Its first nonselected lifecycle handler now ports the bounded readiness query behind an
injected database seam and replays `tests/baselines/modules/db2-wire-v1.json` byte-for-byte. It is
not registered by `aimee-module`, does not open the DSN or own a pool/schema, and cannot serve a live
runtime placement. Live end-to-end verification remains gated on the later activation slice; the
provider remains nonselected until full C-versus-Go replay and the atomic ownership cutover.

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

`check_db2_activation.py` makes that sequencing executable. Setting `enabled_by_default` is rejected
unless the descriptor declares every inventoried DB2 C translation unit, weak backend resolution is
gone, and the declaration ledger proves that `db2_health_probe` has no production direct callers.

## Surfaces

The only current surface is `AIMEE_DB2_EVENT_HEALTH` on the KB-local Unix-domain module bus. There
is no HTTP listener, network service, generic query operation, raw SQL payload, or provider-secret
field. The catalog reserves the eight family identities and event kinds `11521` through `11528`, but
only lifecycle is active and granted. Later operations must be typed, bounded catalog entries.
`module_api.h` is generated from that catalog and must not be edited by hand.

Declaration audit:

`declaration-review.json` is the reviewed source for transitions from the legacy C surface. Its
generated ledger accounts for all 1,351 non-static function declarations in all 137 DB2 headers,
records identical duplicate declarations by location, and fails on conflicting signatures. The
ledger also tokenizes every frozen consumer so test-only and production references cannot be
confused. At this checkpoint 166 declarations are unconsumed implementation details, 273 are used
only by private implementation tests, 61 externally referenced `pgvec_*` declarations are
explicitly private and retained in DB2, lifecycle health is a reviewed retained-DB2 wire operation,
and 850 production-consumed declarations remain
without a reviewed disposition.

The separate `vector-portability.json` audit covers all 76 declared `pgvec_*` symbols, including
internal and currently unconsumed surfaces. It distinguishes provider-neutral candidate searches,
post-commit mutation fanout, provider-local control, necessarily retained DB2 authority, and deferred
analytics. The provider-specific C names remain private DB2 implementation details; the audit records
which logical effects may later receive DB3 operations without leaking those names onto the wire.
See `docs/db3.md` for routing, admission, fallback, and revalidation invariants.

The descriptor also owns the database-free C reference route and wire codecs for the first portable
memory-candidate operation. Its internal pgvector, external provider, and authoritative candidate
checks are injected, so the route can be exhaustively tested before the private DB2 source closure is
linked. This is not a production cutover: the module remains disabled and no provider grant ships.

DB2's schema also contains the pre-activation DB3 durability owner. AFTER ROW triggers on the
reviewed pgvector mutation relations create canonical apply-v2 operations and per-principal
delivery obligations in the same PostgreSQL transaction as the authoritative vector write. A new
apply-capable provider receives durable catalog-driven cursors under the same advisory transaction
lock, then a Go worker backfills in bounded transactions while live writes also target that
principal; it cannot advertise ready search evidence until that backfill is acknowledged. With no
active or backfilling external provider, no outbox history is retained because a later snapshot is
authoritative. The Go dispatcher uses bounded `SKIP LOCKED` leases and
broadcasts over the event bus, while only authenticated per-principal applied acknowledgements
complete delivery. These paths remain unselected until the standalone C DB2 activation gate.

Link-closure audit:

`link-closure-v1.json` accounts for all 138 storage-owner C translation units in the private DB2
boundary. Three JSON/RPC composition units were rehomed unchanged to `src/kb/db2_adapters`: they call
high-level memory, dashboard, learning, and agent policy and therefore remain on the caller side of
the process boundary while their storage calls migrate to generated DB2 clients. Treating those
calls as injected DB2 callbacks would recreate the monolith inside the module. The
probe compiles each unit plus exact descriptor-owned support, combines only those objects with a
relocatable link, supplies no archive, shared library, helper stub, or weak definition, and records
the 205 genuinely external symbols plus every referencing unit. Each symbol has a reviewed
disposition and rationale. The current ledger contains 139 explicit system-link dependencies, zero
remaining vendored/generated inputs, 59 sibling or KB contracts to inject, and 7 support APIs to
promote. The standalone-link exit condition requires zero entries in the latter three groups; a
classified ledger alone is not enough.

The descriptor also replaces eighteen live host-config getters with one versioned immutable
startup snapshot. Retained DB2 sources include only the private snapshot header; the legacy
monolith continues resolving the same getter symbols from the config module until cutover, while
the standalone process resolves them locally after one validated install. Wrong ABI, NULL, and
unterminated command snapshots fail atomically, and normal plus hardened tests cover every field,
fail-closed defaults, requested embedder override, and retained state after rejected installs.

Process discovery and daemon spawning were extracted from the retained SQL backend into a
caller-side runtime adapter. Queue rows, transactional claims, and completion state remain in DB2;
the supervisor alone resolves and spawns the host executable. The legacy function and object
names remain linked during cutover, while the standalone DB2 closure no longer imports either
platform lifecycle API. This adapter is explicit S2 caller-side debt: its storage calls must become
generated DB2 client operations before S4, but process discovery and spawn policy never move back
inside DB2 or become injected module callbacks.

The closure probe now uses the same `AIMEE_DB1_DISABLED` and
`AIMEE_DISABLE_DB2_SQLITE_SHIM` mode as the standalone KB/DB2 build. The legacy SQLite evaluation
shim remains available to its existing tests, but its DB1 statement-cache hook and SQLite-only
system ABI no longer count as production module dependencies. The one newly active POSIX process
reference and its exact call site are pinned as part of this one-way probe-mode migration.

The first closure reduction promotes seven deterministic sketch primitives from `src/sketch.c` into
the descriptor-owned `support/sketch_primitives.c`. Their DB2 calls are confined to `c/sketch.c` and
`c/kb_payload.c`. Admission pins the reviewed source and owned-header hashes, exact exports, and base
references, permits only `sketch.h` and `string.h`, and permits only `memset` as a possible system ABI
import. An extra export, include, undefined symbol, weak definition, non-system reference edge,
descriptor omission, or failure to resolve all seven reviewed symbols fails closed. Fixed-vector
parity and sanitizer tests compare the support implementation with the still-authoritative monolith
during the pre-activation period. Both are registered in the native suite; the sanitizer target
compiles independent test, support, and monolith objects with ASan, UBSan, and FORTIFY enabled.

The second reduction promotes the three-function dynamic-string lifecycle used only by
`c/collab_rules.c`: `dstr_init`, `dstr_appendf`, and `dstr_steal`. The descriptor owns the exact
three-word `dstr_t` ABI and support implementation; admission pins both hashes, those three exports,
the sole base call site, the four-header source envelope, and only `realloc` and `vsnprintf` as
possible imports. Empty, formatting, repeated-growth, long-content, ownership-transfer, ABI-layout,
controlled allocation-failure, and sanitizer parity tests run against the pre-activation monolith.
The allocation-failure case also fixes both copies to preserve length, capacity, and content when
growth fails. The legacy fixed-buffer string uses elsewhere are not aliases of this lifecycle and
remain outside the admission.

The third reduction promotes `text_sanitize_utf8`, the allocation-free in-place repair used by
three DB2 units at five call sites. The descriptor pins its minimal `size_t (char *)` ABI, source
and header hashes, sole include, empty import set, exact base references, and single export.
Parity covers every non-NUL byte, valid boundary sequences, every truncated prefix, overlong
forms, surrogates, values above U+10FFFF, mixed text, empty input, and NULL under both the normal
and ASan/UBSan/FORTIFY builds.

The fourth reduction promotes `server_mgmt_read_selector_name`, used by only the DB2 management-read
journal. Its descriptor-private header pins the existing two numeric selector values and int-sized C
calling convention while the parity test compile-time checks both against the authoritative legacy
enum. Normal and ASan/UBSan/FORTIFY tests cover the complete signed 16-bit selector domain plus
full-width integer boundaries. The helper has no imports and no pgvector, DB3, provider, database,
event-bus, allocation, I/O, or platform edge, so it cannot alter either vector ownership boundary.

The fifth reduction promotes the adjacent UTC formatter and parser shared by 18 and two DB2 units,
respectively. The private header preserves the `size_t` and `time_t` ABI; admission pins both
exports, exact base references, the three-header source envelope, and only the observed system time
and parsing imports. The existing Linux `timegm` and Windows `_mkgmtime` branches remain intact.
Parity covers both stored timestamp spellings, date-only values, epoch and calendar normalization,
invalid basic ranges and separators, trailing input, NULL/empty input, three host timezones, exact
formatter grammar, parser round-trip, and a five-second wall-clock window under normal and hardened
builds. The two formatter callers that also log share the bounded process logger described below;
the time support unit itself retains no logging import.

The sixth reduction promotes `rel_type_normalize` and `rel_type_is_functional` without importing the
relationship enum, seed-table, or memory-ontology surface. The private header exposes only string,
integer, and `size_t` ABI. Admission pins both exports, four DB2 referencing units, the three-header
source envelope, and only the observed ctype and `strcmp` imports. Parity covers the legacy corpus,
every non-NUL one- and two-byte input, every output length through the full test buffer, canary
preservation, NULL input/output, zero-length output, all nine functional relations, and representative
multi-valued and non-canonical labels under normal and hardened builds. Seed iteration, lookup, and
enum text conversion remain deferred to a deliberate shared-type boundary. The byte parity preserves
the legacy process-locale ctype behavior, and the legacy/support comparison fails if the duplicated
functional set drifts. This slice moves total debt from 341 to 339 and portable promotion debt from
22 to 20.

The seventh reduction promotes `correction_behavior_to_text` and `rel_sensitivity_to_text`, the two
enum-to-column-literal switches used only by `c/rel_types_store.c`. A descriptor-private numeric ABI
records all six authoritative values without importing `rel_types.h` or memory-ontology types. The
parity test compile-time binds those values and both enum sizes to the monolith's int-sized calling
convention, then exercises every signed 16-bit value plus the remaining int boundaries. Both normal
and hardened builds preserve the legacy defaults: an unknown correction becomes `supersede`, while
an unknown sensitivity fails closed to `pii`. The support object has no imports. This slice moves
total debt from 339 to 337 and portable promotion debt from 20 to 18.

The eighth reduction packages exact descriptor-owned copies of the canonical vendored `cJSON.c` and
`cJSON.h`. The closure policy pins both copy hashes, requires byte equality with the non-symlink
vendor origins, freezes all 79 global exports, and binds the 25 DB2-consumed APIs to every original
call site. It also freezes the source's include order and permits only its observed C-runtime imports;
extra exports, includes, undefined symbols, weak definitions, origin drift, descriptor omission, or
failure to resolve any consumed API fails closed. Runtime tests exercise malformed and trailing-input
parsing, typed lookup, construction, deterministic compact printing, deletion, deep duplication,
comparison, and balanced custom allocator hooks under normal and ASan/UBSan/FORTIFY builds. This
eliminates the generated-input class from 25 to zero. The owned implementation itself imports the
libc `sprintf` ABI, so the honest net ratchet is total debt from 337 to 313 and reviewed system-link
dependencies from 144 to 145 rather than claiming that a transitive dependency disappeared.

The ninth reduction promotes `platform_random_bytes` and `platform_random_hex` behind a minimal
descriptor-private ABI. The support implementation preserves the existing POSIX `/dev/urandom` and
Windows `BCryptGenRandom` branches plus the bounded lowercase-hex formatter. Admission pins the
source and header hashes, both exports, the four DB2 referencing units, the complete include
envelope, and the five observed POSIX system imports. Deterministic I/O seams compare success,
open/read/close failure, zero-fill, length bounds, terminators, and buffer canaries byte-for-byte
with the authoritative legacy implementation. The real source is also exercised for nonzero and
nonrepeating output, eight-thread concurrency, fork safety, and ASan/UBSan/FORTIFY cleanliness.
This slice moves total debt from 313 to 311 and portable promotion debt from 18 to 16 without
changing database, pgvector, provider, event-bus, or activation ownership.

The tenth reduction packages `rel_types_seed_count`, `rel_types_seed_at`, and
`rel_types_seed_lookup` as one generated ontology unit. The existing compiled-table generator now
walks the authoritative `SEED_ONTOLOGY` once and can emit DB2's complete rows alongside the Go
memory table and its fixture; it does not parse or duplicate the C initializer. A descriptor-private
ABI mirror keeps monolithic relationship and memory headers outside the bundle while preserving all
kind arrays, inverse and correction policy, category, sensitivity, hierarchy, and status fields.
Admission pins the generated source and header hashes, three exports, four original references,
three includes, and only the adjacent normalization support API plus `strcmp` imports. Compile-time
checks bind every size, enum width, constant, and field offset to the authoritative row. Runtime and
hardened parity walk every row and unused kind slot, prove iteration bounds and pointer identity,
and compare canonical, normalized, missing, empty, NULL, and overlong lookups. This slice moves total
debt from 311 to 308 and portable promotion debt from 16 to 13 without importing the memory module
or changing database, pgvector, provider, DB3, event-bus, or activation ownership.

The eleventh reduction replaces the monolithic host logger import with a descriptor-owned process
sink. Seventeen DB2 units retain the existing `aimee_log` ABI while a startup-only installer binds
level, module, and a capped formatted message to the module runtime. The remaining six legacy
logger includes are localized to the same private header even though they currently emit no symbol.
Normal and hardened tests cover an absent sink, formatting, truncation, invalid levels and pointers,
and uninstall. The implementation imports only `vsnprintf` and has no KB logger state, allocation,
database, bus, pgvector, provider, or DB3 edge. This slice moves total debt from 207 to 206 and
portable promotion debt from 9 to 8.

The async-queue drain now receives a nonempty, NUL-terminated worker identity bounded to 127 bytes.
The KB caller supplies its session attribution and DB2 persists that exact value with the atomic
claim; DB2 no longer imports the host `session_id()` accessor. Invalid identities fail before any
configuration or database access, and database-backed plus HTTP-adapter tests cover rejection and
round-trip attribution. This moves total debt from 206 to 205 and portable promotion debt from 8 to
7 while leaving durable claim/state ownership inside DB2.

The remaining portable rows are named migration debt, not an assertion that copying is always the
right answer. DB2 owns the decision and must close it before activation: canonical-index extraction
and shell helpers trigger a typed indexing capability review; session identity and
briefing rendering trigger an injected process/config/memory contract review; executable discovery
and daemon spawning trigger a runtime lifecycle capability review. Those rows may be reclassified
only with a reviewed contract and replay evidence, so later slices do not repeatedly guess between
support copying and injection.

The gate rejects legacy source additions or omissions, support path escape, symlinks, content drift,
new unresolved symbols, non-system reference growth, missing evidence, and any attempt to make the
probe pass through helper objects or libraries. Resolved symbols are also surfaced as
review-required shrinkage so the ledger and its human-readable counts cannot silently become stale.
Regeneration never activates the module or asserts that the C closure is complete.

A review transition binds the symbol and normalized-signature hash to one closed disposition,
family, DB3 placement, and nonempty reason. Signature drift invalidates the review. Unsupported or
ambiguous C declarations, malformed lexical input, stale review rows, premature completeness, and
generated-ledger drift fail closed. No C signature is treated as a wire encoding: pointer buffers,
callbacks, and composite transactions still require an explicit provider-neutral operation.

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
failure, typed-client transport/protocol failures, and successful encode-handler-decode. A dedicated
integration test crosses the real authenticated event bus from the generated client through the
module runtime into the C handler and verifies non-zero evidence. Runtime-bundle tests compile the
descriptor-owned C
process. Catalog tests mutate every closed field, process/descriptor binding, resource limit, and
generated artifact. Boundary tests prohibit any direct import from `src/modules/db2/c` into private
`src/kb`. Declaration-ledger tests cover C linkage blocks, multiline and callback declarations,
comments/literals/directives, identical and conflicting duplicates, malformed nesting, resource
limits, signature-bound review transitions, pgvector retention, output symlinks, reproducibility,
and unchanged-output failure. Activation-gate mutation tests prove that an incomplete source list,
weak backend, or remaining direct production caller prevents enablement.
DB3-portability tests additionally prove exhaustive 76-symbol coverage, closed classification
identities, fingerprint drift, duplicate/missing/extra detection, ordering, malformed JSON, resource
limits, and copied-repository CLI behavior.
Route and authenticated-bus tests cover pgvector with no deployed provider, deterministic automatic
external serving, explicit override, unavailable and malformed providers, explicit fallback
provenance, candidate revalidation, finite/bounded codecs,
two-observer idempotent apply fanout, and one-server-only search.
Descriptor-owned cJSON tests additionally prove byte-for-byte vendor origin binding, exact exports,
complete consumed-symbol resolution, allowed runtime imports, malformed-input behavior, allocator
balance, descriptor closure, and sanitizer-clean execution.
Descriptor-owned randomness tests prove exact legacy parity under deterministic I/O, bounded failure
behavior, lowercase encoding, canary preservation, real entropy-source operation, concurrent calls,
fork safety, descriptor closure, and sanitizer-clean execution.

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

Next increments review the remaining production declarations, map them to typed operations or
private/compatibility dispositions, generate the remaining C dispatch families, package the complete
C source
closure, and add replay gates before activation. After parity, a pure-Go implementation
replaces the C process behind the same contract. The `src/modules/db2/c` tree is removed only after
the Go runtime is the sole deployed provider and every boundary test proves the old link and fallback
are gone.
