# Aimee Memory Reliability: Proposal Series

**State:** Proposed · **Revision:** 1 · **Date:** 17 September 2026

## Decision requested

Adopt the following 18 proposals as a dependency-ordered improvement program for memory correctness, evidence quality, context efficiency and operational reliability. Review and implement each proposal in its defined slices. Approval of the program is not approval to enable every optional policy in production.

The program reuses Aimee's personal/shared memory ownership, PostgreSQL schema, typed context, event bus, reviewed learning, execution policy, audit/WORM and economizer. It establishes consistent contracts across those owners and adds derived views and diagnostics where needed.

All command names, schema additions, defaults and release thresholds described as proposed are design work. The acceptance gates are requirements to implement and run; this package does not represent them as completed changes or passing tests.

## Proposal index

| ID | Proposal | Priority | Primary outcome |
|---|---|---|---|
| [MR-01](memory-reliability-01-unified-eligibility-and-validity.md) | Unified retrieval eligibility and validity | P0 | Every serving surface applies the same authorized lifecycle/time rules |
| [MR-02](memory-reliability-02-authority-preserving-mutations.md) | Authority-preserving memory mutations | P0 | All write verbs preserve author authority, provenance and history |
| [MR-03](memory-reliability-03-final-payload-context-budgets.md) | Final-payload budgets and protected projections | P0 | Actual provider-bound context obeys byte/token caps |
| [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md) | Evidence lineage, independent corroboration and retraction | P1 | Copies cannot self-corroborate; corrections/erasure reach derivatives |
| [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) | Requirement-based sufficiency and bounded recovery | P0/P1 | Completeness reflects delivered evidence and declared task needs |
| [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) | Actual traces, context receipts and evidence states | P0/P1 | Explain ranking and distinguish assembled, dispatched and acknowledged context |
| [MR-07](memory-reliability-07-task-exploration-contracts.md) | Task-bound exploration contracts | P1 | Reduce redundant discovery without trapping tasks or widening permission |
| [MR-08](memory-reliability-08-retrieval-health-telemetry.md) | Retrieval health telemetry | P1; baseline starts immediately | Detect concentration, lifecycle leakage and low-trust spread with defined metrics |
| [MR-09](memory-reliability-09-fair-hybrid-ranking-and-exposure.md) | Fair hybrid candidates, bounded priors and exposure selection | P1/P2 | All retrieval arms compete; priors/diversity cannot dominate requirements |
| [MR-10](memory-reliability-10-deterministic-utility-horizons.md) | Deterministic utility horizons | P1 | Expire ordinary usefulness of transient state without deleting valid history |
| [MR-11](memory-reliability-11-embedding-generations-and-index-freshness.md) | Embedding generations and bounded index freshness | P1 | Prevent vector-space mixing and retrieval stalls during rebuilds |
| [MR-12](memory-reliability-12-served-memory-views-and-claim-cards.md) | Named served-memory views and claim cards | P1 | Deliver task-oriented briefings and inspectable evidence-backed claims |
| [MR-13](memory-reliability-13-disposable-task-projections.md) | Disposable task projections | P2 | Keep working hypotheses useful without promoting them to canonical truth |
| [MR-14](memory-reliability-14-proposal-only-memory-hygiene.md) | Proposal-only hygiene | P2 | Detect memory problems while keeping canonical changes reviewable |
| [MR-15](memory-reliability-15-procedure-outcomes-and-task-cost.md) | Procedure experience, delayed outcomes and task cost | P1/P2 | Learn from verified application outcomes and account for complete task cost |
| [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md) | Evidence-bound actions and composition policy | P1 | Reauthorize exact effects and prevent sequence/idempotency bypasses |
| [MR-17](memory-reliability-17-clean-retry-context.md) | Clean retry context with preserved real-world state | P2 | Isolate failed reasoning without hiding or replaying actual effects |
| [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) | Frozen evaluation, migration parity and release gates | P0; continuous | Measure equivalent behavior and block invariant regressions |

P0 denotes correctness foundations and required evaluation. P1 denotes the next integration wave. P2 denotes measured extensions. A P2 feature may be designed earlier; it must not be enforced before its dependencies and evaluation gates are ready.

## Common contracts and ownership

These definitions apply to every proposal. Individual files restate their critical invariants so that they can be reviewed separately.

| Concept | Shared definition |
|---|---|
| Authenticated scope | Host-derived principal, audience, purpose and current permission; a caller's scope string is not a grant |
| Canonical memory | Versioned durable assertion/experience/instruction admitted by the existing owner |
| Derived projection | Rebuildable state carrying declared input versions and inherited access; no independent authority |
| Valid time / belief time | Separate temporal coordinates, with explicit support or rejection by each endpoint |
| Eligibility | Hard policy decision before ranking and again at release; similarity cannot override it |
| Utility horizon | Ordinary-serving policy for usefulness, separate from truth, validity, retention and erasure |
| Evidence family | Established common origin/dependency group; distinct identifiers alone do not prove independence |
| Sufficiency | Coverage of declared task requirements by the final retained evidence; separate from answer correctness |
| Context plan | Versioned selection/requirement/projection result bound to the request/task and its inputs |
| Context receipt | Durable commitment and staged evidence about assembly/preparation/dispatch; its state must match what happened |
| Exploration contract | Host-issued adaptive work limit that can tighten, never widen, operator permission or ceilings |
| Outcome | Host-verifiable result or explicitly attributed feedback; automatic exposure is not successful use |

Keep policy artifacts versioned and atomically switchable. Record the effective versions used by a request. Distinguish absent limits, disabled adaptive policies and literal zero allowances; do not overload integer zero across incompatible meanings.

A budget's identity includes its unit and reset scope. Token, byte, candidate, file, raw-scan, graph-work, time and monetary budgets are separate. Concurrent operations reserve against the relevant task/session ceiling atomically. Refund only under a defined outcome that proves the charged work was not admitted/executed; network uncertainty is not such proof.

Trace metadata and model context are separate projections. IDs, hashes and family links remain potentially sensitive. Keep authorized audience/purpose and retention on diagnostics, caches and audit records. Required action/context receipts cannot be replaced by sampled health events.

## Serving sequence

1. Authenticate identity, scope and purpose; choose the explicit temporal mode.
2. Retrieve bounded authorized arm pools with declared index state.
3. Apply eligibility, fuse/deduplicate and derive task requirements.
4. Select coherent evidence, apply bounded optional priors/diversity and produce the model projection.
5. Serialize, enforce hard budgets and evaluate requirement coverage over the exact retained evidence.
6. Perform only bounded, authorized recovery when necessary; revise the plan explicitly.
7. Reauthorize release, bind final provider-shaped bytes and durably append the governed preparation receipt.
8. Dispatch and record acknowledgement or uncertainty; use the context contract only within operator policy.
9. Attribute actual applications/outcomes and update scoped health/experience projections.
10. Maintain derived state and generate reviewable hygiene proposals without autonomous canonical rewrites.

This is a logical order across existing owners. It does not require a new process, database or synchronous external audit service at every stage. Where durability is required, the existing audit/WORM pipeline must acknowledge its specified durable boundary before dispatch.

## Delivery waves and dependencies

| Wave | Work | Exit condition |
|---|---|---|
| 0: Baseline and contracts | Start [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md); inventory current serving surfaces and parameter semantics; prepare [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md)/[MR-08](memory-reliability-08-retrieval-health-telemetry.md) event schemas | Fixed fixtures, honest baseline and identified owners/defaults |
| 1: Boundary correctness | [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-02](memory-reliability-02-authority-preserving-mutations.md), [MR-03](memory-reliability-03-final-payload-context-budgets.md); deterministic [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) requirements and [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) final-payload receipts | Eligibility/history/caps are enforced; incomplete context and unsent attempts are labeled correctly |
| 2: Evidence and retrieval | [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md), [MR-08](memory-reliability-08-retrieval-health-telemetry.md), candidate/prior slices of [MR-09](memory-reliability-09-fair-hybrid-ranking-and-exposure.md), [MR-10](memory-reliability-10-deterministic-utility-horizons.md), [MR-11](memory-reliability-11-embedding-generations-and-index-freshness.md), [MR-12](memory-reliability-12-served-memory-views-and-claim-cards.md); complete [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md)/[MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) integration | Independent support, lineage, fair candidates, readiness and views are inspectable |
| 3: Governed efficiency and effects | [MR-07](memory-reliability-07-task-exploration-contracts.md) observe then canary; reward-correction/attribution slices of [MR-15](memory-reliability-15-procedure-outcomes-and-task-cost.md); [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md) | Fewer redundant reads without starvation; current evidence governs exact effects |
| 4: Derived continuity and maintenance | [MR-13](memory-reliability-13-disposable-task-projections.md), [MR-14](memory-reliability-14-proposal-only-memory-hygiene.md), [MR-17](memory-reliability-17-clean-retry-context.md); measured experience/exposure/routing extensions | Temporary state stays non-authoritative; hygiene/retries preserve canonical and external history |

[MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) runs in every wave. [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md) moves earlier for any automated external-write workflow that already relies on remembered evidence. Health collection can start with existing final-selection events; any unavailable dimensions must be labeled missing until their producing proposal lands.

Implementation work may proceed in parallel once interfaces are fixed, but deployment dependencies remain explicit. In particular, restrictive [MR-07](memory-reliability-07-task-exploration-contracts.md) enforcement waits for truthful [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) coverage and [MR-03](memory-reliability-03-final-payload-context-budgets.md)/[MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) final context evidence. Independent-support requirements wait for [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md); simpler deterministic requirements can ship earlier.

## Proposed configuration and compatibility

Use existing configuration ownership and validation. Add small versioned policy sections only where needed; these are proposed controls, not claims of current configuration keys.

| Control | Initial behavior | Promotion rule |
|---|---|---|
| Eligibility and mutation fixes | Correctness enforcement after migrations/conformance tests | No tuning flag may bypass the corrected invariants |
| Final payload cap and receipt stages | Enforced per converted endpoint | Legacy endpoints explicitly identify unsupported guarantees |
| Exploration contracts | Observe, with independent operator opt-in for enforcement | Paired completion/efficiency and starvation tests pass |
| Retrieval health | Bounded collection with declared sampling/retention | Expand after overhead/privacy and population-metric checks |
| Utility horizons | Shadow; enforce only explicitly configured transient kinds | Historical preservation and domain usefulness gates pass |
| Ranking/exposure/routing | Versioned baseline; optional additions independently disabled | Held-out paired ablation and hard invariant gates pass |
| Task projections | Explicit reads first; automatic preload later | Scope, expiry, non-authority and invalidation gates pass |
| Hygiene | Manual dry run / proposal creation; scheduler opt-in | No canonical mutation without existing review/admission |
| Outcome learning | Observe attributable outcomes before fitting policies | Sufficient validated feedback and fixed evaluation |

Retain advertised legacy semantics through adapters. A compatibility parameter must work, be explicitly deprecated or be rejected; it must not be silently ignored. Legacy `ingress_max_raw_scans <= 0` remains disabled behavior, while a new typed contract can express a literal zero allowance without ambiguity.

## Review checklist

For every implementation slice, reviewers should be able to identify the problem, owner, contract version, state transitions, exact failure behavior, migration strategy, changed defaults, acceptance tests, observability and rollback. Keep a small PR when a slice changes a durable boundary. Do not group unrelated optional ranking experiments with authorization or history fixes.

No proposed threshold is evidence of an achieved improvement. Freeze the [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) evaluation policy before observing experimental outcomes. Distinguish verified effects from claims, unknown data from success, and retrospective explanation from execution-time evidence.

## Deferred experiments

Cross-encoder reranking, vector quantization, alternative ANN backends, community summaries, learned utility horizons and broader autonomous consolidation remain separate experiments. They require equal-budget quality/cost evidence and complete provenance/invalidations before production adoption. Their absence does not block the correctness program.

## Series contents

The 18 numbered proposal files are independently reviewable. This program index defines common contracts and delivery order. The [requirements coverage map](memory-reliability-requirements-coverage.md) assigns each requirement to a proposal and its acceptance gates.

All documents remain proposed. Each implementation slice requires its own review, validation and rollout decision.
