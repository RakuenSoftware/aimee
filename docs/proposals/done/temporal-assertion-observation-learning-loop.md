# Proposal: temporal assertion recall, evidence-backed observations, and a closed learning loop

- **State:** done — implemented and validated; default-off promotion remains a separate reviewed decision
- **Date:** 2026-08-24
- **Charter roles:** Extract / Recall / Rank-Fuse / Observe / Learn / Gate-Promote /
  Evaluate / Constrain-Verify
- **Owns:** the integration contract from episode evidence through temporal assertion
  retrieval, derived observations, reviewed procedural learning, context assembly, and
  outcome evaluation
- **Depends on:** the canonical assertion mutation gate, evidence provenance policy,
  memory/learning ownership boundaries, the retrieval context contract, and the benchmark
  artifact/cadence contracts

## Implementation status

The full default-off implementation was completed and validated on 2026-08-24:

- the result and dual-axis temporal contracts are represented by a distinct semantic assertion
  search surface with typed invalid-time and degraded-channel responses;
- model-extracted claims require exact, bounded byte spans and hashes of the supporting region;
- hybrid lexical/vector recall uses late rank fusion, a vector similarity floor, bounded
  temporally filtered traversal, scope filtering at every hop, and per-result retrieval traces;
- recurring failures and successful recoveries materialize deterministic, evidence-linked
  observations before any learning signal or proposal is emitted;
- procedural changes remain behind the existing review and promotion gate, with applicability,
  expiry, evidence, and rollback metadata;
- retrieval, rendering, selection, application, and outcome attribution are persisted atomically,
  idempotently, and with stable references and cost/latency/turn/tool metrics;
- typed context assembly exposes independent current assertions, historical assertions, episodes,
  summaries, observations, reviewed procedures, and recent working context with channel budgets,
  packing decisions, watermarks, sufficiency, and explicit trust boundaries;
- retrieval sufficiency is scored separately from answer correctness in benchmark artifacts; and
- schema mirrors, version fencing, temporal/evidence goldens, observation refresh tests, and
  compatibility checks cover the implementation.

Semantic recall, observations, and the typed assembler remain opt-in and do not alter normal
prompt assembly. Production activation is intentionally outside this implementation: it requires
review of representative benchmark evidence and is never inferred from code completion.

Implementation and deployment evidence is recorded in the
[validation report](../../validation/temporal-assertion-learning-loop.md).

## Decision

Connect the existing canonical assertion store to primary ranked recall; require every
derived claim to identify exact source evidence; add a read-only observation layer that
detects recurring failures, recoveries, commitments, and state transitions across episodes;
route observations into the existing reviewed learning pipeline; and measure retrieval
sufficiency independently from answer correctness.

This is an integration proposal, not a replacement memory architecture. The system already
has the stronger integrity primitives: authority-ranked corrections, bitemporal assertion
history, independently auditable evidence mentions, reversible graph commits, candidate
quarantine, reviewed promotion, scoped recall, and benchmark provenance. The missing piece is
that these primitives do not yet form one closed, measurable loop.

The target loop is:

```text
raw episode or task attempt
    -> exact evidence spans
    -> canonical assertions and outcomes
    -> evidence-backed observation
    -> reviewed procedural proposal
    -> scoped retrieval and application
    -> attributed outcome
    -> corroborate, revise, retire, or reject
```

No model-generated interpretation becomes canonical truth or an executable instruction merely
because it was generated repeatedly. Evidence supports an observation; an observation may
justify a proposal; only the existing authority and promotion policies may make that proposal
durable or actionable.

## Thesis

The memory substrate currently contains more truth and provenance than the primary retrieval
path exposes. At the same time, failure learning records recurrence counts without preserving a
first-class, queryable interpretation of the pattern and its exact evidence. Consequently:

1. point-in-time facts are available through a narrow lexical relation search rather than the
   measured hybrid recall path;
2. canonical semantic assertions are physically present but deliberately excluded from generic
   co-occurrence graph readers;
3. model-extracted claims can cite an entire memory rather than the exact region that supports
   each claim;
4. recurring failures can create a workflow proposal without first producing an evidence-linked,
   revisable observation; and
5. answer accuracy can hide whether the retriever supplied sufficient context or the response
   model succeeded by guessing.

Closing these gaps provides a safer recursive-learning loop: the system can recognize that a
strategy repeatedly fails, identify the conditions and recovery sequence, propose a bounded
procedure, and later determine whether using that procedure actually reduced recurrence.

## §0 Existing substrate and reuse boundary

The following surfaces already exist and remain authoritative. Implementations under this
proposal must extend or compose them, not create parallel truth stores or bypass their gates.

| Capability | Existing surface | Reuse decision |
| --- | --- | --- |
| Canonical semantic assertion | `entity_edges` rows with `edge_class='semantic'` | Reuse as the only canonical proposition store. |
| Valid-time history | `valid_from` / `valid_until` | Reuse as the world-time axis. |
| Transaction-time history | `asserted_at` / `superseded_at`, plus invalidation and suppression | Reuse as the belief-time axis. |
| Authority and lifecycle | `authority_rank`, `actor_principal`, confidence class, candidate/persistent/promoted/superseded/invalidated states | Reuse without weakening. |
| Evidence mentions | `fact_evidence` with source kind/id/span/hash, actor, observation time, ingest run, commit, and stance | Reuse as the assertion evidence ledger. |
| Reversible mutation | `fact_graph_commits`, structured change rows, ingest preview/rollback, and erasure reports | Reuse for every assertion-affecting write. |
| Current typed-fact lookup | `db2_typed_fact_recall` and relation lookup | Preserve as compatibility lookup; do not treat it as ranked recall. |
| Valid-time relation lookup | `db2_memory_relations_search_as_of` | Preserve as compatibility lookup; supersede its role with a dual-axis semantic retriever. |
| Co-occurrence graph fusion | generic `entity_edges` readers restricted to `edge_class <> 'semantic'` | Preserve separation; semantic facts enter through a distinct channel and fuse late. |
| Episode and relation records | `memory_episodes`, `memory_relations`, reference time, valid/invalid times | Reuse as episodic source material and compatibility graph data. |
| Failure event substrate | delegate outcomes, interaction events, failure modes, embeddings, cluster keys, and session identity | Reuse as observation inputs. |
| Recurrence miner | grouped failure recurrence with a high-water mark and pending-proposal deduplication | Refine into observation production; retain high-water mark, scheduler, and dedup behavior. |
| Reviewed learning | signals -> proposals -> review/Gate-Promote -> typed sink | Reuse as the only route from an observation to a durable procedure or rule. |
| Benchmark provenance | target/model/config/data/harness hashes, seed, environment, tokens, citations, latency, and cost | Extend rather than replace. |

### Ownership and overlap

- The memory/learning boundary contract continues to own which module may store, retrieve,
  observe, propose, or apply.
- The evidence provenance-tier contract continues to decide which evidence may anchor an answer
  and how untrusted data is isolated.
- The binding retrieval context contract continues to own exploration budgets and enforcement.
  This proposal owns the content and temporal semantics of the retrieved channels, not tool-use
  caps.
- The benchmark cadence proposal continues to own scheduling and artifact retention. This
  proposal owns the new metrics, cases, and promotion gates.
- The canonical assertion mutation gate remains the only authority for assertion replacement,
  supersession, invalidation, correction, and evidence attachment.

## §1 Canonical temporal query semantics

### §1.1 Two independent time axes

Every semantic retrieval request may specify:

- `valid_at`: the time in the represented world for which a fact must be true;
- `believed_at`: the time at which the system must have held the assertion as current; or
- both, to ask what the system believed at one time about what was true at another time.

These axes are not aliases. For a fact learned today about an event last month, `valid_at` and
`believed_at` intentionally differ.

For an explicit `(valid_at=V, believed_at=B)` query, an assertion is eligible only when:

```text
asserted_at <= B
AND (superseded_at is empty OR B < superseded_at)
AND (invalidated_at is empty OR B < invalidated_at)
AND suppressed = false
AND valid_from <= V, when valid_from is present
AND (valid_until is empty OR V < valid_until)
```

The transaction-time comparison uses the assertion version that existed at `B`; it does not
consult only the current row or current-view projection. Candidate assertions remain ineligible
unless the caller explicitly requests a review surface and is authorized for it.

### §1.2 Safe defaults

When the caller supplies neither time:

- `believed_at` defaults to now;
- world facts default to currently valid;
- experience and observation records without a world-validity interval remain eligible by their
  typed lifecycle rules;
- superseded, invalidated, suppressed, expired, candidate, and quarantined assertions are excluded;
  and
- historical results are never silently mixed into a current answer.

An explicit `include_historical=true` may widen recall, but every historical item must be labeled
with both time axes and may not be rendered as a current fact.

Invalid timestamps, inverted intervals, unsupported precision, or an impossible combination fail
with a typed request error. They never fall back to an unfiltered search.

### §1.3 A distinct semantic retrieval channel

Do not remove the `edge_class <> 'semantic'` filters from generic graph readers. Co-occurrence
edges and canonical propositions have different uniqueness, correction, weight, authority, and
lifecycle semantics. Mixing them before candidate normalization would make graph weight look like
truth confidence and could reintroduce invalid facts through traversal.

Add a semantic assertion channel that:

1. resolves lexical candidates over subject, relation, target, and fact text;
2. resolves vector candidates over a canonical assertion rendering;
3. optionally expands from matched entities through bounded, temporally filtered semantic hops;
4. applies scope, authorization, provenance-tier, authority, lifecycle, `valid_at`, and
   `believed_at` filters before ranking and at every traversal hop;
5. deduplicates by canonical proposition/version identity;
6. attaches the top supporting and contradicting evidence mentions; and
7. enters the shared fusion and packing contract as `source=semantic_assertion`.

Late fusion keeps channel scores interpretable. The first implementation should use an existing
measured fusion method; it must not add a new reranker or model merely because the channel is new.

### §1.4 Retrieval result contract

Each semantic result carries at minimum:

```text
assertion_id
version
subject / relation / object
assertion_kind
lifecycle_state
authority_rank
confidence_class / confidence
valid_from / valid_until
asserted_at / superseded_at
historical: true|false
support_count / contradiction_count
evidence[] { source_kind, source_id, source_span, observed_at, stance }
retrieval[] { channel, raw_score, fused_score, rank }
inclusion_reason
```

The prompt renderer may shorten evidence content, but the structured response and explain path
must retain the stable evidence locators.

## §2 Exact episode and source attribution

### §2.1 Extraction contract

Every extracted entity, claim, outcome, and temporal expression must identify the exact input
episode indices and source spans from which it was derived. For batch extraction, the response
schema includes a bounded list of zero-based episode indices per item. Each index is validated
against the immutable batch manifest and mapped to a durable episode/source identifier.

An extracted assertion without at least one valid source locator is not committed as persistent
or promoted knowledge. It may be:

- retried with a stricter extraction prompt;
- stored as a non-recallable extraction failure for diagnostics; or
- quarantined as a candidate when policy explicitly permits unsupported candidates.

There is no fallback that attributes a claim to every episode in the batch or to the entire source
memory merely because precise attribution failed.

### §2.2 Span integrity

For every evidence mention:

- source identifiers are durable and scope-qualified;
- byte/line/page spans are within source bounds;
- the evidence hash covers the normalized source region, not only the whole document;
- observation time comes from the source record or a typed temporal extraction with confidence;
- stance is explicitly `supports` or `contradicts`;
- actor identity is captured from verified ingress context and cannot be supplied by model output;
  and
- batch/commit identity groups the write for preview, rollback, and erasure.

When raw content is subject to retention or erasure, the evidence row keeps only the locator/hash
fields permitted by policy. A hash is an integrity aid, not permission to retain sensitive text.

### §2.3 Duplicate and correction behavior

- An exact or semantically resolved duplicate adds a new independent evidence mention to the
  canonical assertion; it does not create a parallel fact.
- A contradiction adds contradicting evidence and enters the authority-aware mutation policy.
- Lower-authority evidence cannot silently replace a higher-authority assertion.
- Late-arriving older evidence must not invalidate a newer valid interval.
- Reprocessing the same source span is idempotent.

## §3 Evidence-backed observations

### §3.1 Definition

An observation is a read-only, derived interpretation of multiple evidence-bearing records. It
captures a durable pattern that is not adequately represented by one fact, such as:

- recurring failure or recovery sequences;
- a strategy that succeeds only under particular preconditions;
- a repeated missing prerequisite;
- a stable preference or working style;
- a commitment spanning multiple episodes;
- a state transition involving several entities; or
- a repeated relationship/path pattern worth reviewing.

An observation is not a canonical world fact, a user instruction, a procedure, or an authorization
grant. It follows its evidence and can be regenerated or retired.

### §3.2 Required observation fields

The logical observation contract contains:

```text
observation_id
scope_kind / scope_id
observation_type
title / summary
status: candidate | active | retired | rejected
confidence
evidence_window_start / evidence_window_end
created_at / refreshed_at / retired_at
synthesis_policy_version
evidence_count / independent_session_count
evidence[] { kind, stable_id, span, stance, observed_at }
supersedes / superseded_by
```

The storage implementation may use the existing artifact/link substrate if it can enforce all of
these fields, stable evidence references, lifecycle transitions, and query semantics. A new table
is justified only if the current substrate cannot express those invariants without serialized
opaque JSON becoming the source of truth.

### §3.3 Failure and recovery observations

The existing recurrence miner groups exact failure labels. Extend it to consider bounded,
deterministic features from complete attempts:

- role and task family;
- ordered action/tool sequence;
- failure class and exact error signature;
- environment and preconditions;
- files, symbols, services, or resources involved;
- whether the attempt was abandoned, retried, corrected, or recovered;
- recovery action and eventual outcome; and
- whether a recalled procedure was exposed, selected, and actually applied.

Frequency alone is insufficient. A candidate recurring-failure observation requires independent
episodes and sessions, evidence diversity, and a minimum precision gate on held-out labeled cases.
A candidate recovery observation must include both the failed sequence and the successful change;
otherwise it is merely a correlation.

Suggested initial types are:

- `recurring_failure`
- `failed_strategy`
- `successful_recovery`
- `missing_precondition`
- `tool_misuse`
- `environment_mismatch`
- `unstable_procedure`

Types are closed, validated identifiers with operator-controlled descriptions. Novel model-emitted
types remain generic candidates until reviewed through the existing ontology/evolution policy.

### §3.4 Refresh, contradiction, and retirement

Observations are incrementally refreshed when new eligible evidence arrives:

- supporting evidence updates confidence and the evidence window;
- contradictory outcomes are retained and may narrow the observation's preconditions;
- invalidated or erased evidence is removed from the effective evidence set;
- an observation below its support floor retires rather than remaining active on stale counts;
- a materially changed interpretation creates a new version linked through supersession; and
- exact recomputation over the same evidence and policy version is deterministic.

The summary is never the audit record. The evidence links, policy version, and lifecycle are the
audit record; the summary is a replaceable rendering.

### §3.5 Scope and authorization inheritance

An observation may be returned only when the caller is authorized to read every evidence item used
to synthesize the returned summary. Mixed-visibility evidence must either:

- produce separate observations per visibility partition; or
- make the combined observation visible only at the most restrictive common scope.

Derived metadata is deny-dominant: sensitivity, retention, tenant/workspace identity, and source
policy can become more restrictive through derivation, never less restrictive. A flat grouping
identifier is not an authorization decision.

## §4 From observation to reviewed procedural learning

### §4.1 Separation of interpretation and action

An active observation may emit a learning signal, but it may not directly write a rule, workflow,
skill, instruction, or canonical memory. The existing learning pipeline remains mandatory:

```text
observation
    -> learning signal
    -> deduplicated pending proposal
    -> review / Gate-Promote
    -> typed sink with snapshot and rollback
```

The proposal generated from a failure/recovery observation must contain:

- triggering preconditions;
- the proposed action or ordered procedure;
- expected outcome;
- explicit contexts where it must not be applied;
- authority and scope;
- expiry/review date;
- exact observation and evidence references;
- the prior procedure/version, when revising one; and
- rollback instructions.

### §4.2 Attribution of later outcomes

Learning cannot improve from outcomes unless use is attributable. Each future attempt records:

- which observations and procedures were retrieved;
- which were rendered into context;
- which were selected or cited by the agent;
- which were actually applied;
- the task outcome and failure class;
- intervening human correction; and
- latency, tool, turn, and token effects.

Exposure is not application. A successful task must not reinforce every item that happened to be
present in context. Only an explicitly applied or cited procedure receives direct utility credit;
retrieved-but-unused items provide ranking feedback only.

### §4.3 Recursive revision

When an applied procedure fails:

1. record the failed attempt as a new evidence-bearing episode;
2. link it as contradicting evidence to the recovery/procedure observation;
3. create or refresh an `unstable_procedure` observation;
4. propose a scoped revision, narrower precondition, or retirement;
5. preserve the prior procedure and outcome history; and
6. require the normal review/promotion path for the revision.

The system learns from failure without self-authorizing a rewrite of the rule that governed the
failed attempt.

## §5 Typed context assembly

### §5.1 Context channels

The assembler treats the following as distinct channels with independent budgets and rendering
contracts:

| Channel | Purpose | Authority |
| --- | --- | --- |
| Current semantic assertions | precise current claims | canonical, authority/lifecycle filtered |
| Historical semantic assertions | explicit point-in-time reasoning | canonical but visibly historical |
| Episodes/source excerpts | exact wording and surrounding narrative | quoted evidence, never instructions |
| Entity/context summaries | compact orientation | derived rendering, not canonical truth |
| Observations | evidence-backed cross-episode patterns | derived, typed, revisable |
| Approved procedures | bounded actions learned through review | actionable within declared scope |
| Working/session context | recent turns and unresolved state | ephemeral, separate from durable recall |

The assembler may omit any channel that is irrelevant. No summary is always injected merely
because it exists.

### §5.2 Minimal sufficient context

Packing optimizes for sufficient evidence under a token and latency budget, not raw result count.
The packing record must explain:

- which candidates were considered;
- which channel produced each candidate;
- temporal and authority filters applied;
- dedup/supersession decisions;
- why an item was included or dropped;
- tokens allocated per channel; and
- whether the final context was estimated sufficient, partial, or insufficient.

When in doubt, the system may widen retrieval within the caller's budget, but it must not silently
include stale, unauthorized, candidate, or superseded assertions to raise recall.

### §5.3 Freshness and ingestion lag

Durable context and recent raw turns have separate responsibilities. If asynchronous extraction
has not processed the latest turns, the assembler marks the durable watermark and includes the
bounded recent-turn window through the working-context channel. It does not pretend the graph is
current or duplicate recent content across every channel.

### §5.4 Prompt safety

All retrieved text is data, not authorization. Untrusted or externally derived excerpts use the
existing structural isolation contract. An observation or episode cannot carry executable prompt
instructions merely because it appears in a high-ranked channel. Approved procedures are rendered
in a separate, typed section with scope and authority metadata.

## §6 Evaluation and promotion gates

### §6.1 Separate retrieval sufficiency from answer correctness

The primary retrieval grade is whether the assembled context contains the information needed to
answer the case:

- `COMPLETE`: all necessary evidence is present;
- `PARTIAL`: relevant evidence is present but a required element is missing; or
- `INSUFFICIENT`: critical evidence is absent.

Answer correctness is secondary and scored separately. Reports must expose at least:

- complete context + correct answer;
- complete context + wrong answer;
- partial/insufficient context + correct answer; and
- partial/insufficient context + wrong answer.

This separates retrieval defects from composition defects and detects lucky guesses.

### §6.2 Minimality, safety, and temporal metrics

Each run records:

- retrieval sufficiency by category and scope;
- answer accuracy;
- relevant evidence recall and unsupported-context rate;
- stale/historical fact leakage into current answers;
- authority/lifecycle/scope violations (must remain zero);
- evidence citation validity;
- assembled and retrieved tokens;
- sufficient-context tokens and completeness per token;
- per-channel candidate/result counts and score distributions;
- retrieval, assembly, answer, and judge latency;
- cost where applicable; and
- errors, skips, retries, degraded paths, and sample counts.

High completeness obtained by indiscriminately overpacking is not a win. Promotion requires a
quality floor and a non-regression bound on context size/noise.

### §6.3 Learning-loop metrics

Failure-learning evaluation records:

- recurrence-detection precision and recall against labeled attempts;
- observation evidence precision;
- recovery/precondition extraction accuracy;
- proposal acceptance and rejection rates;
- recurrence rate before and after an applied procedure;
- negative transfer to unrelated task families;
- correction adherence;
- time/evidence required to retire a contradicted observation; and
- percentage of successful outcomes with attributable procedure use.

No claim of learned improvement is valid when procedure application was not recorded.

### §6.4 Reproducible run structure

Ingestion, extraction, indexing, retrieval, context assembly, answering, and grading are separate
stages with immutable manifests. A run records code/data/config/model/prompt/policy hashes,
concurrency, seed, environment, parent ingestion/index run, and raw result locations. Expensive
ingestion artifacts are reusable across retrieval configurations when their manifests match.

Long-running stages support bounded concurrency, retry with jitter, atomic checkpoints, resume,
and explicit incomplete status. An interrupted or empty run never reports a pass.

Automated judges are calibrated against a human-scored set. The default profile avoids using the
same model identity for both answering and grading; any exception is labeled and cannot alone
promote a production change.

### §6.5 Required temporal and contradiction matrix

The committed corpus includes at minimum:

1. a newer correction closes the old valid interval;
2. a late-arriving older episode does not invalidate a newer fact;
3. two duplicate claims add two evidence mentions but one canonical assertion;
4. lower-authority contradiction is quarantined and cannot replace higher authority;
5. `valid_at` and `believed_at` return different correct histories;
6. current recall excludes superseded, invalidated, suppressed, expired, and candidate facts;
7. historical recall labels non-current facts;
8. temporal filters apply to every graph hop;
9. out-of-order batch ingestion is deterministic;
10. evidence erasure changes or retires dependent observations;
11. unauthorized evidence cannot leak through an observation summary; and
12. a failed learned procedure creates contradicting evidence without self-authorized mutation.

## §7 Failure behavior and operational guarantees

- **Semantic retrieval unavailable:** return a typed degraded-channel status; other authorized
  channels may continue, but the response cannot claim semantic completeness.
- **Embedding unavailable:** use the declared lexical/graph degraded mode and record it; never
  compare the run to a full hybrid baseline as if equivalent.
- **Evidence attribution invalid:** do not persist the derived assertion as durable knowledge.
- **Observation synthesis unavailable:** canonical facts, episodes, and ordinary recall continue;
  observations become stale/unavailable with an explicit watermark.
- **Learning pipeline unavailable:** observations remain readable, but no durable procedure changes.
- **Context packing failure:** fail to a bounded, current, authority-filtered fallback; never an
  unfiltered graph dump.
- **Partial batch write:** assertion, evidence, commit/change rows, and observation dependencies use
  real transaction semantics. Unsupported backends report typed unavailability rather than weaker
  atomicity.
- **Concurrent ingestion:** group/scope selection is request-local and cannot mutate shared driver
  state used by another request.

## §8 Delivery slices

### P0 — Contracts and goldens

- Freeze the dual-axis temporal semantics and result schema.
- Add temporal, contradiction, access, and evidence-attribution goldens.
- Extend benchmark result schemas with sufficiency/minimality fields.
- No production behavior change.

### P1 — Exact evidence attribution

- Add episode-index/source-span fields to extraction responses.
- Validate bounds, hashes, stance, actor, and batch identity.
- Remove whole-source fallback for model-extracted claims.
- Add idempotency, rollback, and erasure tests.

### P2 — Semantic assertion retrieval in shadow

- Add the distinct semantic channel and dual-axis filter.
- Emit explain traces and shadow deltas against current recall.
- Keep prompt injection unchanged until quality and safety gates pass.

### P3 — Observation substrate in shadow

- Materialize deterministic, evidence-linked observation candidates.
- Refactor recurrence output to emit observations before learning proposals.
- Add refresh, contradiction, scope-inheritance, and retirement behavior.
- No observation enters normal context yet.

### P4 — Reviewed learning and typed context

- Route eligible observations through the existing proposal/review pipeline.
- Record retrieval/exposure/application outcome attribution.
- Add bounded observation and approved-procedure context channels behind flags.

### P5 — Promotion and cleanup

- Run the full sufficiency, temporal, safety, learning, latency, and token gates.
- Promote channels independently; retain instant rollback.
- Retire compatibility-only paths only after caller and parity audits prove they are no longer
  required.

Each slice has its own schema compatibility, migration, rollback, source-guard, unit, integration,
and benchmark evidence. A default flip is a separate reviewed decision, not an incidental part of
implementation.

## §9 Binding acceptance checks

```yaml acceptance
- {id: 1, tier: mechanical, check: "scripts/check_semantic_retrieval_boundary.sh --separate-from-cooccurrence --single-canonical-assertion-store --single-mutation-gate --forbid-unfiltered-historical-default"}
- {id: 2, tier: integration, check: "scripts/test_temporal_assertion_retrieval.sh --valid-at --believed-at --both-axes --current-default --all-hops-filtered --authority-lifecycle-scope --historical-labels"}
- {id: 3, tier: integration, check: "scripts/test_exact_evidence_attribution.sh --episode-indices --exact-spans --span-hashes --actor-capture --stance --batch-id --duplicate-corroboration --rollback --erasure --forbid-whole-source-fallback"}
- {id: 4, tier: integration, check: "scripts/test_observation_contract.sh --evidence-linked --deterministic-refresh --contradiction --supersession --retirement --visibility-partitions --deny-dominant-metadata --read-only"}
- {id: 5, tier: integration, check: "scripts/test_failure_learning_loop.sh --failed-attempt --recovery --observation --reviewed-proposal --no-direct-action --application-attribution --failed-procedure-revision --rollback"}
- {id: 6, tier: integration, check: "scripts/test_derived_context_security.sh --all-evidence-authorized --no-mixed-scope-summary-leak --untrusted-data-isolation --no-authorization-from-memory --zero-authority-violations"}
- {id: 7, tier: deployment, check: "scripts/run_memory_sufficiency_gate.sh --complete-partial-insufficient --answer-accuracy --minimal-sufficient-context --temporal-matrix --citation-validity --channel-breakdown --tokens-latency-cost --human-calibrated-judge"}
- {id: 8, tier: deployment, check: "scripts/run_learning_observation_gate.sh --recurrence-precision --recovery-accuracy --negative-transfer --correction-adherence --retirement-latency --require-attributed-application"}
- {id: 9, tier: integration, check: "scripts/test_observation_retrieval_failure_modes.sh --typed-degradation --checkpoint-resume --no-empty-pass --transactional-batch --concurrent-scope-isolation --instant-rollback"}
```

## Non-goals

- Replacing the canonical assertion store or creating a second fact graph.
- Letting a model decide authority, authorization, correction precedence, or promotion.
- Treating entity/context summaries as canonical truth.
- Reintroducing historical facts into current recall by default.
- Replacing the existing co-occurrence graph, vector index, lexical index, or fusion framework.
- Adding a new reranker without a measured same-corpus improvement.
- Automatically editing protected procedures, rules, or skills from an observation.
- Making heavyweight synthesis a prerequisite for base memory readiness.
- Using a flat grouping field as tenancy or access control.
- Claiming recursive learning from un-attributed outcomes.

## Resolved implementation decisions

1. Observations and application-attribution events use dedicated normalized tables; proposal
   payloads remain review artifacts and are not treated as the authoritative observation record.
2. Vectors derive from a canonical assertion rendering and carry assertion version plus embedding
   model/dimension identity; canonical fact text remains in the assertion store only.
3. Omitted time axes resolve to the request's current view. Explicit axes remain independently
   replayable and invalid precision fails closed.
4. Promotion-quality recurring observations require at least two evidence items from two
   independent sessions. A directly attributed failed application may immediately mark its exact
   procedure unstable, but can only create a review proposal.
5. Successful-recovery, failed-attempt, precondition, tool-sequence, and environment patterns may
   generate review proposals when the failure-learning gate is enabled. None may self-promote.
6. Evidence rows preserve policy-allowed durable locators, spans, hashes, stance, and scope. Raw
   source retention remains governed separately; erasure recomputes dependent observations.
7. The committed promotion contract requires at least twelve human-scored calibration cases with
   agreement of at least 0.90, complete manifests, and independently reported retrieval and answer
   grades. Representative production activation still requires an operator-reviewed run.

The implementation and its acceptance checks are closed. The feature remains shadow-safe and
default-off until the separate production activation decision is approved.
