# MR-18 and 18-proposal implementation closeout — 2026-09-27

MR-01–18 implementation is complete in [PR #2990](https://github.com/RakuenSoftware/aimee/pull/2990), within the documented supported modes and operator-approved observe scope. This closes implementation and deterministic acceptance; it does not promote the optional adaptive policies or claim a new model-quality result. MR-07 enforcement promotion was explicitly deferred by the operator.

The final executable revision is `1fac4f35cafc7a9d29c4f551614886989dab5f18`.
Its candidate image is `aimee-pr2990:1fac4f35c`, image ID
`sha256:4128001946cc046bac7159e65c856eff6e199c7324e5de386ce8931c28c3281d`.
The [evidence manifest](memory-mr18-evidence-2026-09-27/sha256.json) pins the final gate, fresh deployment checks, test logs and harnesses. Documentation-only closeout commits do not change this executable identity.

## Final validation

The frozen release runner passed on a clean checkout of the executable revision:

- **173 required Go tests across 12 adversarial groups**, with race detection and real PostgreSQL stores; all required cases executed and none skipped.
- **40 required Python tests**, all executed and none skipped.
- Manifest SHA-256 `63dd592f27965e6f94cac69387a4566880a125df3b5832d5f7f53253ae3bda24`; original 105-case corpus and 123 acceptance-clause inventory retained unchanged.

The broader memory/PostgreSQL race and export suites, CLI evaluator race suite, real C-host/Go-process wire and Server/KB restart conformance, 781-test Python suite, 14-case S1 integration parity suite, 77 lint checks, memory-boundary/ownership checks and generated-documentation check passed. The full Python/lint runs preceded the final small coverage/gate-runner additions; the final frozen 40-test Python gate and YAML validation cover those additions. Earlier proposal evidence remains tied to its original revision and is not relabeled as execution on the final image.

The fresh CT109 upgrade preserved both private stores, workspaces, Vaults and enrolled identities, after quiesced mode-0600 snapshots. Server and KB became healthy on the pinned image. Snapshot hashes and administrator-only role prerequisites are retained; private dumps and configuration are not published.

On that image, **43 CLI/MCP/HTTP served-view checks**, **30 action checks (including 12 timed governed reads)** and **20 clean-retry checks plus durable reservation assertions** passed. These exercise correction/empty-result invalidation, hidden sources, clock-only activation, historical identity, idempotent writes, changed destinations, unknown effects, actual Server restarts and fresh provider payloads after a failed attempt and procedure correction. The retry fixture uses a controlled primary-provider adapter; it proves functional behavior, not live-model task quality. Action timings are local API wall time, not isolated policy overhead.

The fixture restored the prior runtime policy and model configuration. Final health confirms CT100 released 0.4.5 and both CT109 candidate owners healthy, with optional policies unpromoted and clean retry disabled. The CLI PR-check command returned no checks output; no remote-CI success is claimed.

## What MR-18 adds

Every dataset question now remains in the evaluation inventory, including unanswerable questions and exclusions. Ordered IDs, dataset hashes, explicit answerability, failures and caps make incomplete runs visible. Undefined metrics serialize as null. Answerability reporting includes the confusion matrix, false abstention, unsupported answers and an observed risk/coverage point. Go relevance scores exclude unscored questions from their denominator while reporting total cases separately. Gold answers and evidence stay in evaluation; they are not passed to the serving planner or reader.

Versioned paired manifests freeze both arms, ordered tasks, inputs, model/tokenizer/index identities, flags, budgets, seeds, judge/reader and the cost basis. Protocol-compatible and product-optimized runs remain separate. The promotion evaluator refuses incomplete or underpowered pairs; the fixed policy requires at least 30 pairs and uses 10,000 paired bootstrap resamples. Estimated token-rate costs remain explicitly estimated. Passing deterministic invariants does not establish model-quality, p95 or cost noninferiority.

The required CI gate verifies source/input hashes and exact named-test execution; a missing case, skipped required subtest or failing package refuses a passing artifact. The [serving matrix](memory-mr18-serving-parity-2026-09-27.md) records ownership, stores, scope, modes, index state, activation, budgets, receipts and unsupported behavior across CLI, MCP, HTTP, bus, ingress, provider dispatch, maintenance and compatibility routes. See the [evaluation guide](../memory-reliability-evaluation.md) for operation.

## Repairs exposed by the final gate

The full retrieval evaluator exposed a real candidate defect: unit-semantic hits lacked source-version observations, so strict cross-channel identity checks correctly rejected otherwise current dense hits. Unit recall now carries the owner/revision identity, and whole/unit merges reject mismatched or absent versions. The release guard was not weakened.

Broad integration checks also exposed stale test expectations. The S1 fixture now reverses only the exact reviewed memory-adapter additions, preserving its unrelated-change rejection tests. Stage/command counts match the existing process contracts. The real C-bus harness serves only the exact private-owner startup erasure-replay query through an external test storage fixture before advertising readiness; it does not bypass production startup or add a native memory implementation.

The first image build exhausted CT109 disk space. Exact obsolete archived sources and reviewed reclaimable source-cache IDs were removed; running images, rollback image, stores and evidence were preserved. The retry built successfully.

## Per-proposal closeout

| Proposal | Implemented result and acceptance record | Operating scope |
|---|---|---|
| MR-01 | [Unified eligibility and validity](memory-mr01-closeout-2026-09-24.md) | Complete; current and supported historical modes |
| MR-02 | [Authority-preserving mutations](memory-mr02-closeout-2026-09-24.md) | Complete; durable versioned mutation contract |
| MR-03 | [Final payload budgets](memory-mr03-closeout-2026-09-24.md) | Complete; protected projections and final admission |
| MR-04 | [Lineage and independent support](memory-mr04-closeout-2026-09-25.md) | Complete; unknown lineage remains unknown |
| MR-05 | [Sufficiency and bounded recovery](memory-mr05-closeout-2026-09-25.md) | Complete; explicit requirements and gaps |
| MR-06 | [Ranking traces and receipts](memory-mr06-closeout-2026-09-25.md) | Complete; admission, handoff and acknowledgement distinct |
| MR-07 | [Durable exploration contracts](memory-mr07-session-2026-09-26.md#operator-approved-observe-completion) | Observe implementation complete; enforcement promotion deferred by operator |
| MR-08 | [Health telemetry](memory-mr08-health-2026-09-26.md) | Complete; optional collection off by default |
| MR-09 | [Retrieval selection and ranking](memory-mr09-ranking-2026-09-26.md) | Baseline/selector complete; adaptive policy disabled and unqualified |
| MR-10 | [Utility horizons](memory-mr10-horizons-2026-09-26.md) | Functional acceptance complete; optional policy disabled |
| MR-11 | [Embedding generations](memory-mr11-generations-2026-09-27.md) | Complete within declared adapter modes |
| MR-12 | [Served views and claim cards](memory-mr12-views-2026-09-27.md) | Complete; scoped current/historical identities |
| MR-13 | [Task projections](memory-mr13-task-projections-2026-09-27.md) | Complete; disposable state and explicit promotion |
| MR-14 | [Memory hygiene](memory-mr14-hygiene-2026-09-27.md) | Complete; proposals require version-bound review |
| MR-15 | [Procedure experience and task costs](memory-mr15-procedure-experience-2026-09-27.md) | Functional acceptance complete; attribution/cost reporting observe-only |
| MR-16 | [Governed actions](memory-mr16-actions-2026-09-27.md) | Complete for documented adapters; unknown effects block conflicting retry |
| MR-17 | [Clean retries](memory-mr17-retries-2026-09-27.md) | Functional acceptance complete; optional capability disabled |
| MR-18 | This report, frozen gate and serving matrix | Complete; measured adaptive promotion remains a separate gate |

## Release boundaries

CT100 remains on released 0.4.5 (`aimee-native-core:0.4.5-bridge.2`); PR features were validated in the isolated CT109 candidate. This report does not claim that PR #2990 has merged or that its candidate has replaced production. Full DB2 retirement is a separate proposal.

MR-07 requires a frozen independent Aimee code-navigation workload and the operator-selected documented token-rate cost model before enforcement promotion. MR-09's small controlled pilot is unqualified; MR-10 domain tuning remains gated. No new live-model paired experiment or measured adaptive benefit is asserted here.

MR-15 has no automatic host-execution attribution adapter and cannot turn model claims into verified application outcomes. MR-16 effect confirmation is limited to documented exact-resource adapters. MR-17 refuses unsupported token-cap enforcement; its byte-cost reservation is not billing, in-flight cancellation is separate, and retained-input expiry uses logical purge on the next operation rather than a physical-erasure promise. These limits are explicit operator contracts, not hidden passing measurements.
