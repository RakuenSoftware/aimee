# db2 module

## Purpose and non-goals

`db2` is the KB-local process boundary for the shared PostgreSQL knowledge store. Its
disabled-by-default C process now compiles the complete descriptor-owned DB2 implementation and
freezes the lifecycle-health wire format while the same implementation remains authoritative in the
KB process. It does not activate a second store owner or send SQL over the bus; replay and the atomic
consumer cutover still precede activation.

## Public contracts

The C process owns principal 29 and event `11521`. Its descriptor owns
`eventcontract/operations.json`, which generates the public C codec, transport-agnostic typed C
client, the shared `server-go/db2` Go caller contract, and fingerprinted positive and negative wire
vectors. Both clients accept a narrow bus-call interface rather than depending on a daemon global,
so C, Go, `aimee-kb`, and parity harnesses exercise the same bytes and contract fingerprint. The Go
package is caller-side only during phase one: it does not read the DB2 DSN, open PostgreSQL, serve a
stage, or become a second DB2 owner. The frozen bootstrap health operation keeps its fixed eight-byte
request and sixteen-byte response. Every later catalog operation uses the generated 24-byte
request/reply envelope: distinct magic, a 16-bit version and header length, operation discriminator,
request flags or a closed reply result, exact payload length, and zero reserved bytes. Shared C/Go
positive and negative vectors pin that additive envelope without changing a health byte. The first
envelope-backed operation, `lifecycle.embedding_dimension`, returns the effective PostgreSQL/
pgvector schema width as a bounded `u32`, or the closed `invalid_state` result when no valid width is
available. The exported process binds every operation to strong production DB2 accessors; explicit
injected backends are confined to tests.

The next lifecycle operation, `pool_status`, returns a single mutex-protected snapshot of the
PostgreSQL pool: bounded size/occupancy, waiters, lease grants/timeouts, stuck leases, and poisoned
connections. Signed or relationally impossible backend values become `invalid_state` rather than
wrapping onto the wire.

`embedding_refusals` preserves the schema-dimension refusal evidence used by lifecycle health. It
returns the cumulative refused-offer count and last refused positive dimension in one response. The
only valid states are both zero or both positive, and the offered dimension is bounded by `INT_MAX`;
otherwise the process returns `invalid_state`.

`postgres_status` carries the best-effort PostgreSQL diagnostics used by `aimee doctor`: active and
maximum connections, recovery role, and standby WAL replay lag. A four-bit availability mask keeps
missing probes distinct from zero, requires unavailable values to remain zero, and permits lag only
for a known replica. A dead probe connection returns `invalid_state`.

`reembed_status` reads the canonical DB2 maintenance marker that blocks vector search during a
schema-width rebuild. It returns a bounded target dimension and positive start epoch, `not_found`
when maintenance is inactive, or `invalid_state` for an unreadable or malformed marker.
`reembed_clear` is its idempotent, zero-payload companion mutation: DB2 deletes that marker after
reconciliation, returning `invalid_state` if the single statement fails.

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

The current surface is the lifecycle event on the KB-local Unix-domain module bus. It serves the
frozen health operation plus the envelope-backed embedding-dimension, pool-status, and
embedding-refusal, PostgreSQL-status, re-embedding-status, and re-embedding-clear operations. There
is no HTTP listener, network service, generic query operation, raw SQL payload, or provider-secret
field. The catalog reserves the eight family identities and event kinds `11521` through `11528`, but
only lifecycle is active and granted. Later operations must be typed, bounded catalog entries.
`module_api.h` is generated from that catalog and must not be edited by hand.

Declaration audit:

`declaration-review.json` is the reviewed source for transitions from the legacy C surface. Its
generated ledger accounts for all 1,397 non-static function declarations in all 137 DB2 headers,
records identical duplicate declarations by location, and fails on conflicting signatures. The
ledger also tokenizes every frozen consumer so test-only and production references cannot be
confused. Every catalog operation names its exact C backend symbols; generation fails unless each
symbol has a signature-bound `wire-operation` review with the same family and DB3 placement, and
also fails if a wire review has no catalog operation. At this checkpoint 153 declarations are
unconsumed implementation details, 286 are used
only by private implementation tests, 61 externally referenced `pgvec_*` declarations are
explicitly private and retained in DB2, lifecycle health is a reviewed retained-DB2 wire operation,
the embedding-dimension, pool-status, embedding-refusal, PostgreSQL-status, re-embedding-status, and
re-embedding-clear backends are reviewed wire operations, and 888
production-consumed declarations remain
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
the 190 genuinely external symbols plus every referencing unit. Each symbol has a reviewed
disposition and rationale. The current ledger contains 139 explicit system-link dependencies, zero
remaining vendored/generated inputs, 51 sibling or KB contracts to inject, and zero support APIs to
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

The twelfth reduction removes generic shell execution from canonical indexing. The DB2 owner now
constructs fixed argv vectors for its exact `git` and `find` operations, while the KB host installs
the bounded `safe_exec_capture` capability once at service initialization, before HTTP routing or
ingest workers start. A missing installer logs an explicit rejection and
fails `canonical_index_scan_project` before database mutation; caller-pushed
`canonical_index_scan_files` remains process-free. The production index test covers absence and a
git root containing a space, including incremental co-change replay. This moves total debt from 205
to 203 and portable promotion debt from 7 to 5, removes one host-header dependency, and eliminates
both `run_cmd` and `shell_escape` without adding shell parsing to the module.

The thirteenth reduction closes the portable ledger with one generated fallback-parser cluster. Its
private ABI mirrors only definitions and call references; the generator derives the parser bodies
from the three authoritative extractor sources, embeds the shared C system-header policy, removes
the separately owned import-identity span, and supplies the normal build's unavailable tree-sitter
fallback. Normal and ASan/UBSan parity replay imports and system flags, exports, routes, definitions
and spans, and calls for all sixteen supported language families plus an unknown extension. This
moves total debt from 203 to 198 and portable promotion debt from 5 to 0 without adding DB, bus,
provider, pgvector, process, filesystem, network, logging, or configuration imports.

The fourteenth reduction realizes the two pure co-change calls as one bounded injected contract.
Its private pair ABI preserves object-id validation, deduplication, the bulk-commit gate, lexical
ordering, and output truncation. Normal and ASan/UBSan parity cover empty, duplicate, unordered,
boundary-length, hostile object-id, gated, and capped inputs against the authoritative monolith.
This moves total debt from 198 to 196 and injected-contract debt from 59 to 57 without adding git,
DB, bus, provider, pgvector, DB3, allocation, configuration, or logging dependencies.

The fifteenth reduction owns the three model-catalog validators at DB2's pre-database storage choke
point instead of importing their HTTP-layer implementation. Its contract freezes the exact wire
whitelist, printable-name bounds, HTTP(S) prefix grammar, and empty-endpoint default. Normal and
ASan/UBSan parity cover NULL, every non-NUL byte, lengths through 512, inclusive/exclusive bounds,
accepted and rejected schemes, and negative maxima. This moves total debt from 196 to 193 and
injected-contract debt from 57 to 54 while importing only `strcmp`, `strlen`, and `strncmp`.

The sixteenth reduction isolates the certificate-serial canonicalizer used by DB2 enrollment from
the broader identity owner. Normal and ASan/UBSan parity freeze prefix and separator removal,
process-locale lowercasing, leading-zero collapse, bounded output, and fail-with-empty-output
behavior for every non-NUL byte, short output capacities, and lengths across the internal 512-byte
boundary. This moves total debt from 193 to 192 and injected-contract debt from 54 to 53 while
leaving principal construction and authentication policy outside DB2.

The seventeenth reduction owns the string-only code-search line-enrichment helper. Normal and
ASan/UBSan parity cover NULL, malformed and repeated markers, empty and absent tokens, every
non-NUL byte, 256-line content, and token lengths through 2048 while preserving first-verbatim-match
and one-based line semantics. This moves total debt from 192 to 191 and injected-contract debt from
53 to 52 with only `strncmp` and `strstr` imports.

The eighteenth reduction adds a descriptor-owned node-kind text contract for the memory node-kind
serializer used by the relationship seed writer. Normal and ASan/UBSan parity freeze all seventeen
named persisted values, the `NODE_OTHER` sentinel, and every other signed 16-bit value plus integer
boundaries to `other`. This moves total debt from 191 to 190 and injected-contract debt from 52 to
51 without imports, shared ontology headers, allocation, I/O, DB, bus, provider, pgvector, DB3,
configuration, or logging dependencies.

The nineteenth reduction isolates `memory_pii_should_inject`, the final allocation-free decision in
DB2 recall after relation sensitivity has been obtained. Its descriptor-private numeric ABI freezes
the three sensitivity tiers and the `0.4` confidence floor without importing the memory classifier,
ontology tables, or registration state. Normal and ASan/UBSan parity cover every signed 16-bit
sensitivity, integer boundaries, values immediately around the floor, finite extremes, infinities,
NaN, and full-width false/true request values. Unknown tiers, credentials, low confidence, and NaN
remain fail-closed. This moves total debt from 190 to 189 and injected-contract debt from 51 to 50
without imports, allocation, I/O, DB, bus, provider, pgvector, DB3, configuration, or logging edges.

The twentieth reduction packages the import-identity pair used by DB2 code indexing together with
its required path-identity helper. The descriptor-private `size_t` ABI and 4096-byte workspace
preserve Python relative-import resolution, `__init__` equivalence, slash normalization, bounded
truncation, and empty-input behavior without importing the rest of the extractor monolith. Normal
and ASan/UBSan parity cover NULL, every non-NUL byte, POSIX and Windows separators, relative-dot
levels, all short output capacities, the internal path boundary, and importer/import/target
resolution matrices. This moves total debt from 189 to 187 and injected-contract debt from 50 to
48; its five C string/format imports were already declared system dependencies.

The twenty-first reduction packages the complete pure code-audit graph owner: dead-export selection
and bounded cycle discovery. Its descriptor-private edge ABI preserves borrowed export pointers,
import/reference tail matching, the 4096-node cap, deterministic traversal and rendering, duplicate
suppression, result limits, and cleanup without importing DB fetch or JSON assembly. Normal and
ASan/UBSan parity cover NULL and negative bounds, prefix variants, duplicates, DAGs, self/two/three-
node and overlapping cycles, null endpoints, disconnected graphs, limits, and a generated 64-node
graph. This moves total debt from 187 to 185 and injected-contract debt from 48 to 46; its allocation,
string, and formatting imports were already declared system dependencies.

The twenty-second reduction packages the remaining memory PII classifier owner while keeping the
already-isolated final injection decision separate. It includes turn cue scanning, direct and batched
relation sensitivity, plus both provider-registration seams; registered provider failures remain
authoritative and fail closed instead of silently falling back. The relation classifier composes only
with DB2's admitted seed and normalization support. Normal and ASan/UBSan parity cover every seed
relation, every non-NUL byte, sensitive-name and case/length boundaries, local and provider paths,
provider failures after partial writes, invalid batches, and output canaries. This moves total debt
from 185 to 182 and injected-contract debt from 46 to 43 without adding a system dependency.

The twenty-third reduction replaces DB2's direct audit-module row-hash call with one bounded
startup-installed host contract. The KB host installs the existing canonical WORM hash owner before
DB2 initialization; absent providers and non-lowercase/non-64-byte results fail before an append and
fail chain verification explicitly. Existing SQLite parity, lifecycle audit, and optional real-PG
mixed-chain tests preserve the pinned cross-engine digest. This moves total debt from 182 to 181 and
injected-contract debt from 43 to 42 without copying audit canonicalization into DB2.

The twenty-fourth reduction replaces DB2 artifact feature emission's direct KB MDL scorer call with
a startup-installed host contract that returns only the three scalar score values DB2 consumes. The
KB host retains canonical zstd scoring and installs a thin adapter; an absent or failed scorer keeps
the artifact commit successful while omitting the optional `mdl-v1` feature row, matching the prior
scoring-failure behavior. This moves total debt from 181 to 180 and injected-contract debt from 42
to 41 without copying the MDL implementation or its zstd dependency into DB2.

The twenty-fifth reduction replaces DB2 typed-fact commit's direct memory-gate call with a
startup-installed verdict contract. The KB host adapter encodes the bounded memory-module request
and obtains the authoritative verdict over the event bus; DB2 accepts only its pure verdict range
and returns `FACT_GATE_DEFER` without writing when the provider is absent, fails, or supplies an
invalid verdict. This moves total debt from 180 to 179 and injected-contract debt from 41 to 40
without giving DB2 ownership of memory policy.

The twenty-sixth reduction moves typed-fact pattern extraction and retraction scanning behind two
startup-installed memory-module contracts. The KB host adapters encode both bounded requests over
the event bus and validate decoded field sizes and flags; DB2 additionally rejects invalid counts,
unterminated fields, unknown node kinds, and inconsistent scan answers. Missing or failed
extraction is an error rather than a false zero-fact answer, while missing or failed scanning never
deletes a fact. This moves total debt from 179 to 177, injected-contract debt from 40 to 38, and
outbound source-boundary includes from 168 to 167.

The twenty-seventh reduction moves DB2's single-text embedding dependency behind a startup-installed
host contract. HTTP embedders now traverse the memory module's served `embedding` stage and use its
process-owned circuit breaker; program-based embedders retain the explicitly documented C host path
because the Go module declines them without touching its breaker. DB2 independently rejects an
absent provider, invalid dimensions, and non-finite components. This moves total debt from 177 to
176, injected-contract debt from 38 to 37, and outbound source-boundary includes from 167 to 166.

The twenty-eighth reduction replaces DB2 tenant setup's direct canonical identity-key call with a
startup-installed identity-owner contract. The callback receives only the principal fields the key
derivation consumes; DB2 pins the principal-kind ABI and independently revalidates the returned
owner, issuer-scoped OIDC, normalized certificate, or bounded host-account grammar before opening a
transaction or setting tenant GUCs. Missing, failed, unterminated, or noncanonical answers remain
unauthenticated failures. This moves total debt from 176 to 175 and injected-contract debt from 37
to 36 without copying identity canonicalization into DB2.

The twenty-ninth reduction replaces the two direct authority-record validation calls with one
paired, startup-installed host contract. Production installs the canonical management and identity
validators before serving token requests, while DB2 rejects an absent callback and every return
other than exact success. Focused boundary tests cover missing, failed, and invalid positive and
negative verdicts without weakening the existing exhaustive record and signing tests. This moves
total debt from 175 to 173 and injected-contract debt from 36 to 34.

The thirtieth reduction replaces DB2's parse/compare/free chain for retained computed-style
snapshots with one startup-installed CSS-owner contract. The provider returns only per-snapshot
validity, availability, equivalence, and a diff count; DB2 independently rejects missing providers,
provider errors, nonbinary flags, negative counts, and inconsistent verdicts before updating the
migration unit. This moves total debt from 173 to 169 and injected-contract debt from 34 to 30.

The thirty-first reduction moves CSS stylesheet parsing, release, and static class-token extraction
behind one startup-installed CSS-owner contract. DB2 independently bounds the nested rule and
declaration counts, validates every fixed field and binary flag, and accepts only bounded, unique,
terminated static class tokens. Missing providers or malformed output skip the derived graph write
instead of mutating it. This moves total debt from 169 to 166 and injected-contract debt from 30 to
27.

The thirty-second reduction moves DB2's nine credential-record cryptographic calls behind one
startup-installed vault-owner vtable. The boundary covers canonical v1/v2 AAD, random generation,
DEK wrap/unwrap, authenticated secret encryption/decryption, and KEK-check wrap/verify. DB2 accepts
only exact success, bounds every variable length, validates AAD output length, and cleanses secret
outputs on absence or failure. This moves total debt from 166 to 157 and injected-contract debt from
27 to 18.

The thirty-third reduction replaces DB2's mutation-budget lookup and canonical reseal
operation-ID/receipt helpers with one startup-installed vault-owner contract. DB2 bounds each
deadline by its local monotonic per-call window, independently validates lowercase operation IDs
against decoded bytes, and cleanses failed receipt/digest output. This moves total debt from 157 to
151 and injected-contract debt from 18 to 12.

The thirty-fourth reduction moves DB2's 12 witness hashing, canonical encoding, framing, signing,
and verification calls behind one startup-installed vault-owner vtable. DB2 independently bounds
wire and Merkle inputs, validates every provider verdict and encoded length, and cleanses hashes,
signatures, identities, and partial frames after absence or failure. This moves total debt from 151
to 139 and injected-contract debt from 12 to zero.

The remaining 139 unresolved rows are declared system links. The standalone C closure therefore
has zero packaging, injection, promotion, private-implementation, or dead-code debt. This completes
the S2 C closure gate; it does not activate the process owner or complete the S4/S6 ownership and Go
provider transitions.

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

When explicitly launched, `aimee-module-db2` opens PostgreSQL through the existing `db2_init`
lifecycle and applies or validates the unchanged DB2 schema. It remains disabled by default, so the
existing in-process owner retains deployed responsibility until replay passes and S4 transfers the
DSN, startup order, callers, and link ownership atomically.

## Security and privacy

The wire contains capability bits only. DSNs, SQL, row contents, identities, and driver errors do
not cross the bus. Runtime admission continues to pin the executable path, UID, principal class,
principal reference, and event-kind grant. The process reads `AIMEE_DB2_URL` during initialization,
never echoes it or a libpq diagnostic, and refuses bus attachment on missing or failed initialization.
Before opening PostgreSQL it also applies the existing `EMBEDDER_DIMS` contract: a valid value is an
operator pin, while an unset or malformed value uses the one declared config default. This preserves
the in-process dimension precedence without linking the config implementation into DB2.

## Supported journeys

Build tooling exports and compiles `aimee-module-db2` from all 138 descriptor-owned DB2 translation
units plus its reviewed support closure. The generated main opens the real backend before attaching;
the health handler then reports the real schema, extension, and KB-table evidence. Test injection
continues to cover every response and failure shape without providing a production fallback.
The descriptor declares both canonical DB2 SQL inputs for deterministic text embedding. Runtime
bundles generate `schema_data.h` in a temporary build directory, and standalone exports emit the
equivalent CMake rule and copy those exact inputs; neither path requires or modifies a source-tree
generated header. The same descriptor declares the 55 non-owned compatibility headers required by
the retained C implementation. Export validation requires sorted, normalized, real `.h` inputs,
copies them without claiming DB2 ownership, includes them in the repository source digest, and
rejects module-local entries that belong in `private_headers` or `public_headers`. A clean isolated
export now compiles and links the same 138 translation units without reaching back into the
monorepo.

The `db2-replay` target starts that packaged executable as principal 29 against a fresh pgvector
PostgreSQL database and an executable-bound bus grant. It compares the returned health bytes with
the generated reference codec, repeats the request through the typed client, verifies all schema,
`pg_trgm`, and KB-table evidence bits, terminates the process cleanly, and leaves the recorded
embedding dimension and representative schema tables for a separate database-effect assertion.
The replay also rejects an expired deadline, deterministically cancels a request only after it has
entered the bus, proves a later live health call drains any stale terminal reply, and reads the
effective dimension through the generated envelope client from the real initialized process. It also
requires that process to return its configured 16-slot pool and a bounded occupancy snapshot through
the generated client. The required
CI job runs this target and is part of the stable `unit-tests` aggregate. These are the first live
S3 replay groups; the remaining operation families and fault fixtures still gate S3 completion and
activation.

## Tests and failure behavior

Focused C tests cover every response flag combination, both embedding-dimension result shapes,
pool counter widths and occupancy relations, and
dimension bounds, malformed magic/version/length, unknown
flags, reserved bytes, wrong stage, undersized output, cancellation, missing callbacks, backend
failure, typed-client transport/protocol failures, and successful encode-handler-decode. A dedicated
integration test crosses the real authenticated event bus from the generated client through the
module runtime into the C handler and verifies non-zero evidence. Runtime-bundle tests compile the
descriptor-owned C process from a clean tree with no `src/schema_data.h`. Generator tests pin
UTF-8/C escaping, reproducibility, output location, path containment, ordering, duplicate symbols,
and symlink rejection; an exported miniature CMake project exercises the same rule where CMake is
available. Export tests pin compatibility-header path safety, ownership separation,
materialization, manifest admission, and missing-file failure. Catalog tests mutate every closed
field, process/descriptor binding, resource limit, and generated artifact. Boundary tests prohibit
any direct import from `src/modules/db2/c` into private
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

Before activation this real process remains disabled by default. `aimee_db2_module_init` in
`src/modules/db2/module_init.c` is the only startup path, and it has exactly two refusal modes.
An unset or empty `AIMEE_DB2_URL` prints `db2: AIMEE_DB2_URL is unset; refusing to serve`; a failing
`db2_init` prints `db2: database initialization failed; refusing to serve`. The second message is
deliberately opaque because the DSN can carry a password, so no libpq diagnostic is echoed — read the
database server log, not this process, to learn why the connection failed. Neither path partially
initializes: both return `-1` before any handler is registered, so a refusing module serves nothing
rather than serving degraded results. Once attached, the `AIMEE_DB2_EVENT_HEALTH` lifecycle response
is the strong DB2/KB health verdict. Existing deployed KB health remains unchanged until the S4
ownership cutover.

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
