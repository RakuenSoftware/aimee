# Provider-specific, proof-gated economizer implementation proposal

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

> **Archived complete (2026-07-26).** The scoped implementation and safety gates are
> present on `testing` with dedicated tests.

- **State:** DONE. Delivered scope archived 2026-07-26.
- **Review status:** CONVERGED (final review aligned, zero issues)
- **Date:** 2026-07-22
- **Normative gate:** `provider-neutral-economizer-safety-spec.md`
- **Initial release:** proof infrastructure only; live transform registry empty

## Executive decision

Replace estimated generic economizer policy with two independent local planners, an empty
release-reviewed transform registry, and one unconditional dispatch fence. The initial implementation
cannot mutate a provider request because the registry contains no authorized transform. It creates the
mechanical foundation required for later, separately reviewed OpenAI and Anthropic transforms.

There is no runtime shadow mode, remote token-count preflight, predicted-savings report, restore and
resend, recall, rehydration, rescue, or generic provider formula. Offline fixtures may exercise the
planners against recorded requests and settlements, but those fixtures are not reachable from a live
request path.

This proposal is self-contained on the normative v2 pillars: provider-specific local planners; an
empty live transform registry until separate converged review; explicit-only initial cache layouts;
exact protected-prefix and cache-state parity; symmetric full-price treatment of unknown residency;
finite complete-call cost bounds; local-only planning; authenticated tenant isolation; and exactly one
atomic provider dispatch. A conflict with the safety specification is a release-blocking error.

## Existing code audit and required disposition

| Existing surface | Disposition before release |
|---|---|
| `gw_economizer_measure()` | replace estimates with local provider planner call; empty registry yields pass-through |
| `gw_buffered_mutate()` | remove from production dispatch; a future reviewed transform enters before `econ_wire_snapshot` |
| pristine request buffer | retained only until the one wire commit; never used for post-send restore/resend |
| `context_reduce()` | no production caller until a concrete transform receives separate approval |
| `tool_condense_apply()` | disable production caller; lossy condensation is outside v2 |
| `build_fold_view()` | disable production caller; history folding is outside v2 |
| `agent_compress_tool_result()` | no production mutation until provenance and a transform contract are approved |
| Anthropic cache toggling | remove economizer control; preserve client `cache_control` exactly |
| 4xx restore/resend | remove unconditionally before proof infrastructure is enabled |
| savings ledger/stats | remove savings deltas; retain only qualified proof inputs, decisions, and settlement incidents |

## Architecture

```text
authenticated request context
        |
        v
local provider/model/API classifier
        |
        +----> OpenAI GPT-5.6 planner
        |
        +----> Anthropic Claude planner
                         |
                         v
                empty transform registry
                         |
                         v
                  PASS_THROUGH only
                         |
                         v
       immutable econ_wire_snapshot + exact-length transport
```

Classification, pricing lookup, contract lookup, tokenization, proof construction, and registry
membership checks are local. They cannot open a provider connection or make a provider API call.

## Core types

All money uses checked fixed-point integers in the signed account price table's declared currency and
precision. All arithmetic rejects overflow. No floating-point value can authorize mutation.

```c
typedef enum {
    ECON_PASS_THROUGH,
    ECON_INTERVENE,
    ECON_INDETERMINATE
} econ_decision_t;

typedef struct econ_tenant_handle econ_tenant_handle_t;
typedef struct econ_kill_switch econ_kill_switch_t;
typedef struct econ_cohort econ_cohort_t;

/* Opaque immutable handles; only authenticated dispatcher context can construct them. */
struct econ_cohort_key {
    provider_id_t provider;
    endpoint_id_t endpoint;
    model_snapshot_id_t model_snapshot;
    tokenizer_id_t tokenizer;
    pricing_table_id_t pricing_table;
    contract_versions_t contracts;
    uint64_t registry_generation;
};

typedef struct {
    uint64_t ordinary_input;
    uint64_t cached_read_input;
    uint64_t cache_write_input;
    uint64_t output;
} econ_token_buckets_t;

typedef struct {
    econ_cache_outcome_t cache_outcome;
    econ_token_buckets_t baseline_lower_tokens;
    econ_token_buckets_t baseline_upper_tokens;
    econ_token_buckets_t candidate_lower_tokens;
    econ_token_buckets_t candidate_upper_tokens;
    money_t baseline_lower;
    money_t baseline_upper;
    money_t candidate_lower;
    money_t candidate_upper;
} econ_scenario_t;

typedef struct {
    const econ_tenant_handle_t *tenant;
    const econ_cohort_t *cohort;
    const econ_kill_switch_t *kill_switch;
    account_id_t account_id;
    task_id_t task_id;
    call_id_t call_id;
    endpoint_id_t endpoint_id;
    model_snapshot_id_t model_snapshot_id;
    tokenizer_id_t tokenizer_id;
    pricing_table_id_t pricing_table_id;
    uint64_t pricing_generation;
    contract_versions_t contract_versions;
    transform_id_t transform_id;
    transform_version_t transform_version;
    buffer_identity_t pristine_buffer;
    buffer_identity_t candidate_buffer;
    econ_scenario_t scenarios[ECON_MAX_SCENARIOS];
    size_t scenario_count;
    money_t safety_margin;
    econ_reason_t reason;
} econ_proof_t;
```

`ECON_MAX_SCENARIOS` is a release-pinned compile-time constant for each planner, with a static
assertion equal to that planner's closed scenario enumeration. The opaque tenant handle owns immutable
tenant-scoped planner, account, pricing, contract, tokenizer, cohort, registry-view, and kill-switch
handles. Its accessors require the authenticated tenant identity and reject mismatches. The cohort
has a signed generation; its key is the tuple above plus tenant identity. The tenant kill switch is an
opaque signed-generation handle. Cohort disablement atomically increments the generation checked at
the first wire write.

`econ_proof_t` is an affine in-memory object. It is created for one call and two fully serialized
buffer objects, consumed once by the wire-snapshot selector, and cannot be persisted, cached, copied to
another tenant/call, or looked up by generation. No prompt-derived digest is logged.

For every scenario, authorization hard-asserts:

```text
baseline_lower <= baseline_upper
candidate_lower <= candidate_upper
candidate_upper + safety_margin < baseline_lower
```

Any failed assertion is `ECON_INDETERMINATE`.
Before these comparisons, checked arithmetic must prove every token bucket, money bound, and margin is
finite, representable, nonnegative, and free of overflow. A protected-prefix/cache-state mismatch is
a deterministic `ECON_PASS_THROUGH`, not an arithmetic indeterminate result.
The same checks apply symmetrically to baseline lower/upper, candidate lower/upper, and every margin
component. A compile-time assertion proves the maximum price quantization plus the maximum priced
token guard plus `ECON_MIN_MARGIN_PRICE_UNITS` fits in `money_t`.

## Empty transform registry

The production registry initially contains zero entries. The only implementation in this proposal is:

- a signed registry version and generation;
- exact membership lookup by provider, endpoint, pinned model snapshot, tokenizer, transform ID, and
  transform version;
- an atomic generation check at wire commit; and
- denial when membership is absent, stale, or ambiguous.

The signature covers the complete registry and every membership tuple, including provider, endpoint,
pinned model snapshot, tokenizer, transform ID, and transform version, not only version counters.

Transform algorithms, provenance capabilities, MIME/schema rules, semantic contracts, property tests,
and enablement are not authorized here. Each transform requires a separate proposal and converged
review before a signed registry update. Until then, `ECON_INTERVENE` is unreachable in production.

## OpenAI GPT-5.6 planner skeleton

The live skeleton accepts only a pinned snapshot and a locally pinned exact tokenizer. Aliases are
indeterminate. It parses without modifying:

- Responses versus Chat Completions shape;
- `prompt_cache_key`;
- `prompt_cache_options.mode`;
- every explicit breakpoint and its canonical provider-bound byte range;
- client output limit or documented hard model/API maximum; and
- service/batch/account pricing modifiers.

Scenario buckets explicitly cover ordinary input, cached reads, 1.25x cache writes, output, no-write
fallback, and every documented tier boundary. The GPT-5.6 boundary operator is strict `input_tokens >
272000`. `ECON_GPT56_TOKEN_GUARD` is a finite release-pinned constant derived from the pinned
tokenizer's documented maximum error (zero only when byte-equivalence is proven); either count with
`abs(count - 272000) <= ECON_GPT56_TOKEN_GUARD` is indeterminate. The 2x
input and 1.5x output rates apply to the full request in the over-boundary scenario.

Each scenario fixes one cache outcome and applies it symmetrically to baseline and candidate. Unknown
residency is full-priced ordinary input for both. Every breakpoint's read/write/miss state, including
explicit omission, must be identical in baseline and candidate; any mismatch is pass-through. Initial
live transforms, when separately approved, must use explicit-only mode and have no explicit or
documented implicit cache boundary in the mutable suffix.

The planner validates locally pinned provider-contract versions; these represent documented pricing
and API semantics, not inferred cache residency. A stale contract disables the adapter.

## Anthropic Claude planner skeleton

The live skeleton requires an exact model snapshot and a pinned locally executed exact tokenizer.
Remote provider token counting exists only in a separately linked offline test binary. The production
planner target neither defines nor links a remote-count client or symbol.

It parses without modifying:

- ordered Messages content blocks and system blocks;
- every `cache_control` marker and TTL;
- block count, order, type, byte ranges, and effective marker coverage;
- client output limit or documented hard model/API maximum; and
- base/read/5-minute-write/1-hour-write/output prices and long-context modifiers.

Residency-bearing fields are `cache_control` type and TTL, breakpoint positions, ordered block count,
block order/type/byte ranges, and all bytes under marker coverage. Unknown values in those fields are
full-priced symmetrically. Unknown JSON members, beta headers, and server-tool semantics are not
residency facts and are indeterminate.

Each explicit protected block and all framing through its marker must be byte-identical. A documented
implicit boundary anywhere in the protected prefix or mutable suffix, an unknown server-tool/beta behavior, an unsupported field,
or a cacheability change is indeterminate. Unknown residency is full-priced for both alternatives;
other unknown semantics are not residency and remain indeterminate.
Both alternatives use the same pinned canonical JSON serializer, including identical key ordering,
whitespace, escaping, system/message block order, and unknown-field preservation. The protected
serialized byte ranges, not decoded semantic objects, are compared for exact equality.

## Immutable wire-snapshot fence

The initial empty-registry release freezes the final provider body at the last economizer-aware
boundary:

1. `off` returns the already serialized pristine body without allocation or registry work;
2. `proof_gated` verifies the signed registry and, because it must be empty, selects only pristine;
3. the selected bytes are copied into an immutable `econ_wire_snapshot` with an explicit length; and
4. buffered, streaming, and retry transports receive that exact pointer and length until the call
   finishes.

No production path may rebuild, restore, or substitute an economizer representation after snapshot
selection. Ordinary transport retry remains owned by the existing retry layer. It may duplicate the
same request after an ambiguous failure, but every attempt uses the same snapshot bytes. This release
does not claim exactly-once delivery or an atomic first-positive-byte generation lease.

The stronger proof-consuming generation lease described by the normative safety specification is a
future prerequisite for a non-empty transform registry. It is intentionally not implied by the
empty-registry snapshot fence.

## Tenant isolation

Tenant identity comes only from authenticated dispatcher context, never request-body metadata.
`econ_tenant_handle_t` owns planner context, account price table, contracts, tokenizer, cohort,
registry view, proof, and `econ_kill_switch_t` as immutable tenant-scoped handles. There are no
module-global mutable provider/account handles. Missing or mismatched tenant/account identity returns pass-through before candidate creation
and is asserted again at the first wire write.

## Configuration

```yaml
economizer:
  mode: off | proof_gated
```

`off` is the default and bypasses all planning with byte-identical baseline behavior. `proof_gated`
still passes through while the signed registry is empty. Safety margin is derived mechanically as the
maximum of three finite release-pinned values: signed-price quantization, the named tokenizer guard
constant priced by a checked formula at the scenario's maximum rate, and
`ECON_MIN_MARGIN_PRICE_UNITS`. It is not derived from unbounded runtime input or operator-tunable.

## Telemetry

No predicted or realized savings delta is exposed. Tenant-scoped records may contain:

- decision and reason code;
- provider, endpoint, pinned model, tokenizer, pricing, contract, and registry versions;
- scenario baseline-lower and candidate-upper cost bounds;
- returned usage buckets for the sent request; and
- a settlement-bound-exceeded incident.

Records contain no prompt content, prompt-derived digest, raw cache key, task/session aggregate, or
field named `saved`, `savings`, `verified_savings`, or equivalent. A returned settlement above a bound
does not rewrite history; it disables the affected adapter cohort and emits an incident.
No field or derived metric may encode a baseline-versus-candidate arithmetic delta, percentage
reduction, effective savings rate, or equivalent under another name.

## Implementation slices

### Slice 0: baseline and removal

1. Capture byte-exact off-mode goldens across OpenAI Responses/Chat Completions and Anthropic Messages.
2. Remove 4xx restore/resend before enabling any new planner path.
3. Disable production callers of lossy condensation, folding, recall, and rescue.
4. Remove economizer cache toggling and preserve client cache intent exactly.
5. Introduce opaque tenant/cohort/kill-switch handles and remove every module-global mutable
   provider/account/cohort handle before the dispatch fence is added.
6. Regenerate and explicitly reapprove goldens whenever a pinned model snapshot, tokenizer, provider
   contract, or serializer changes; unreviewed drift blocks release.

### Slice 1: types, registry, and fence

1. Add checked money, named token buckets, scenario arrays, reason codes, and affine proof ownership.
2. Add the signed empty registry and exact membership/generation validation.
3. Implement `econ_wire_snapshot` as the immutable empty-registry dispatch fence.

### Slice 2: OpenAI local planner skeleton

1. Parse canonical API shapes, explicit cache layout, pinned model/tokenizer, and local price contract.
2. Enumerate symmetric ordinary/read/write/no-write/output/long-context scenarios.
3. Implement the strict `>272000` operator and guard-band denial.
4. Keep mutation unreachable because registry membership is empty.

### Slice 3: Anthropic local planner skeleton

1. Parse canonical block layout, marker coverage, TTL, pinned model/tokenizer, and local price contract.
2. Enumerate symmetric ordinary/read/5m-write/1h-write/output/long-context scenarios.
3. Reject unknown fields, server-tool semantics, aliases, remote token counting, and boundary changes.
4. Keep mutation unreachable because registry membership is empty.

### Slice 4: offline verification

1. Exercise planners only in separately linked test binaries against recorded provider requests and usage objects.
2. Verify bounds, drift incidents, and adapter disablement without attaching candidates to live calls.
3. Submit each proposed transform as a separate artifact for converged review.

## Release gates

- Off-mode provider bytes match the frozen baseline.
- The production registry is signed, empty, and makes `ECON_INTERVENE` unreachable.
- Both provider planners use pinned local tokenizers and independent scenario code.
- No provider network call occurs before the single dispatch.
- All cache fields and protected bytes/states are unchanged.
- Unknown residency is full-priced symmetrically; other unknown semantics are indeterminate.
- Every cost component has a finite authoritative bound or the decision is indeterminate.
- Exact-length transports retain one immutable selected buffer across ordinary retries.
- Fault injection proves retries cannot substitute an alternate representation and restore/resend is absent.
- Cross-tenant tests reject classifier, handle, proof, registry, and kill-switch mismatches.
- Source/link scans prove zero mutable module-global provider/account/cohort handles and zero remote
  token-count symbols reachable from the production dispatch fence. The sole exception file names a
  separately linked offline-test binary and its exact remote-count symbols; it is signed and reviewed
  per release, and production artifacts may match none of its entries.
- A non-empty registry remains blocked until cohort and kill-switch generations participate in an
  atomic wire-commit revalidation.
- Compile-time assertions pin scenario counts, tokenizer guard bands, and every finite margin formula.
- No live shadow mode, savings delta, task/session aggregate, or remote token-count path exists.
- Schema/source scans reject every baseline-minus-candidate delta or percentage-reduction field,
  regardless of name.
- Off mode bypasses the economizer dispatch fence and matches the frozen pre-economizer provider bytes.
- Model, tokenizer, contract, or serializer changes require regenerated, reviewed off-mode goldens.
- Build, unit, integration, sanitizer, and `git diff --check` tests pass.

## Deferred work

Every concrete lossless transform, new-output provenance issuer, live registry update, and bounded
rollout is deferred to a separate reviewed artifact. Lossy summarization, truncation, folding,
retrieval, recall, rehydration, rescue, and complete-task savings claims remain outside v2.

## Review record

Repeated implementation review removed live shadow behavior, savings claims, remote live token
counting, generic accounting, and restore/resend. The final review found the deliverable aligned with
the original request and all normative pillars, with zero issues.
