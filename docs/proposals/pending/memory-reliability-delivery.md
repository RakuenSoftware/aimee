# Memory reliability delivery tracker

This records implementation evidence and remaining acceptance work for PR #2983.
It does not turn proposal requirements into passing release gates. The scope is
one Go memory implementation in Server/private and KB/shared placements, with
Go module-side producers/consumers using the existing C bus. The bus stays C.

## Authorized work

| Work | Current evidence | Remaining acceptance |
|---|---|---|
| 1. G0 ownership and documentation | Zero native files/descriptor entries; CGO-disabled module and live probe; both C-bus placements; native labelled audit/calibration and live benchmark policy moved to Go; orphan fusion fixture replaced with production Go assertions | Server KB store/list/get/delete/supersede, search and context-read preserve complete Go envelopes and owner refusals; fact/window search orchestration is Go-owned. Complete the remaining external-caller ownership review, retire stale ABI/fixtures and record per-owner conformance evidence. Broad native-name findings are not themselves C memory implementations. |
| 2. Retrieval compatibility | Versioned whole-record/unit/temporal semantic recall, opt-in Go PageRank, independent-arm RRF with per-arm deduplication | [Compatibility decisions](memory-reliability-retrieval-compatibility.md) are recorded; extend the frozen corpus to the full adversarial matrix and run paired quality measurements. |
| 3. Evaluation and release evidence | Shared injected Go module; isolated corpus/dataset/QA/support/miss runners; scoped runtime-role and transport regressions | Initial frozen 105-case input and manifest-bound corpus baselines/per-case receipts are implemented; live CLI/Server benchmarks share the Go owner and refuse partial results; paired quality/performance and the complete CLI/MCP/HTTP/bus restart/failure matrix and required CI remain. |
| 4. MR-01–18 | Existing foundations below | Finish each proposal's acceptance gates and integration dependencies. None is certified complete by the language migration. |

## Proposal acceptance ledger

| Proposal | Implemented foundation | Work still required for full acceptance |
|---|---|---|
| MR-01 eligibility | Scoped transactions, placement separation, current-hash filters and Go current-validity predicates before lexical/semantic/unit/graph/window/bundle/activation/briefing limits | Unified temporal/utility policy on every surface, release recheck and race tests |
| MR-02 mutations | Shared KB admission across same-key/edit/legacy verbs, user/model versions, original-author preservation, identity locking, tombstones and atomic extraction actor/job writes | Linked review proposals, personal versioning, expected-version/idempotency contracts, durable guards, invalidation outbox and replay |
| MR-03 budgets | Go typed-context assembly and existing packing limits | Final serialized provider byte/token caps and protected-projection accounting |
| MR-04 lineage | Fact lineage/review and invalidation paths | Independent-family accounting, full derivative closure and restore-resistant erasure |
| MR-05 sufficiency | Typed context and answer abstention | Requirement/coherence coverage of retained evidence and bounded recovery |
| MR-06 receipts | Owner diagnostic parts and postcommit observations | Durable pre-inference receipts, dispatched/acknowledged stages and crash uncertainty |
| MR-07 exploration | Existing adaptive recall limits | Host-issued task contracts, calibrated starvation recovery and operator ceilings |
| MR-08 health | Go lane counters and PageRank timing | Served-population concentration/entropy/fanout/reentry metrics with exact denominators |
| MR-09 ranking | Eligible semantic lanes, RRF, per-arm deduplication, bounded opt-in PageRank | Aggregate prior bounds, final exposure diversity/type floors and adversarial quality gates |
| MR-10 utility | Existing temporal/lifecycle fields | Deterministic utility horizons, authenticated time anchors and historical-access rules |
| MR-11 embeddings | Pinned identity/current hashes, versioned rebuild/cutover/rollback | Full freshness/temporal-activation acceptance under concurrent rebuild/failure |
| MR-12 views | Go briefing/alerts and visible parent checks | Named view/claim-card contracts and full scoped collection-generation cache identity |
| MR-13 task projections | No release claim | Audience intersection, disposable provenance and promotion gates |
| MR-14 hygiene | Existing lint/review surfaces | Proposal-only privileged boundary and rejected-proposal deduplication |
| MR-15 outcomes | Existing feedback/workflow and learning paths | Verified application versus exposure, delayed outcomes and complete task cost |
| MR-16 actions | Existing host authorization boundaries | Exact action evidence reauthorization, idempotent effects and composition budgets |
| MR-17 retries | No release claim | Clean reasoning context with retained real-action journal and replay prevention |
| MR-18 release gates | Go owner tests, live C bus, isolated evaluators, frozen corpus and manifest-bound per-case baselines | Complete adversarial manifests, temporal reproducibility, paired quality/performance and required surface/restart/failure gates |

Historical notes are preserved in the [program migration history](memory-reliability-migration-history.md)
and [module migration history](../../modules/memory-migration-history.md). Their
counts and pending statements are dated checkpoints, not current certification.
