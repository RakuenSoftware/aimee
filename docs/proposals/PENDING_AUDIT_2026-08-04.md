# Pending proposal audit — 2026-08-04

This is the exhaustive reconciliation of the 69 Markdown proposals in
`docs/proposals/pending/` at `testing` commit `5b203df06`. It reuses the evidence from the
2026-07-26 audit, then inspects every proposal added or materially affected by merged work since that
snapshot. In particular, it checks the 62-slice source modularization, event-bus promotion, workflow
reliability fixes, per-user writes, embedder/synthesis topology, and current config/graph behavior.

The machine-verifiable partition is
[`PENDING_AUDIT_2026-08-04.tsv`](PENDING_AUDIT_2026-08-04.tsv): one row per original, with its final
path and any residual. After reconciliation and the workflow proof there are 4 complete originals,
20 partially delivered originals, 45 still-accurate pending originals, 19 new residuals, and 64
final pending proposals.

## Decision rules

1. A proposal stays pending only when its own stated deliverable remains coherent and unshipped.
2. A fully shipped or superseded proposal moves to `done/` with an explicit terminal state.
3. A partly shipped proposal also moves to `done/`; only its independently testable remainder returns
   to `pending/` as a new residual with reciprocal links.
4. Existing foundations described by a proposal do not make the proposal partial. At least one of
   the proposal's requested deltas must have shipped.
5. Evidence is taken from the `testing` tree and merged history, not from feature-branch intent.

## Complete originals moved to done (4)

| Original | Completion evidence |
|---|---|
| `appliance-state-recovery-runbook.md` | Workflow `wi_36d92dad5082a081952bac50d0aaf4bb` delivered the runbook, passed implementation, acceptance, documentation, and documentation-review gates, and opened draft PR #2329 against `testing` for human review. |
| `wizard-first-user-bootstrap.md` | The first-user wizard, persisted bootstrap state, validation, and live proof are implemented; the proposal itself recorded ready/validated status. |
| `per-user-writes-operator-cli-spec.md` | Subject/team/tier grant functions, server and KB routes, CLI commands/help/rendering, unit coverage, and live validation are present. |
| `embedder-query-document-prefixes.md` | The registry owns query/document prefixes; every C embed call declares polarity; request `input_type` and default-document behavior are covered by tests. |

## Partially delivered originals archived with residuals (20)

| Archived original(s) | Delivered evidence | New residual |
|---|---|---|
| `per-user-remote-writes-authz.md`, `per-user-remote-writes-authz.acceptance.md` | OIDC/PAM identity, grants, KB-signed single-use tokens, write enforcement, operator CLI, and individual live rigs are delivered. One production-shaped PAM→mint→write run, vault-custodied signing, hardened-tier proof, and live JWKS rotation/failure cases remain. | `per-user-remote-writes-deployment-validation.md` |
| `feature-liveness-and-background-curator-removal.md` | The background curator removal is restored and its absence checker passes. The suite-wide dead-feature inventory was not performed. | `feature-liveness-inventory-residual.md` |
| `config-t-encapsulation.md` | Production code is at zero direct `config_t` use and the ratchet holds. Tests still contain 241 mentions and 149 direct `config_load()` calls. | `config-t-test-and-accessor-safety-residual.md` |
| `config-single-source-of-truth-audit.md` | The falsey-antipattern bypass is fixed and tested. Cache TTL ownership and several env/autonomy projection surfaces remain split. | `config-authority-surface-residual.md` |
| `synthesis-sidecar-and-embedder-axis.md` | mTLS synthesis sidecar, one KB axis, no KB embedder, wizard integration, and identity paths are merged. The promised production throughput comparison is not recorded. | `synthesis-sidecar-throughput-validation.md` |
| `embedder-image-split-and-rebuild.md` | A dedicated image/publish path, mTLS proof, KB-issued identity, model-change replay, and drift guard landed. Managed topology still keeps the in-process embedder and lacks a same-dimension migration proof. | `embedder-sidecar-deployment-and-migration-residual.md` |
| `event-bus-wire-spec.md` | The v0 wire, C host/client, Go client, arena routing, capture, conformance vectors, and performance baseline are on `testing`. The proposal explicitly left its third-language proof unwritten. | `event-bus-third-language-conformance.md` |
| `learning-to-rank-from-interactions.md` | Serving rows, outcome capture, fitter, and promotion gate are implemented. Default-on labels, full candidate-pool logging, and true propensity/IPW activation remain. | `learning-to-rank-activation-and-ipw-residual.md` |
| `wfe-panel-cannot-seat-under-self-load.md` | Active delegate occupancy is accounted for and idle/all-saturated/unreported cases are tested. Deadline and production health/capacity behavior remain. | `wfe-panel-capacity-residual.md` |
| `wfe-slices-conflict-on-shared-file.md` | Slices branch from and reintegrate the remote feature tip, with terminal conflict handling. The proposed frozen sibling-diff collision check is absent. | `wfe-sibling-freeze-collision-residual.md` |
| `core-substrate-and-source-module-boundaries-residual.md` | Descriptor catalog, ownership-complete source moves, dependency checks, event wire, reference clients, and conformance foundation landed. Separate program boundaries and rollout remain. | `core-process-separation-residual.md` |
| `module-runtime-source-ownership-and-build-residual.md` | All 25 descriptors validate and declare complete ownership; dependency checks pass with one tracked legacy-root exception. Descriptors do not yet author both builds or packaging. | `descriptor-authoritative-build-residual.md` |
| `aimee-core-capability-contract.md` | Capability taxonomy, descriptors, source carving, stage vocabulary, and initial event adapters landed. The canonical cross-process MCP/ACP round trip is not implemented. | `aimee-core-cross-process-contract-residual.md` |
| `memory-learning-and-inference-boundaries.md` | Memory, learning, skills, response-composition, and synthesis ownership/provider seams exist. Separate-program bus isolation and remaining readiness/semantic contracts do not. | `memory-learning-cross-process-boundaries-residual.md` |
| `product-governance-web-and-config.md` | Runtime/control web ownership and initial decision surfaces landed. Naming/packaging, separate processes, optional lifecycle, and effective-config parity remain. | `runtime-control-product-boundary-residual.md` |
| `large-refactor-delivery-and-compatibility.md` | The 62 source slices, inventories, cleanup/ownership records, validation, and bus foundation merged. Profile parity, process isolation, recovery, and compatibility closeout remain. | `modular-refactor-completion-residual.md` |
| `git-core-contract-runtime-residual.md` | Git source ownership, descriptor, public contract, caller migration, and core behavior tests landed. Cross-process events and deployment telemetry/rollback remain. | `git-core-cross-process-rollout-residual.md` |
| `event-bus-governance-and-capture.md` | Capture/replay and audit, guardrail, memory, tool, vault, and sandbox sinks landed. Structural all-event capture, synchronous action verdicts, and attestation integration remain. | `event-bus-enforcement-and-attestation-residual.md` |
| `code-graph-architecture-surface.md` | Project lifecycle, graph diff, provenance/confidence, task-conditioned context, and language-aware blast radius landed. Route/storage nodes and repository orientation did not. | `code-graph-route-storage-orientation-residual.md` |

## Accurate proposals left pending (45)

| Proposal | Current live gap |
|---|---|
| `agentic-supervised-swebench.md` | Design-only; no supervised agentic benchmark harness. |
| `capability-scoped-agent-execution.md` | Resolve-once execution scope and removal of legacy unbound dispatch are absent. |
| `capability-thresholded-delegate-routing-residual.md` | Threshold calibration and rollout residual remains unimplemented. |
| `code-intelligence-language-coverage.md` | Language suffix/parser policy is still duplicated and omits the named coverage. |
| `compaction-quality-baseline.md` | No checked-in agentic quality baseline. |
| `config-field-descriptor-save-residual.md` | Save/generation authority remains outside the descriptor table. |
| `corpus-pipeline-has-no-driver.md` | No scheduled driver; the CLI path remains unreachable through the server. |
| `dedicated-extraction-model-curator-tier-a.md` | Design-only; no dedicated extraction-model tier/evaluation. |
| `delegate-budget-must-fit-its-stage-cap.md` | Delegate tool-loop and stage wall caps remain unreconciled and poorly diagnosed. |
| `delegate-sandbox-aimee-sole-egress.md` | Direct tool/backend egress bypass remains. |
| `delegate-sandbox-image-customization.md` | No complete per-project image resolution/build/cache policy. |
| `dynamic-tool-egress-registration-identity.md` | Registration identity and capability binding remain open. |
| `frontend-development-module.md` | Browser executor, containment, and visual-QA slices are absent. |
| `gemma4-unified-embed-rerank-synth-base.md` | Design-only model/evaluation change. |
| `governance-agent-identity-and-artifact-trust.md` | Per-agent principals, delegation chain, fleet registry, and artifact gates remain open. |
| `governance-attestable-enforcement.md` | WORM is still default-off; policy revisions, capture completeness, external anchor, and `audit attest` are absent. Bus sinks alone do not satisfy these deltas. |
| `governance-policy-surface-and-posture.md` | Profiles, complete ingress integrity gating, approval verdict, and containment defaults remain open. |
| `ir-sole-path-residual.md` | Legacy builders/raw paths remain outside the canonical IR seam. |
| `kb-hybrid-outcome-wiring-residual.md` | Remaining hybrid outcome activation/proof is absent. |
| `kb-ingest-content-push-deltas.md` | Content identity, monotonic ordering, force-push, and operator controls remain a coherent slice. |
| `local-first-memory-and-trust-patterns.md` | Proposed quarantine/codec/owner-gated deltas remain distinct from existing foundations. |
| `mcp-adapter-bus-routing-residual.md` | MCP invocation is not hosted through the module bus. |
| `memory-auto-population-phase4.md` | Feedback-to-rules, quarantine extraction, and promotion gates are not implemented. |
| `module-loader.md` | No trusted external/user-authored module loader or its sandbox/artifact checks. |
| `mtls-transport-rollout-evidence.md` | Operational cohorts and default promotion evidence remain. |
| `operator-audit-activity-residual.md` | Remaining operator surface/export residual remains. |
| `org-data-connectors-and-source-ingestion.md` | Design-only; no connector adapters or incremental supersession system. |
| `per-query-feature-persistence-residual.md` | Feature rows still lack the required durable per-query grouping proof. |
| `persona-authored-outputs-residual.md` | Permission, voice, and commit-style portions remain. |
| `proposal-evidence-provenance-tiers.md` | Tier-3 classification and fail-closed write gate are absent. |
| `proposal-retrieval-context-contract.md` | Binding retrieval-context contract and Tier-C surfaces are absent. |
| `proposal-supersession-hygiene.md` | Same-commit lifecycle enforcement and one-time sweep remain. |
| `remote-session-start-workspace-context.md` | Workspace context residual remains beyond the shipped remote brief. |
| `route-descriptor-single-source-of-truth-residual.md` | Remaining descriptor authority/generation surfaces are unimplemented. |
| `standing-benchmark-cadence.md` | No scheduled workflow, retained result store, or drift gate. |
| `thin-client-capability-advertisement.md` | Registration-chain advertisement and non-disclosure checks are absent. |
| `tiered-llm-offering-residual.md` | The already-narrowed remaining tier program is not complete. |
| `tiered-llm-p2b-forwarding-and-streaming.md` | Forwarding and true streaming remain. |
| `tiered-llm-p6-native-invokemodel-and-pricing.md` | Native InvokeModel and pricing breadth remain. |
| `tiered-llm-p9-forwarding-and-otlp.md` | Forwarding and OTLP export remain. |
| `transactional-turn-rewind-and-session-recovery.md` | No sealed turn-scoped change-set lifecycle and safe restore contract. |
| `user-selectable-fusion-surface-residual.md` | Remaining public surface/control work is absent. |
| `v1-stability-and-distributed-validation.md` | Stability snapshot and distributed concurrency/isolation harness are absent. |
| `web-search-fanout-resilience-accounting.md` | Default install path, fanout, circuit, provenance, and accounting decisions remain open. |
| `webchat-project-lifecycle.md` | Org-scoped clone migration and true delete/purge lifecycle remain. |

## Validation and release-hygiene fix

The old checker hard-coded July's filename and 79-row count. That made a valid later lifecycle move
look like repository corruption and caused current `testing` to fail on a proposal already archived
by a later workflow. The checker now discovers the newest strictly dated manifest, derives its row
count, accepts either the legacy roundtable field or a general review record, and still proves exact
pending-set equality, archive/residual reciprocity, explicit states, and evidence anchors. Historical
manifests remain immutable evidence rather than mutable declarations of current state.

Proposal links, proposal-state reconciliation, background-curator absence, descriptor schema, source
ownership, config encapsulation, and the latest manifest are the mechanical closeout gates for this
audit.
