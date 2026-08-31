# Provider-specific, proof-gated economizer safety specification

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

> **Archived complete (2026-07-26).** The scoped implementation and safety gates are
> present on `testing` with dedicated tests.

- **State:** DONE. Delivered scope archived 2026-07-26.
- **Review status:** CONVERGED (`converged=true`, zero issues)
- **Version:** `aimee-economizer-safety-v2`
- **Date:** 2026-07-22
- **Scope:** OpenAI GPT-5.6-family and Anthropic Claude request paths

## Decision

Aimee may economize only through a provider-specific planner that proves the candidate request has a
strictly lower cost than sending the untouched request. If the proof cannot be completed from facts
known before dispatch, Aimee passes the request through byte-for-byte.

This is **proof-gated**, not off-only. The previous off-only draft incorrectly treated the inability
to guarantee lower dollars per completed task for every lossy transform as proof that no request can
be made cheaper. Several closed cases have a mechanically provable call-level saving:

1. reducing large, newly produced tool output before it has ever been sent or cached;
2. reducing only the mutable suffix after a client-supplied or provider-defined explicit breakpoint;
3. emitting a deterministic representation so repeated candidates remain byte-identical; and
4. reducing a request below a provider's full-request long-context pricing threshold.

The system distinguishes two claims:

- **provider-call saving:** the dispatched candidate has a lower conservative charge bound than the
  untouched request for this call; and
- **completed-task saving:** the entire task, including any follow-up, recall, retry, and output
  effects, cost less.

V2 authorizes only the first claim. It never labels a call-level estimate as completed-task savings.
Lossy transforms remain disabled until a separately reviewed outcome and lifecycle policy exists.

### Changes from the off-only v1 draft

V1 considered generic summarization, history folding, truncation, retrieval handles, recall,
rehydration, and rescue, then rejected all live behavior because those lossy paths could not prove
complete-task cost or outcome equivalence. V2 keeps every one of those paths disabled. Its only
change is to admit closed, lossless transformations whose individual provider-call inequality can be
proved without relying on later behavior.

## Governing invariant

For an intervention to be authorized:

```text
candidate_call_cost_upper_bound + safety_margin
    < baseline_call_cost_lower_bound
```

Both values are computed by the planner for the exact provider, model snapshot or documented model
family, endpoint, cache mode, breakpoint layout, account-visible pricing configuration, and tokenizer
version. Unknown facts make the proof `INDETERMINATE`, which means pass-through.

For every unknown cache or pricing axis, the planner enumerates one identical, exhaustive set of
mutually exclusive scenarios for baseline and candidate. The inequality must hold in every scenario;
otherwise the result is `INDETERMINATE`. It is invalid to select one cache scenario for the baseline
and another for the candidate. Both use the same pinned model snapshot and tokenizer identifier.

The proof does not depend on a future cache hit, future reuse count, eviction time, provider-internal
cache residency, or a post-response usage field. A future hit may improve the result but is never
needed to authorize it.

## Closed live-intervention classes

### A. New tool output before first dispatch

A tool result may be transformed only when a task-local provenance capability proves it was produced
by Aimee in the current turn and has never crossed the provider boundary in this task. The monotonic
capability is consumed on serialization and invalidated by persistence, import, replay, recall,
rehydration, process transfer, or ambiguous ownership. A missing or invalid capability is
`INDETERMINATE`. It binds authenticated tenant, task, call, source-content digest, transform ID, and
transform version and is consumed exactly once at dispatch selection. The transform must be
semantically lossless under a closed, tested contract whose
equivalence is mechanical and does not require the model to execute, infer, or follow a decoder.

The planner first fully serializes both alternatives without sending either. Token buckets and costs
are computed from those final canonical provider-bound bytes, including delimiters, metadata, schema,
and every framing byte. General summaries, omission, ranking, truncation, retrieval handles, and
rehydration are not in this class.

The initial live adapter additionally requires an explicit-only cache layout: OpenAI implicit caching
must be disabled, and neither provider may have a cache marker or documented implicit boundary on or
after the mutable content. Unknown cache residency is priced as full input for both alternatives.
Every preceding cache-sensitive byte and cache state must remain identical.

### B. Suffix-only reduction after an explicit breakpoint

The planner may transform content strictly after the furthest applicable explicit breakpoint whose
semantics are documented for the provider and whose location is explicit in the client request. For
requests with multiple breakpoints, every prefix ending at every marker is protected and no
transformation may occur before a later marker. The protected region is the exact
canonical provider-bound byte sequence from request start through the breakpoint and all framing
that terminates it, including whitespace, delimiters, adjacent cache markers, keys, mode indicators,
TTLs, ordered tools, images, and schemas. Every protected byte must remain identical.

The economizer may not infer a hidden breakpoint, move or add a breakpoint, turn implicit caching on
or off, or claim a cache hit. Within each exhaustive shared scenario, baseline and candidate use the
identical cache state for the protected prefix; the suffix uses the provider bucket defined by that
same scenario. If the candidate does not win in every scenario, the request passes through.

The initial live adapter requires explicit-only caching and no explicit or documented implicit
boundary within the mutable suffix. Candidate and baseline must satisfy every documented cacheability
condition and have identical read/write/miss state at every protected breakpoint in every scenario.
Absent authoritative residency, both protected prefixes are priced as full input.

### C. Deterministic transforms

Every enabled transform must be deterministic and versioned: identical provider-bound input and
configuration produce identical bytes. Determinism protects future exact-prefix matching and makes
golden tests possible. It is necessary but not sufficient; the cost inequality and an approved
semantic contract must also pass.

The live transform allowlist is empty in this specification. Each concrete transform requires its own
contract version, provider/model/tokenizer compatibility matrix, property tests, and converged review
before it is added. A class description by itself grants no live authority.

### D. Long-context threshold avoidance

When authoritative local tokenization proves the untouched request is above a documented full-request
pricing boundary and the candidate is below it, the planner applies each tier's price to the complete
request exactly as the provider documents. It includes input, cached input, cache writes, and a
conservative output allowance under all applicable pricing, including threshold changes.

No intervention is authorized when either token count falls within the tokenizer uncertainty band,
the model alias can route to incompatible pricing/tokenization, or account/platform modifiers are
unknown. Baseline and candidate use the identical pinned snapshot and exact provider-side tokenizer
version, and the resolved snapshot identifier is bound into the proof. Threshold avoidance requires
a single pinned snapshot; aliases are `INDETERMINATE` even when documented routable snapshots appear
compatible.

## Provider separation

There is no provider-neutral cost formula. A shared dispatcher enforces common safety states, but
delegates all accounting and cache semantics to one of two independent planners:

```text
request -> classify provider/model/API
        -> OpenAI GPT-5.6 planner OR Anthropic Claude planner
        -> PASS_THROUGH | INTERVENE(proof) | INDETERMINATE
```

### OpenAI GPT-5.6 planner

The OpenAI planner models:

- exact-prefix cache matching;
- `prompt_cache_key`, implicit/explicit mode, and explicit breakpoint placement;
- ordinary input, cached input, and `cache_write_tokens` at the current documented rates;
- the GPT-5.6-family 1.25x cache-write multiplier;
- the documented greater-than-272K full-request input and output multipliers; and
- every documented input, output, cached-input, cache-write, batch, and service-tier boundary; and
- Responses and Chat Completions token accounting separately.

It never treats `prompt_cache_key` as a readable cache handle. Returned `cached_tokens` and
`cache_write_tokens` are settlement evidence for the request actually sent, not pre-dispatch proof of
the counterfactual. With explicit mode and no explicit breakpoint, it models no cache write under the
versioned contract pinned to the OpenAI Prompt Caching guide's **Prompt cache breakpoints** section.
Every proof binds and validates the active local contract digest and generation. A changed, expired,
revoked, or unavailable contract makes the proof `INDETERMINATE`.

### Anthropic Claude planner

The Anthropic planner models:

- exact-prefix matching at `cache_control` boundaries;
- uncached input, cache reads, and cache creation separately;
- five-minute and one-hour cache-write prices and refresh behavior;
- model-specific long-context thresholds and stacked pricing modifiers; and
- Messages API and server-tool token accounting.

It never moves or synthesizes `cache_control` markers. Response usage buckets settle only the sent
request and cannot prove what the untouched alternative would have received. Every byte through each
marker, including the immediately preceding boundary byte, encoding, Unicode normalization form,
whitespace, and segment length, is protected from direct or indirect change.

## Cost proof construction

Each planner produces an immutable proof record before mutation:

```text
tenant_id, task_id, call_id, provider, endpoint, resolved_model_snapshot, tokenizer_id
pricing_table_id, pricing_generation, contract_version_ids_by_rule
baseline_token_buckets_lower_bound
candidate_token_buckets_upper_bound
candidate_cost_upper_bound_in_price_units
baseline_cost_lower_bound_in_price_units
safety_margin_in_price_units
new_output_provenance_capability
transform_id, transform_version, semantic_contract_id
decision, reason_code
```

Both alternatives are fully serialized before counting. For each shared scenario, the baseline bound
uses the cheapest settlement within that scenario and the candidate bound uses the most expensive
settlement within the same scenario. The global baseline
lower bound is the minimum baseline value across scenarios; the global candidate upper bound is the
maximum candidate value across scenarios. Both lower and upper polarities are computed for each side
and checked for consistency. Intervention requires a strict win in every scenario after a margin no
smaller than the greatest of price quantization, token uncertainty priced at the scenario's maximum
rate, and the configured percentage of baseline cost. All cost bounds and the margin use one checked
fixed-point unit: the account billing currency at the signed pricing table's declared precision.

Every provider-billed component is included for every intervention, including output. The candidate
output upper bound uses the preserved client output limit at the most expensive applicable output
settlement, while the baseline output lower bound is zero unless a stronger authoritative minimum is
available. If input/cache savings cannot exceed that complete conservative output difference and the
margin, the result is `INDETERMINATE`.
The upper bound must be finite and derived only from a preserved client limit or documented hard
model/API maximum. Streaming, server tools, or an unknown maximum make the proof `INDETERMINATE`.
Only a documented hard minimum may raise the baseline output lower bound above zero.

Provider pricing is a signed, versioned configuration with an expiry and revocation generation. The
proof binds to a table current immediately before the single dispatch. Candidate selection and
dispatch commit have one linearization point at the actual first outbound wire write: a generation-
scoped lease atomically validates pricing, contract, tokenizer, tenant, and kill-switch generations
while selecting the fully serialized buffer and issuing that first write.
Invalidation before that commit selects the pristine request. Invalidation after commit cannot create
a second dispatch. Proofs are never reused across calls or sessions. Public list prices may be used only when the
operator explicitly confirms they are the account's applicable prices. Otherwise the planner is
disabled for that account. Credits and volume commitments are excluded unless exposed as a stable
marginal price suitable for per-request comparison.

## Semantic safety

V2 enables only transforms with a lossless, mechanically testable inverse or equivalence rule.
Byte-for-byte identity is not required for the mutable content, but the represented information must
be identical under the transform's declared contract. Fuzzing, round-trip tests, duplicate-key tests,
Unicode tests, numeric edge cases, ordering tests, and maximum-size tests are release gates.
No transform in any class may require the model to execute, infer, or follow a decoder. Compatibility
and property tests are keyed by provider, pinned model snapshot, and exact tokenizer version.

If a format's semantics depend on whitespace, ordering, duplicate keys, comments, source locations,
or exact bytes, a transform that changes those properties is not lossless and is denied.

Summarization, truncation, selective omission, embeddings, external retrieval, recall, and
rehydration are disabled. They may save an individual call, but complete-task cost and outcome cannot
currently be bounded without a different policy.

## Runtime invariants

1. The pristine request remains available until the single dispatch decision.
2. Exactly one provider dispatch is permitted per outbound request. Classification, pricing,
   contracts, tokenization, and planning are local-only and make zero provider preflight, dry-run,
   cache-probe, or discovery calls. The economizer builds at most one candidate and never sends both.
3. No byte may leave the process until the proof is final, both alternatives are fully serialized,
   and the atomic dispatch commit selects one buffer. A failed transform, proof, serialization, or invariant check falls back before dispatch to the
   untouched request. Once either representation is dispatched, the economizer cannot retry,
   restore, resend, or dispatch another representation.
4. An in-memory constant-time byte comparison must match baseline and candidate across the entire
   protected canonical provider-bound region. Prefix digests are not persisted or logged.
5. Client cache intent passes through exactly; no key, marker, breakpoint, mode, or TTL is invented.
6. Unknown providers, aliases, snapshots, endpoints, pricing, tokenizers, fields, or modifiers pass
   through unchanged.
7. Streaming does not weaken pre-dispatch proof requirements.
8. Tenant identity comes only from authenticated dispatcher context and is required before planner
   selection. Planner contexts, pricing handles, contract registries, tokenizer state, provenance,
   decisions, and proof records are tenant- and call-scoped and assert tenant again at wire commit.
   Module-global mutable handles and shared proof caches are forbidden. Proofs are affine in-memory
   objects tied to the two serialized buffer objects and cannot be looked up or reused. Records contain
   no prompt content or prompt-derived digest.
9. Statistics distinguish predicted call savings from provider-settled deltas and never report
   completed-task savings. No task-, conversation-, or session-denominated aggregate is emitted under
   any name. Every emitted field containing `savings` also contains `predicted_call`.
10. A tenant-scoped kill switch atomically selects byte-identical pass-through without changing
    client cache intent; a global switch independently disables all tenants.

## Acceptance tests

- Golden byte tests prove protected-prefix and cache-field identity for both providers and APIs.
- Exact tokenizer fixtures straddle every pricing threshold by at least the configured uncertainty
  margin; aliases and tokenizer drift fail closed.
- Price-table fixtures cover ordinary input, reads, every write TTL, output multipliers, batch/service
  modifiers, and unknown account pricing.
- Adversarial tests enumerate identical scenarios for both alternatives and prove any non-win,
  inconsistent polarity, or favorable/unfavorable scenario split passes through.
- Bounds hard-fail to `INDETERMINATE` unless `baseline_lower <= baseline_upper` and
  `candidate_lower <= candidate_upper` in every scenario; authorization always compares baseline
  lower with candidate upper.
- New-tool-output provenance tests reject previously dispatched, persisted, recalled, or ambiguous
  content. Capabilities bind tenant, task, call, source digest, transform ID, and transform version;
  one-shot and cross-transform reuse tests reject every mismatch.
- Lossless transform property tests prove round trips across arbitrary valid inputs and reject
  semantic edge cases.
- Cache tests prove no breakpoint/key/mode/TTL synthesis or movement and no prefix mutation.
- Retry/error tests prove at most one candidate construction and exactly one provider dispatch per
  outbound request under every planner, serializer, transport, and response failure.
- Preflight tests prove zero provider network calls occur between ingress and the one wire dispatch.
- Settlement tests reconcile returned usage without retroactively changing the authorization proof.
- Cross-tenant tests use identical requests with different prices and prove tenant A cannot read or
  apply tenant B's proof, price handle, provenance, state, or kill switch. Tests enter through the
  classifier and assert the selected planner, proof tenant, snapshot, and price handle all belong to
  the requesting tenant.
- Documentation-contract fixtures pin dated URLs, section anchors, and content digests. A changed or
  unavailable contract disables its rule on the next proof; documentation is never fetched on-path.
  Every provider-dependent rule has a `contract_version_id` in the proof.
- Pricing freshness tests revoke a table between proof and dispatch and require pristine pass-through.
- Concurrency tests race pricing, contract, tokenizer, tenant policy, and kill-switch generation
  changes against dispatch and prove no candidate commits under a stale generation.
- Model fixtures reject aliases with any incompatible routable snapshot and require the same exact
  provider tokenizer version for baseline and candidate.
- Metrics/log/UI/schema tests reject task-level, completed-task, verified-savings, total-savings, or
  equivalent labels derived from call predictions. Cross-call aggregates, if exposed, are named
  `sum_of_minimum_predicted_call_savings_not_task_savings`.
- Schema tests reject every task/conversation/session aggregate, including sums of valid call
  predictions, regardless of label, and reject any unqualified `savings` string.
- Serialized-overhead fixtures prove token and cost bounds include all candidate framing.
- Output fixtures cover ordinary and threshold-multiplied rates and deny live mutation whenever a
  finite authoritative candidate output upper bound is unavailable or erases the strict win.
- Multi-breakpoint and Unicode-normalization fixtures reject any byte or length change through every
  protected Anthropic or OpenAI boundary.
- Explicit-cache fixtures deny implicit mode, a boundary in the mutable suffix, cacheability changes,
  or any protected-breakpoint state difference; unknown residency is full-priced on both sides.
- Each concrete transform is absent until its provider/model/tokenizer contract and review are pinned.
- Settlement reconciliation emits an incident and disables an adapter when returned usage exceeds
  its bound; it never retroactively changes the completed call's authorization or claim.
- Integration tests cover OpenAI Responses/Chat Completions and Anthropic Messages, sync/streaming,
  tools, images, structured output, errors, and context boundaries.

## Explicit non-claims

V2 does not claim that every request is cheaper, that fewer tokens always cost less, that a cache hit
will occur, or that call-level savings imply lower total task cost. Its guarantee is narrower and
testable: whenever it mutates a request, the candidate's conservative provider-call cost bound is
strictly below the untouched request's optimistic bound under the configured authoritative pricing
contract. Otherwise it does nothing.

## Sources checked 2026-07-22

- https://developers.openai.com/api/docs/guides/prompt-caching
- https://developers.openai.com/api/docs/models/gpt-5.6-sol
- https://platform.claude.com/docs/en/build-with-claude/prompt-caching
- https://platform.claude.com/docs/en/about-claude/pricing

## Review record

The safety review converged after four remediation passes. The final two-round review returned
`converged=true`, no concrete remaining path that violated the formal provider-call guarantee, and
`APPROVE`. The guarantee is deliberately limited to the single accepted dispatched call; it does not
claim future-cache, transport-failure-counterfactual, or completed-task savings.
