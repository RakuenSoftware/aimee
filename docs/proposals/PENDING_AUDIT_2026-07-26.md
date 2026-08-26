# Pending proposal audit — 2026-07-26

This is the exhaustive reconciliation of the 79 Markdown files that were in
`docs/proposals/pending/` at `testing` commit
`e34f339e12ac53bf6a74dd3699c9984419a889b3`. It records the decision for every
original file; newly created residual proposals are listed with their parent.

## Decision rules

1. Move a proposal to `done/` only when its scoped deliverable is shipped,
   superseded by the current implementation, or fully discharged by completed child slices.
2. For partial delivery, archive the original proposal (and its plan, if any) in
   `done/`, add an archive notice, and create one narrowly scoped pending residual.
3. Keep accurate, unimplemented proposals pending.
4. Update a still-live proposal in place when paths, claims, or assumptions are stale.
   Feature-branch-only implementation is not “done on testing” and still receives this stale-content review.

The evidence notation below is `proposal section; source; test/check; commit evidence`.
“Audit SHA” means the implementation was inspected at the full commit above, not inferred from a filename.

The machine-verifiable companion manifest is
[`PENDING_AUDIT_2026-07-26.tsv`](PENDING_AUDIT_2026-07-26.tsv). It has exactly one row per original
and records disposition, final path, residual path, stale-update state, evidence anchor, and the
roundtable decision. `python3 scripts/check_pending_audit_manifest.py` verifies the 79-row partition,
final directory membership, archive notices, explicit residual states, and reciprocal links.

## Moved to done: complete or superseded (20 files)

| Original file | Why complete | Evidence |
|---|---|---|
| `chunker-splits-exact-needles.md` | Its resolved match-centered extraction disposition is shipping. | “Resolution/current behavior”; `src/chunker.c`; chunker tests; audit SHA. |
| `economizer-lossless-json-live-transform.md` | The proposed single transform was superseded by the shipped `off`, `safe`, and `aggressive` model; future safety work has separate owners. | “Resolution”; `src/modules/economizer/`; economizer unit tests; `9d478dca`. |
| `mcp-tool-call-audit-on-bus.md` | Tool-call audit hook, bridges, outcomes, and transport are present. | “Landed”; MCP audit hook/bridges; MCP and server dispatch tests; `fc286c03`. |
| `response-orchestration-stages.md` | Request/response registries, governance response stage, orchestration seam, and delegate/workflow ports are shipping. | Slice sections; `src/pipeline/gw_response_registry.c`, `gw_orchestration_seam.c`; `test_gw_response_registry`, `test_gw_orchestration_seam`; `81e52add`. |
| `retrieval-result-cache-schema.md` | The cache schema, TTL, provenance/age, and egress recheck exist in the current web cache substrate. | Schema/acceptance; `src/db1/web_page_cache.c`, `src/posix/web_read.c`; `test_web_page_cache`; `facb8c02`. |
| `surface-neutral-retrieval-substrate.md` | Its retrieval premise was superseded and its security gate is shipping; remaining search work has separate proposals. | Security/DRY sections; web cache/search implementation; web cache/read/search tests; audit SHA. |
| `sliced-lifecycle-build-workflow.md` | Lifecycle panel, build/slice workflows, `branch.open`, foreach, child/fan-in persistence, and Go support are present. | Slice acceptance; `src/modules/workflows/wfe_live_panel.c`, `wfe_live_foreach.c`; workflow/panel tests; audit SHA. |
| `sliced-lifecycle-build-workflow.plan.md` | The companion execution plan is discharged by the same implementation. | Plan slices; same source/tests; audit SHA. |
| `tiered-llm-p5-oidc-control-plane.md` | P5 A–D are completed and represented by completed child slices. | Phase completion map; P5 auth/status/custody sources and PG tests; audit SHA plus child-plan PR evidence. |
| `tiered-llm-p5-oidc-control-plane.plan.md` | The P5 plan is discharged by completed child plans. | Plan gates; P5 PG tests; audit SHA plus child-plan PR evidence. |
| `tiered-llm-p5b1-status-authority.plan.md` | Status authority is implemented and its dedicated PG test exists. | Plan acceptance; status authority sources; `scripts/p5b1-status-key-pg17-test.sql`; audit SHA. |
| `tiered-llm-p6c-egress.plan.md` | The scoped Converse egress path is delivered; native InvokeModel breadth is a separate residual. | P6c gates; Bedrock Converse/egress sources; P6 slice tests; audit SHA plus completed P6 slice docs. |
| `tiered-llm-p7-hardened-kb-vault.md` | P7 E1–E3, external WORM witness, offline verification, and release gate are delivered. | P7 completion map; vault/witness sources; witness PG/offline-fuzz tests; merged PR `#1930` recorded by child plans. |
| `tiered-llm-p7-tpm2-reseal-orchestration.plan.md` | Reseal orchestration and its protected mutation/staging slices are complete. | Plan gates; TPM/reseal sources; P7 kill matrix and child tests; audit SHA plus completed child plans. |
| `tiered-llm-p8-thinclient-mtls.md` | Thin-client mTLS, durable ramp, enrollment, and per-request revocation are delivered. | P8 phase map; mTLS/enrollment sources; P8a/P8c tests; audit SHA plus completed child plans. |
| `tiered-llm-p8b-csr-enrollment.plan.md` | CSR enrollment slice is delivered. | Plan acceptance; enrollment/cert lifecycle sources; management certificate tests; audit SHA. |
| `web-retrieval-capability-map.md` | The assessment is complete and adopted mechanisms landed; its remaining recommendations already have dedicated live proposals. | Recommendation map; web cache/search sources; cache/search tests; audit SHA. |
| `orchestration-seam-delegate-firstport.md` | Both the delegate first port and workflow second port now use the orchestration seam and have tests. | “Follow-up port plan”; `gw_orch_delegates.c`, `gw_orch_workflows.c`; `test_gw_orch_delegates`, `test_gw_orch_workflows`; audit SHA. |
| `provider-neutral-cache-aware-economizer.md` | The specified proof infrastructure, provider planners, immutable wire snapshot, and dispatch fence are implemented. | Architecture/acceptance; `economizer_proof.c`, provider planners, `economizer_wire_snapshot.c`; economizer proof/wire tests; `9d478dca` and prerequisite commits. |
| `provider-neutral-economizer-safety-spec.md` | Its normative proof and immutable-dispatch invariants are implemented and tested. | Governing invariant/live-intervention classes; economizer proof/wire sources; proof, planner, and wire tests; audit SHA. |

## Moved to done with a new pending residual (23 original files, 21 residuals)

| Archived original(s) | Delivered scope and evidence | New pending residual |
|---|---|---|
| `capability-thresholded-delegate-routing.md` | Catalog identity/enrichment, pricing, provider-general routing, scope/health inputs, and auto-escalation retirement; delivered-status section, delegate/catalog sources and tests, audit SHA. | `capability-thresholded-delegate-routing-residual.md` |
| `dynamic-tool-egress-classification.md` | Host-CLI classification landed; proposal “partially delivered” section, tool dispatch/egress tests, audit SHA. | `dynamic-tool-egress-registration-identity.md` |
| `ir-sole-path-and-pluggable-stages.md` | Request registry and newer response/orchestration seams landed; stage sources/tests, `81e52add`; legacy builders/raw paths remain. | `ir-sole-path-residual.md` |
| `kb-hybrid-outcome-wiring.md` | B1/B2, pairwise/IPW consumption, overlap, and ranker capture landed; ranker/feature-row sources and tests, `462ade93`. | `kb-hybrid-outcome-wiring-residual.md` |
| `mcp-adapter-optional-module.md` | Direct adapter, server/KB federation, invocation, collision, audit, and E2E Phase 1 landed; MCP sources/tests, audit SHA. | `mcp-adapter-bus-routing-residual.md` |
| `mtls-transport-performance.md` | All implementation packets recorded in the proposal are merged; transport tests and audit SHA. Operational cohorts/default promotion remain. | `mtls-transport-rollout-evidence.md` |
| `persona-authored-outputs.md`, `persona-authored-outputs.plan.md` | `require_aimee_git` enforcement landed; plan enforcement section and config/tests at audit SHA. No permission/voice/commit-style implementation was found. | `persona-authored-outputs-residual.md` |
| `tiered-llm-offering.md`, `tiered-llm-offering.plan.md` | P1/P3/P4/P5/P7/P8/P10 are complete per phase map, sources/tests, and completed child plans at audit SHA. | `tiered-llm-offering-residual.md` |
| `tiered-llm-p2-kb-egress-authority.md` | P2a and initial P2b authority are delivered per phase sections and KB egress tests; forwarding/true streaming remain. | `tiered-llm-p2b-forwarding-and-streaming.md` |
| `tiered-llm-p6-bedrock-and-breadth.md` | Auth/catalog/Converse IR+stream/egress/transport are delivered per child slices at audit SHA. | `tiered-llm-p6-native-invokemodel-and-pricing.md` |
| `tiered-llm-p9-telemetry-tiering.md` | Local ingest and Prometheus export are delivered per P9a sections and telemetry tests at audit SHA. | `tiered-llm-p9-forwarding-and-otlp.md` |
| `user-selectable-fusion-mode.md` | Algorithms, config, request override, and benchmark engine are shipping; fusion sources and `test_kb_fusion`/graph-fusion tests at audit SHA. | `user-selectable-fusion-surface-residual.md` |
| `config-field-descriptor-table.md` | Eligibility, flat defaults, schema derivation, and table-driven parsing landed; config field sources/tests, `41704c10`. | `config-field-descriptor-save-residual.md` |
| `route-descriptor-single-source-of-truth.md` | Descriptor extraction and generated CLI routes landed; generator/descriptor drift check, `4c428086`. | `route-descriptor-single-source-of-truth-residual.md` |
| `per-query-grouping-key-for-ranking.md` | Retrieval-event grouping now supports outcome wiring, but feature rows still diagnose `missing_grouping_key`; ranker source/tests, audit SHA. | `per-query-feature-persistence-residual.md` |
| `remote-first-session-start.md` | Remote brief assembly, memory recall, exclusive transport selection, and never-empty floor landed; `cli_session_start.c`, session brief tests, `7c4e1c80`. | `remote-session-start-workspace-context.md` |
| `operator-audit-activity-surface.md` | `/v1/audit/actions` and ACL landed; `kb_http_governance.c`, route/ACL tests, `011bfee8`. | `operator-audit-activity-residual.md` |
| `core-substrate-and-source-module-boundaries.md` | Source modularization landed; module ownership checks at audit SHA. Separate-program/core/event-bus adoption is not on `testing`. | `core-substrate-and-source-module-boundaries-residual.md` |
| `module-runtime-source-ownership-and-build.md` | Source movement and `check_module_source_ownership.py` (15 contracts, one legacy root) pass at audit SHA. Descriptor-authoritative builds/event contracts remain. | `module-runtime-source-ownership-and-build-residual.md` |
| `git-core-contract.md` | Roundtable-approved contract checker and 27-test suite pass; proposal binding-check sections, `a3c4d413`. On 2026-07-26 the checker and `test_check_git_core_contract.py -q` both exited 0 after their contract reference moved to `done/`. Runtime adoption remains. | `git-core-contract-runtime-residual.md` |
| `compaction-quality-measurement.md` | Rounds-to-resume accounting and false-positive tests landed in `6641177d`; no checked-in agentic baseline was found. | `compaction-quality-baseline.md` |

## Left pending: accurate, unimplemented, or regressed (36 original files)

| File | Audit finding / evidence of the live gap |
|---|---|
| `agentic-supervised-swebench.md` | Explicit design-only benchmark; no supervised agentic SWE-bench harness matching its “measurement problem” and acceptance sections was found. |
| `aimee-core-capability-contract.md` | Approved contract still depends on the unmerged event-bus/core boundary work; its executable-core binding checks are not present on `testing`. |
| `appliance-state-recovery-runbook.md` | Required `docs/runbooks/appliance-state-recovery.md` does not exist. |
| `automatic-wfe-trigger-blob-dedup-runbook.md` | Required section is absent from `docs/wfe-autonomy-runbook.md`. |
| `capability-scoped-agent-execution.md` | Proposal says no implementation; current dispatch retains legacy/unbound behavior and lacks its resolve-once scope contract. Source paths were refreshed. |
| `code-graph-architecture-surface.md` | Current graph has symbol support but not the proposal’s five architecture-node/orientation/diff/coverage/impact slices. One header path was refreshed. |
| `dedicated-extraction-model-curator-tier-a.md` | Explicit design-only model tier; no dedicated extraction-model routing/evaluation matching §§1–6 was found. |
| `delegate-sandbox-aimee-sole-egress.md` | Existing Docker sandbox still has the direct tool/backend gap described in “The gap”; source paths were refreshed. |
| `delegate-sandbox-image-customization.md` | Design is still unimplemented; no complete per-project image resolution/build/cache policy matching its acceptance gates was found. |
| `event-bus-governance-and-capture.md` | Draft consumer proposal remains unimplemented on `testing`; feature-branch bus work does not supply the requested mainline governance/capture contract. Parent link was updated. |
| `event-bus-wire-spec.md` | Accurate feature-branch-only status: `origin/feat/event-bus` and its S1–S12 branches exist, but the implementation is not promoted to `testing`. Parent/owner links were updated. |
| `feature-liveness-and-background-curator-removal.md` | Previously delivered but currently regressed: `skill_curator.c`, its header, and build membership were reintroduced in `e9093c70`; the dedicated absence checker exits 1 with `rule=deleted-file`. The live proposal now records the repair requirement in place. |
| `frontend-development-module.md` | The existing computer-use pieces do not implement its browser executor and containment/visual-QA module slices. |
| `governance-agent-identity-and-artifact-trust.md` | Per-agent principals, delegation chains, fleet registry, and signed executable-artifact gates remain absent as a complete arc. One source/link reference was refreshed. |
| `governance-attestable-enforcement.md` | WORM remains default-off (`config_save.c` and `kb_audit_worm.c`); capture completeness, policy revisions, and `aimee audit attest` remain open. Paths/links were refreshed. |
| `governance-policy-surface-and-posture.md` | Profiles, complete ingress integrity gating, first-class approval verdict, and containment defaults remain open. Source paths were refreshed. |
| `kb-ingest-content-push-deltas.md` | Its content-push identity, monotonic ordering, force-push, and operator requirements remain a coherent unimplemented slice. |
| `large-refactor-delivery-and-compatibility.md` | The compatibility/recovery program remains pending. A false present-tense claim was corrected: `scripts/recover_refactor.sh` does not exist. Parent links were updated. |
| `local-first-memory-and-trust-patterns.md` | Survey explicitly distinguishes current mechanisms from proposed quarantine/codec/owner-gated patterns; the proposed deltas remain open. |
| `memory-auto-population-phase4.md` | Draft says not started; feedback-to-rules, quarantine extraction, and promotion gates are not implemented as specified. Source paths were refreshed. |
| `memory-learning-and-inference-boundaries.md` | Approved boundary rework remains dependent on the core/module program; binding checks are not satisfied. Parent links were updated. |
| `module-loader.md` | External/user-authored module loader and its trust/binding checks are not on `testing`. Parent link was updated. |
| `org-data-connectors-and-source-ingestion.md` | Explicit design-only connector contract; the proposed adapters and incremental sync/supersession system were not found. |
| `per-user-remote-writes-authz.md` | Explicit no-implementation draft; KB-signed token/tier administration/enforcement seam remains open. |
| `product-governance-web-and-config.md` | Runtime/control-plane split, web modules, and truthful effective-config binding checks remain open. Parent links were updated. |
| `proposal-evidence-provenance-tiers.md` | Proposed Tier-3 classification and fail-closed write-side gate remain absent as described. |
| `proposal-retrieval-context-contract.md` | Binding agent retrieval-context contract and the genuinely new Tier-C surfaces remain unimplemented. |
| `proposal-supersession-hygiene.md` | Same-commit lifecycle rule, reconcile enforcement, and one-time sweep are not yet implemented. |
| `search-egress-policy-split.md` | Search destinations are not yet separated from operator-configured endpoint policy as specified. Links to completed context documents were updated. |
| `standing-benchmark-cadence.md` | Benchmark runners exist, but the scheduled workflow, retained results store, and drift gate in §§1–3 do not. |
| `thin-client-capability-advertisement.md` | Registration-chain/capability advertisement work is not on `testing`; dependencies/links were refreshed. |
| `transactional-turn-rewind-and-session-recovery.md` | Existing snapshots do not implement the sealed turn-scoped change-set lifecycle and safe restore contract. Related source/proposal links were refreshed. |
| `v1-stability-and-distributed-validation.md` | V1 snapshot/stability gate plus distributed concurrency/isolation harness remain absent. |
| `web-search-fanout-resilience-accounting.md` | The document accurately records unresolved fanout/default-install/URL/circuit/provenance/accounting decisions and no complete implementation exists. |
| `webchat-project-lifecycle.md` | Org-scoped clone migration and true delete/purge lifecycle slices remain unimplemented; one Git source path was refreshed. |

## Stale-content updates applied to live proposals

- Replaced pre-modularization source paths with their current module-owned paths.
- Repaired links to archived parents or their new residual proposals.
- Corrected the nonexistent `scripts/recover_refactor.sh` from present-tense implementation to a future deliverable.
- Rechecked `event-bus-wire-spec.md`: the named remote feature branches exist, but no promotion to `testing` was inferred.

## Reference updates outside `docs/proposals/`

Moving machine-readable or source-cited proposals requires their consumers to follow the archive.
The audit therefore updates only proposal-path references in the Git contract/order checkers, the
background-curator checker/disposition, route generator comments, two validation records, and source
comments that cited the archived originals. These are reference-only edits; no runtime behavior changes.

## Review record

The intent classification was reviewed by the `documentation` roundtable in run
`roundtable-de0fb8de6c6e17307226169d` (3/3 seats, converged). Its blocking ruling—update stale but
still-live proposals in place rather than moving them—and its evidence-ledger requirement are encoded
above. Frozen review `roundtable-c103f6e71d7aa11c9109614d` correctly requested reciprocal archive
links and checker exit evidence, which are now present; its apparent missing-file blockers were caused
by a review packet built from `git diff --stat`, which omits untracked move destinations and residuals.
Closure review `roundtable-f70d41be60116af007a68a2e` ruled that a regressed removal proposal cannot
be archived while its required checker fails. The proposal was restored to pending and updated in
place. Long-running closure review `oprun_g6a6608f115294ad8_1785071857_1` then converged with one
blocking documentation finding: this paragraph still described the earlier five-minute API timeout
and labeled the artifact unratified. That stale process-state statement is removed here; the
classification, manifest, and verification results received no further finding. This exact corrected
artifact was approved with no findings by final roundtable run
`oprun_g6a6608f115294ad8_1785072239_2` (artifact
`5270345b9d1f3dbe807e47500077f12130a9586d827d9737f708cf0148b7f39f`; 3/3 participants,
converged, not degraded, no deadline hit).
