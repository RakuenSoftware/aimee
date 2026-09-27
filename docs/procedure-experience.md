# Procedure experience and task cost

MR-15 deploys attribution in observe mode. Search result count means availability
or truncation; it cannot close a correctness reward or update a posterior. The
KB retrieval-limit and fusion decision points retain their previous artifacts,
but their `observe` status prevents sampling count-trained weights. Go memory
also stops dynamic sampling of those weights; explicit operator retrieval limits
remain available.

## Receipt-bound feedback

Inspect `aimee memory receipt REQUEST_ID --json`. A receipt's
`procedure_exposures` lists only versioned reviewed procedures in its final
retained source manifest. Provider acknowledgement establishes `delivered`;
preparation alone remains `retrieved`. Neither proves application.

An authenticated operator can submit explicit feedback:

```sh
aimee learning application REQUEST_ID --event-json "$EVENT_JSON" --json
```

HTTP clients use `POST /v1/learning/application` with `request_id` and an `event`
object. The event follows the learning owner's
[`ProcedureEvent`](../server-go/modules/learning/procedure_experience.go) contract:

- `schema_version: 1`, immutable `event_id`, `task_id`, `attempt_id`, and `trial_id`.
- Exact `procedure` owner/ID/revision and `receipt_ref` from the exposure.
- `authority: user_feedback`; the KB replaces `actor` with the verified caller.
- Environment, model, task class, applicability gaps, action references and an
  RFC3339 `observed_at` timestamp. An optional `reported_latency_ms` is a decimal
  integer string; unknown latency is not zero.
- State: `retrieved`, `delivered`, `selected_for_use`, `applied`,
  `verified_success`, `verified_failure`, `abandoned`, or `outcome_unknown`.
- Verified outcomes require a verifier and evidence reference. Bound tests also
  require `verified_revision`. `explicit_user_evaluation` remains attributed user
  feedback, not independently verified test execution.

The Server reads its existing principal-owned, chain-checked ledger. Public
arguments cannot supply exposure, ledger rows or chain-integrity assertions.
Only its authenticated mTLS transport can forward the internal admission
request to KB. The learning process checks the separate caller context and exact
exposure. Missing receipts, omitted versions, model application/success claims,
and conflicting immutable event replacements fail closed.

There is no automatic host-execution attribution adapter in this rollout.
Generic tool success does not prove that a textual procedure was followed. The
reserved host-execution contract requires a host verifier binding; no public
route can select that authority. No model-facing tool automatically converts a
self-report into user feedback.

## Experience and retention

Governed events reuse `learning_application_events`; receipt-backed rows are
separate from legacy mining signals. Legacy rows are not verified experience.
The learning owner creates projections under a cohort transaction lock. Go
memory exposes the owner projection with reviewed procedure views, without
importing learning or economizer modules.

A trial is `(actor, task_id, attempt_id, trial_id)` with immutable procedure,
receipt and cohort bindings. Repeated events do not create trials. Separate
attempts remain separate; `terminal_task` is an explicit terminal observation,
not inferred from the latest successful retry. Success and failure evidence for
one trial are conflicting. Conflicting terminal observations are also explicit.

Context rendering bounds experience to four cohorts/references and 2 KiB,
with explicit partial coverage; an oversized history cannot crowd out procedure
text. Full projections remain in the learning ledger and admission response.

Cohorts preserve procedure versions and separate environment, model and task
class. They include unknown/abandoned/conflicting counts, counterexamples,
applicability gaps, last verified success, evidence/cost references, reported
trial latency, and Wilson intervals with verified sample counts. Associations
are not causal claims; correlated trials remain possible. Unresolved cost
references and missing latency remain unknown. Canonical instructions are never
rewritten by an experience projection. The rendered experience revision is bound
into source revalidation; governed writes and erasures participate in the final
provider-send barrier, including when canonical procedure text is unchanged.

Deleting governed evidence invalidates associated cached projections under the
same cohort lock. Old versions remain historical cohorts; they are not merged
into the current procedure's experience. Projection reads are principal-bound.

## Complete-task cost declarations

```sh
aimee learning task-cost --cost-json "$COST_JSON" --json
```

HTTP uses `POST /v1/learning/task_cost` with a `cost` object. Economizer owns
[`TaskCostRequest`](../server-go/modules/economizer/task_cost.go) and
[`TaskCostPairRequest`](../server-go/modules/economizer/task_cost_pair.go).
Amounts are USD nanodollars encoded as decimal strings, never JSON floating-point
amounts. Actual, estimated and unpriced usage remain separate.

Declare all seven stages: indexing/embedding, retrieval/reranking, context
transformation, generation, tools, retries, and verification. Each stage is
`observed`, `unknown`, or `not_applicable:<reason>`. An observed stage requires an
entry. Unpriced calls are retained by ID, not silently omitted. A generation
call during a retry is counted once under generation with its retry attempt ID;
the retry stage contains separate retry overhead only.

Entries bind a unique charge identity and attempt to provider/model, pricing
snapshot, cache assumptions, evidence, latency, marginal/amortized allocation
and its rule. Identical repeats are idempotent; conflicting repeats are refused.
An actual zero charge still needs its invoice/pricing evidence. Unpriced calls
cannot masquerade as zero. Estimated usage may include integer `estimated_units`
with rates per million units: uncached/cached input, output, embedding, tool
invocations, or wall milliseconds. Economizer sums exact products and rounds up
once per call. A supplied amount that contradicts those rates is rejected.
Precomputed estimates are explicitly labeled `caller_declared`.

Paired reports require both arms for every task in a frozen corpus of task and
verifier hashes. The corpus hash covers the task-ID-sorted JSON array. Missing,
duplicate, substituted or changed tasks are refused. Reports retain per-arm
cost components, unknown costs, completion and declared quality regressions.
They expose known cost differences, not claimed savings.

Caller declarations are not independently verified population completeness or
quality. These reports remain `observe_only_unqualified`; paired reports require
independent quality and coverage verification before release qualification.
They cannot construct an economizer intervention proof, fit routing weights, or
weaken exact-token/exact-request admission. MR-18 owns frozen release evidence
and coverage qualification. No fitted policy is promoted by this rollout.
