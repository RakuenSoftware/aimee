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
client, and fingerprinted positive and negative wire vectors. The client accepts a bus-call
function rather than depending on a daemon global, so `aimee-kb` and parity harnesses exercise the
same bytes. The fixed eight-byte request carries only magic and wire version. The
fixed sixteen-byte response carries schema, `pg_trgm`, and KB-table evidence; unknown flags and
non-zero reserved bytes fail closed. Until the descriptor includes the complete DB2 C closure, an
exported standalone process returns typed `capability_absent` instead of reporting false readiness.

## Dependencies and consumers

- `config`: owns deployment configuration and the existing DB2 configuration surface.
- `module-runtime`: owns authenticated event-bus process admission and lifecycle transport.

No production consumer is switched in this increment. `aimee-kb` now compiles the generated client
behind `kb_module_db2_health_probe`, but the existing in-process health callers remain authoritative
until the atomic activation slice moves every lifecycle caller and the DB2 DSN together.

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

Link-closure audit:

`link-closure-v1.json` accounts for all 141 C translation units in the private DB2 boundary. The
probe compiles each unit plus exact descriptor-owned support, combines only those objects with a
relocatable link, supplies no archive, shared library, helper stub, or weak definition, and records
the 339 genuinely external symbols plus every referencing unit. Each symbol has a reviewed
disposition and rationale. The current ledger contains 144 explicit system-link dependencies, 25
pinned vendored/generated inputs, 150 sibling or KB contracts to inject, and 20 support APIs to
promote. The standalone-link exit condition requires zero entries in the latter three groups; a
classified ledger alone is not enough.

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
builds. The two formatter callers that also log do not create a support import: logging remains an
independent injected process-policy decision.

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
Route and authenticated-bus tests cover default pgvector, selected external serving, unavailable and
malformed providers, explicit fallback provenance, candidate revalidation, finite/bounded codecs,
two-observer idempotent apply fanout, and one-server-only search.

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
