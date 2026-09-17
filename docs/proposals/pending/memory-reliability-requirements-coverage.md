# Requirements Coverage

This matrix assigns each required concept to concrete proposal and test ownership. Primary ownership avoids duplicate implementations; related proposals consume the same contract.

| Requirement | Primary proposal | Integration / acceptance ownership |
|---|---|---|
| Golang-only memory module; C clients owned by external hosts | [Program integration contract and G0 extraction](memory-reliability-00-program.md#go-memory-module-integration) | [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) zero-C/header/descriptor gate, `CGO_ENABLED=0` build, host registration and process/placement parity; no forwarding or cgo exceptions |
| Consistent current/historical lifecycle, suppression and authorization | [MR-01](memory-reliability-01-unified-eligibility-and-validity.md) | [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) cross-surface/non-owner-role fixtures |
| World-time and belief-time correctness | [MR-01](memory-reliability-01-unified-eligibility-and-validity.md) | [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) coherent requirements; [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) interval/correction cases |
| Authority-preserving create/upsert/update/delete | [MR-02](memory-reliability-02-authority-preserving-mutations.md) | Durable transition, concurrency and tombstone tests |
| Durable cross-owner invalidation and collection generations | [MR-02](memory-reliability-02-authority-preserving-mutations.md) | [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md) replay progress, lagging release and verified erasure; [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) commit/publication and consumer-crash barriers |
| Actual serialized context byte/token caps | [MR-03](memory-reliability-03-final-payload-context-budgets.md) | Final provider-shaped request capture in [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) |
| Protected constraints and transformation integrity | [MR-03](memory-reliability-03-final-payload-context-budgets.md) | Existing economizer admission plus negation/limit fixtures |
| Independent evidence families and no self-corroboration | [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md) | Duplicate, composite, unknown-origin and hidden-parent fixtures |
| Complete derived lineage, retraction and restore-resistant erasure | [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md) | [MR-11](memory-reliability-11-embedding-generations-and-index-freshness.md) indexes; [MR-13](memory-reliability-13-disposable-task-projections.md) caches; [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md) release/admission |
| Requirement-based context sufficiency | [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) | False-complete and all-required-evidence coverage in [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) |
| Source-chain coherence and residual recovery | [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) | Bounded expansion, no gold leakage and partial outcomes |
| Actual arm/fusion/prior ranking explanation | [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) | [MR-09](memory-reliability-09-fair-hybrid-ranking-and-exposure.md) score semantics; concurrent/nested trace tests |
| Retained versus retrieved versus sent evidence | [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) | Exact final retained IDs and staged dispatch/acknowledgement |
| Pre-inference durable receipt | [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) | Preparation survives crash; unavailable durability blocks governed send |
| Dispatch admission versus observed transport and crash uncertainty | [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) | [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) handoff/acknowledgement crash matrix; [MR-08](memory-reliability-08-retrieval-health-telemetry.md) unknown-dispatch population |
| Evidence strength and honest replay limits | [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) | Independent verification states; digest-only cannot reconstruct |
| Binding retrieval/context exploration contract | [MR-07](memory-reliability-07-task-exploration-contracts.md) | Host issuance, task identity, typed budgets and dual-guard parity |
| Confidence calibration and no false trust in similarity | [MR-07](memory-reliability-07-task-exploration-contracts.md) | [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) requirement coverage; [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) calibration/false restriction |
| Starvation relaxation and explicit expansion | [MR-07](memory-reliability-07-task-exploration-contracts.md) | Wrong-plan/empty-index fixtures; operator ceiling never widened |
| Concentration, recurrence and entropy | [MR-08](memory-reliability-08-retrieval-health-telemetry.md) | Defined served population and hand-calculated metric fixtures |
| Low-trust/low-confidence/inferred fan-out | [MR-08](memory-reliability-08-retrieval-health-telemetry.md) | Scoped identity, task/family counts, meaningful denominators |
| Lifecycle re-entry and scope-violation health | [MR-08](memory-reliability-08-retrieval-health-telemetry.md) | Current versus historical populations; exact required counters |
| Evidence-family diversity and sole-support displacement | [MR-08](memory-reliability-08-retrieval-health-telemetry.md) | [MR-09](memory-reliability-09-fair-hybrid-ranking-and-exposure.md) selector preservation; unknown family coverage shown |
| Arm contamination and fusion recovery | [MR-08](memory-reliability-08-retrieval-health-telemetry.md) | Labels/verifier required; no inference from score alone |
| Hybrid serving parity and fair candidate admission | [MR-09](memory-reliability-09-fair-hybrid-ranking-and-exposure.md) | Full lexical pool cannot starve semantic/graph-only evidence |
| Bounded individual and aggregate ranking priors | [MR-09](memory-reliability-09-fair-hybrid-ranking-and-exposure.md) | Explicit maximum-overturn property on the actual score scale |
| Exposure-aware diversity without self-reinforcement | [MR-09](memory-reliability-09-fair-hybrid-ranking-and-exposure.md) | Preserve constraints, authoritative corrections and sole support |
| Type floors/caps under global hard budgets | [MR-09](memory-reliability-09-fair-hybrid-ranking-and-exposure.md) | Conflicting floors recorded; no hard-cap exception |
| Deterministic type/domain utility horizons | [MR-10](memory-reliability-10-deterministic-utility-horizons.md) | Authenticated anchor/override; serving does not renew usefulness |
| Historical access after horizon expiry | [MR-10](memory-reliability-10-deterministic-utility-horizons.md) | Only horizon restriction bypassed; access/erasure still enforced |
| Embedding identity and safe same-dimension change | [MR-11](memory-reliability-11-embedding-generations-and-index-freshness.md) | Generation migration/cutover/rollback fixtures |
| Index admission distinct from temporal serving eligibility | [MR-11](memory-reliability-11-embedding-generations-and-index-freshness.md) | Historical coverage after rebuild, future-valid activation without writes and query-time [MR-01](memory-reliability-01-unified-eligibility-and-validity.md) release checks |
| Bounded read latency and indexing freshness | [MR-11](memory-reliability-11-embedding-generations-and-index-freshness.md) | Cold/backlog tests; honest lag/fallback state |
| Named briefings/current state/constraints/decisions/failures/procedures | [MR-12](memory-reliability-12-served-memory-views-and-claim-cards.md) | Shared view API, minimal projection and adapter parity |
| Open contradiction and historical views | [MR-12](memory-reliability-12-served-memory-views-and-claim-cards.md) | No hidden-side disclosure; explicit temporal purpose |
| Evidence-backed claim cards and operator correction | [MR-12](memory-reliability-12-served-memory-views-and-claim-cards.md) | Canonical expected-version writes; cache invalidation |
| Complete cache request identity and query-collection dependencies | [MR-12](memory-reliability-12-served-memory-views-and-claim-cards.md) | New constraints/contradictions, empty results, temporal boundaries, changed query/view/budgets and current-owner watermark fixtures |
| Task-scoped non-authoritative working models | [MR-13](memory-reliability-13-disposable-task-projections.md) | Audience intersection, versioning, expiry and promotion gates |
| Bounded proposal-only memory hygiene | [MR-14](memory-reliability-14-proposal-only-memory-hygiene.md) | Narrow privileges, review-bound diffs, rejection deduplication |
| Deterministic safe maintenance only | [MR-14](memory-reliability-14-proposal-only-memory-hygiene.md) | Disposable cleanup/job queuing cannot rewrite canonical facts |
| Procedure experience and counterexample envelopes | [MR-15](memory-reliability-15-procedure-outcomes-and-task-cost.md) | Applied versus exposed, version cohorts and verified outcomes |
| Delayed reward tied to delivered/applied evidence | [MR-15](memory-reliability-15-procedure-outcomes-and-task-cost.md) | Count proxy cannot earn correctness; unknown outcomes explicit |
| Full task cost, retries and cache economics | [MR-15](memory-reliability-15-procedure-outcomes-and-task-cost.md) | Actual/estimated/counterfactual separation and quality gate |
| Evidence reauthorization at exact action admission | [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md) | Revocation ordering, request/destination binding and leases |
| Idempotent effects and truthful completion claims | [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md) | Duplicate/unknown outcome and downstream-verification tests |
| Composition-aware tool policy and aggregate budgets | [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md) | Trusted registry, cross-turn/delegate lineage and reservations |
| Clean retries without fabricated rollback | [MR-17](memory-reliability-17-clean-retry-context.md) | Current reauthorization, bounded lessons and real action journal |
| Frozen adversarial/multi-turn benchmark corpus | [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) | All feature groups and expected violations covered |
| Answerability, exclusions and metric denominator integrity | [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) | Mixed answerable/unanswerable confusion matrices |
| Comparable retrieval units, budgets and reader/judge sensitivity | [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) | Versioned manifests, paired ablations and held-out evaluation |
| Migration/documentation and advertised-parameter parity | [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) | Endpoint ownership matrix; ignored parameters fail conformance |
| Independent optional-feature rollout and rollback | [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) | Revert experiments while retaining corrected hard invariants |

Every proposal has its own acceptance gates and rollback behavior. The common program index defines cross-cutting ownership, configuration semantics and deployment order.
