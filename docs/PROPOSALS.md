# Proposals

This index reflects the actual proposal tree under
[`docs/proposals/`](proposals/). The authoritative state is the directory
listing itself; this file is a navigable summary of it. Ordering within a
section does not imply global priority.

| State | Folder | Count |
| --- | --- | --- |
| Shipped | [`proposals/done/`](proposals/done/) | 209 |
| Pending | [`proposals/pending/`](proposals/pending/) | 57 |
| Accepted (locked, unimplemented) | `proposals/accepted/` | 0 |
| Deferred | `proposals/deferred/` | 0 |
| Rejected | `proposals/rejected/` | 0 |
| Review notes | `proposals/reviews/` | 0 |

> **Reconciliation note (2026-07-07).** A prior version of this index described a
> ~40-item "charter-spine" roadmap (curator, corpus stages, cross-source learning,
> statistical decision systems, MDL synthesis, Bayesian calibration, contextual
> bandits, planning/MCTS/SMT, the skills library, the hermes-agent intake) and
> claimed `done/` held 17 proposals. An audit confirmed that roadmap was **real and
> is now shipped in code** — its proposal files were removed from the tree and the
> index was never regenerated, so it drifted into describing an empty `accepted/`
> folder and undercounting `done/` by ~48. This rewrite restores the index to the
> tree. Orphaned design docs cited by shipped code are tracked under
> [Documentation integrity](#documentation-integrity).

## Governing contracts

Three platform-wide contracts govern every intelligence-surface change. They are
realized in code and enforced in review; their standalone proposal documents are
**not currently in the tree** (see [Documentation integrity](#documentation-integrity)):

1. **Architecture Charter** — fixes role division across neural, symbolic,
   statistical, planning, and deterministic passes (Recall / Rerank / Rewrite /
   Extract / Synthesize / Judge / Reflect / Classify-Score / Plan-Search / Reason /
   Rank-Fuse / Calibrate / Detect-Cluster / Constrain-Verify / Evaluate-Optimize /
   Gate-Promote / Enforce), one artifact schema, one audit schema, the sidecar
   protocol, and evaluation/calibration discipline. Any new intelligence-surface
   proposal must name its charter role(s).
2. **Two-DB Split** — DB1 (SQLite, local user store) and DB2 (Postgres, shared
   knowledge store) with a tier-pinned Data Access Layer; the boundary is
   compile-enforced. The vector tier lives **inside DB2** as a pgvector extension
   (folded in from a former Qdrant sidecar in #1575) — vectors share the same
   connection and transaction domain as the rows they embed. See
   [`STORAGE_TIERS.md`](STORAGE_TIERS.md).
3. **Memory Public Contract** — the caller-facing memory contract (scope, filters,
   typed mutation verbs, profile packs, stable `--explain`) shared across CLI, MCP,
   and the aimee-kb `/v1/` API.

## Pending (57)

The complete 2026-07-26 reconciliation, including source/test/commit evidence for all 79 original files, is in [`PENDING_AUDIT_2026-07-26.md`](proposals/PENDING_AUDIT_2026-07-26.md). The directory listing remains authoritative.

- [Proposal: Agentic supervised SWE-bench — a true, tool-using, Reddit-parity claim](proposals/pending/agentic-supervised-swebench.md)
- [Proposal: define Aimee's required core capability contract](proposals/pending/aimee-core-capability-contract.md)
- [Proposal: appliance state-recovery runbook](proposals/pending/appliance-state-recovery-runbook.md)
- [Proposal: Document proposal-trigger blob deduplication](proposals/pending/automatic-wfe-trigger-blob-dedup-runbook.md)
- [Proposal: Effective agent tool scoping through aimee's existing toolset seams](proposals/pending/capability-scoped-agent-execution.md)
- [Capability-thresholded delegate routing: residual work](proposals/pending/capability-thresholded-delegate-routing-residual.md)
- [Proposal: the code graph should carry the architecture, not just the symbols](proposals/pending/code-graph-architecture-surface.md)
- [Compaction quality: committed baseline](proposals/pending/compaction-quality-baseline.md)
- [Config descriptor table: generic save residual](proposals/pending/config-field-descriptor-save-residual.md)
- [Core substrate and module boundaries: residual work](proposals/pending/core-substrate-and-source-module-boundaries-residual.md)
- [Proposal: a dedicated extraction model for the curator Tier-A](proposals/pending/dedicated-extraction-model-curator-tier-a.md)
- [Proposal: Delegate sandbox — aimee-server as the sole egress](proposals/pending/delegate-sandbox-aimee-sole-egress.md)
- [Proposal: Per-project delegate sandbox image customization](proposals/pending/delegate-sandbox-image-customization.md)
- [Dynamic tool egress: authenticated registration identity](proposals/pending/dynamic-tool-egress-registration-identity.md)
- [Proposal: govern and capture the module event bus as one uniform seam](proposals/pending/event-bus-governance-and-capture.md)
- [Spec: Aimee shared-memory event bus — wire and segment specification (v0)](proposals/pending/event-bus-wire-spec.md)
- [Proposal: audit feature liveness and remove the background skill curator](proposals/pending/feature-liveness-and-background-curator-removal.md)
- [Proposal: front-end development module — runtime UI verification and design/visual QA](proposals/pending/frontend-development-module.md)
- [Proposal: unify embedding + Tier-A synth on one Gemma-4 base; a dedicated EuroBERT reranker](proposals/pending/gemma4-unified-embed-rerank-synth-base.md)
- [Git core contract: runtime adoption residual](proposals/pending/git-core-contract-runtime-residual.md)
- [Proposal: Per-agent identity, delegation chains, fleet registry, and signed executable artifacts](proposals/pending/governance-agent-identity-and-artifact-trust.md)
- [Proposal: Attestable enforcement — complete the WORM trust anchor and make every verdict provable](proposals/pending/governance-attestable-enforcement.md)
- [Proposal: One governance policy surface — posture profiles, gate completion, and oversight defaults](proposals/pending/governance-policy-surface-and-posture.md)
- [IR sole path: response and legacy-path residual](proposals/pending/ir-sole-path-residual.md)
- [KB hybrid outcome wiring: residual work](proposals/pending/kb-hybrid-outcome-wiring-residual.md)
- [KB ingest: content-push, default-tree indexing, and monotonic delta ordering](proposals/pending/kb-ingest-content-push-deltas.md)
- [Proposal: deliver the modular refactor safely and measurably](proposals/pending/large-refactor-delivery-and-compatibility.md)
- [Proposal: Local-first memory & trust patterns — concepts to adopt](proposals/pending/local-first-memory-and-trust-patterns.md)
- [MCP adapter: general bus routing residual](proposals/pending/mcp-adapter-bus-routing-residual.md)
- [Proposal: Memory auto-population — feedback→rules, promotion, gated extraction (Proposal 2 Phase 4)](proposals/pending/memory-auto-population-phase4.md)
- [Proposal: unify memory, learning, skills, and inference boundaries](proposals/pending/memory-learning-and-inference-boundaries.md)
- [Proposal: `module-loader` — load and host external and user-authored modules](proposals/pending/module-loader.md)
- [Module runtime ownership and build: residual work](proposals/pending/module-runtime-source-ownership-and-build-residual.md)
- [mTLS transport performance: rollout evidence](proposals/pending/mtls-transport-rollout-evidence.md)
- [Operator audit activity: unified surface residual](proposals/pending/operator-audit-activity-residual.md)
- [Proposal: Org-data connectors — the source-ingestion on-ramp for the every-domain KB](proposals/pending/org-data-connectors-and-source-ingestion.md)
- [Per-query ranking feature persistence](proposals/pending/per-query-feature-persistence-residual.md)
- [Proposal: Per-user `remote_writes` authorization](proposals/pending/per-user-remote-writes-authz.md)
- [Persona-authored outputs: residual work](proposals/pending/persona-authored-outputs-residual.md)
- [Proposal: split Runtime and Control Plane governance, web modules, and config surfaces](proposals/pending/product-governance-web-and-config.md)
- [Proposal: Evidence provenance-tier contract — classify + gate Tier-3 (untrusted) memory as an anti-poisoning defense](proposals/pending/proposal-evidence-provenance-tiers.md)
- [Proposal: Binding retrieval context-contract for agents + a survey of context-engine ideas](proposals/pending/proposal-retrieval-context-contract.md)
- [Proposal: Proposal-supersession hygiene — same-commit move + a documented rule](proposals/pending/proposal-supersession-hygiene.md)
- [Remote session start: workspace context residual](proposals/pending/remote-session-start-workspace-context.md)
- [Route descriptor single source: residual work](proposals/pending/route-descriptor-single-source-of-truth-residual.md)
- [Search egress policy: separate untrusted destinations from operator-configured endpoints](proposals/pending/search-egress-policy-split.md)
- [Proposal: Standing LoCoMo / LongMemEval benchmark cadence](proposals/pending/standing-benchmark-cadence.md)
- [Proposal: the registration chain and the static thin client](proposals/pending/thin-client-capability-advertisement.md)
- [Tiered LLM offering: remaining program scope](proposals/pending/tiered-llm-offering-residual.md)
- [P2b residual: KB forwarding and true streaming](proposals/pending/tiered-llm-p2b-forwarding-and-streaming.md)
- [P6 residual: native InvokeModel families and pricing](proposals/pending/tiered-llm-p6-native-invokemodel-and-pricing.md)
- [P9 residual: telemetry forwarding and OTLP](proposals/pending/tiered-llm-p9-forwarding-and-otlp.md)
- [Turn-scoped change sets and safe workspace restore](proposals/pending/transactional-turn-rewind-and-session-recovery.md)
- [User-selectable fusion: surface and policy residual](proposals/pending/user-selectable-fusion-surface-residual.md)
- [Proposal: Close out platform phase 7 — v1 API stability tag + distributed-mode validation](proposals/pending/v1-stability-and-distributed-validation.md)
- [Design brief: multi-engine fanout, circuit-breaking, provenance, accounting](proposals/pending/web-search-fanout-resilience-accounting.md)
- [Proposal: webchat project lifecycle — org-scoped clones and true delete/purge](proposals/pending/webchat-project-lifecycle.md)

## Done (209)

The [`proposals/done/`](proposals/done/) directory holds 209 shipped proposals.
Grouped by theme:

- **Universal gateway, ingress & protocol.**
  [universal LLM gateway](proposals/done/aimee-universal-gateway.md),
  [canonical IR](proposals/done/aimee-canonical-ir.md),
  [Anthropic ingress `/v1/messages`](proposals/done/anthropic-ingress.md),
  [Codex ingress `/v1/responses`](proposals/done/codex-frontend-ingress.md),
  [context pre-injection + confidence-gated retrieval for ingresses](proposals/done/context-preinjection-ingress.md),
  [envelope compression + cache-prefix alignment](proposals/done/ingress-compression-and-cache-alignment.md),
  [ingress cost accounting + request optimizations](proposals/done/ingress-cost-accounting-and-optimizations.md),
  [`/v1` dispatch migration](proposals/done/v1-dispatch-migration.md) (+ [finish](proposals/done/v1-dispatch-migration-finish.md)),
  [server-owned turn lifecycle](proposals/done/server-owned-turn-lifecycle.md).
- **Economizer & context reduction.**
  [gateway mutation / primary-agent context reduction](proposals/done/economizer-gateway-mutation.md),
  [unified economizer with two-tier safety](proposals/done/unified-economizer-two-tier-safety.md),
  [deterministic context folding](proposals/done/deterministic-context-folding.md),
  [deterministic tool-output condensation](proposals/done/deterministic-tool-output-condensation.md),
  [optimization surface](proposals/done/optimization-surface.md) (+ [residual](proposals/done/optimization-surface-residual.md)).
- **Thin client, server, TLS & credentials.**
  [self-sufficient thin client](proposals/done/self-sufficient-thin-client.md) (+ [data plane](proposals/done/self-sufficient-thin-client-data-plane.md)),
  [native TLS thin-client backends](proposals/done/native-tls-thin-client-backends.md),
  [mTLS client identity](proposals/done/mtls-client-identity.md),
  [server-hosted OAuth CLI agents](proposals/done/server-hosted-oauth-cli-agents.md),
  [auto vault provisioning at server standup](proposals/done/auto-vault-provisioning-at-server-standup.md),
  [credential-vault consolidation](proposals/done/cred-vault-consolidation.md),
  [delegate refactor — async + credential vault](proposals/done/delegate-refactor-async-and-credential-vault.md),
  [live config reload](proposals/done/live-config-reload.md).
- **Autonomous development, workflows & delegation.**
  [autonomous-dev execution substrate](proposals/done/autonomous-dev-execution-substrate.md),
  [full autonomous development](proposals/done/full-autonomous-development.md),
  [aimee workflows — autonomy-first dev engine](proposals/done/aimee-dev-lifecycle-workflow.md),
  [config-extensible workflow blocks](proposals/done/workflow-config-blocks.md),
  [primary-as-manager enforced workflows](proposals/done/primary-as-manager-enforced-workflows.md),
  [on-demand delegate execution](proposals/done/delegate-ondemand-execution.md),
  [four-part harness taxonomy](proposals/done/four-part-harness-taxonomy.md).
- **Roundtable, review & verification.**
  [agent roundtable — collaborative drafting](proposals/done/agent-roundtable-collaborative-drafting.md)
  (+ [residual](proposals/done/agent-roundtable-collaborative-drafting-residual.md),
  [authoring pipeline](proposals/done/agent-roundtable-authoring-pipeline.md)),
  [agent-directed PR review](proposals/done/agent-directed-pr-review.md),
  [roundtable panel composition](proposals/done/roundtable-panel-composition.md),
  [roundtable reliability](proposals/done/roundtable-reliability.md),
  [replayable-evidence verification + deepening sweep](proposals/done/replayable-verification-and-deepening-sweep.md).
- **Code-graph intelligence.**
  [code-graph intelligence](proposals/done/code-graph-intelligence.md),
  [graph-derived code-health audit](proposals/done/code-health-audit.md),
  [cross-repo dependency graph](proposals/done/cross-repo-dependency-graph.md)
  (+ [precision hardening](proposals/done/cross-repo-precision-hardening.md),
  [recall recovery](proposals/done/cross-repo-recall-recovery.md)),
  [C++ class/method extraction](proposals/done/cpp-class-method-extraction.md),
  [CSS migration assistant](proposals/done/css-migration-assistant.md),
  [graph feedback — self-audit + learning](proposals/done/graph-feedback-self-audit-and-learning.md).
- **Knowledge base, curator & retrieval.**
  [auditable correctness for the KB](proposals/done/auditable-correctness-for-the-kb.md),
  [pluggable curator LLM backend](proposals/done/curator-llm-backend.md),
  [aimee-kb LLM endpoints + default CPU container](proposals/done/kb-llm-endpoints-and-default-cpu.md),
  [aimee-kb web console](proposals/done/kb-web-console.md),
  [embedder runtime fetch + auto-dimension](proposals/done/embedder-runtime-fetch-autodim.md),
  [ingest restoration + recall contract](proposals/done/ingest-restoration-and-recall-contract.md),
  [recall economy progressive disclosure](proposals/done/recall-economy-progressive-disclosure.md),
  [recall abstention confidence gate](proposals/done/retrieval-abstention-confidence-gate.md),
  [typed-fact knowledge layer](proposals/done/typed-fact-knowledge-layer.md),
  [LLM-sidecar productionization — curator extraction + idle reflection](proposals/done/llm-sidecar-productionization-curator-and-reflection.md),
  [generalise the `memory.benchmark` RPC](proposals/done/memory-benchmark-suite-generalisation.md).
- **Structured PDF & evidence.**
  [structured PDF ingestion + coordinate-anchored evidence](proposals/done/structured-pdf-ingestion-and-evidence-layer.md),
  [structured-PDF tables → typed facts, visual evidence, OCR](proposals/done/structured-pdf-tables-visual-and-ocr.md).
- **Governance, audit & memory interception.**
  [governance — decision records + per-action policy-verdict audit](proposals/done/governance-decision-records-and-action-audit.md),
  [per-service auditable WORM metrics/logs store](proposals/done/auditable-worm-audit-store.md),
  [central agent-memory interception](proposals/done/central-agent-memory-interception.md).
- **LLM container & UI.**
  [one unified `aimee-llm` container](proposals/done/unified-llm-container.md),
  [webchat git projects + in-browser VSCode](proposals/done/webchat-git-projects-and-vscode.md),
  [dedicated Proposals web page](proposals/done/proposals-ui-page.md).

For the full chronological record, see the directory listing or
`git log -- docs/proposals/done/`.

## Documentation integrity

Cleanup surfaced while regenerating this index:

- **`three-db-split` fully purged (this change).** The former three-tier framing
  (DB1 / DB2 / DB3-Qdrant) is gone: the vector tier is a pgvector extension inside
  DB2, full stop. All `docs/proposals/{accepted,pending,done}/three-db-*.md`
  citations and `DB3` / `db3` mentions in source comments and docs were repointed
  to [`STORAGE_TIERS.md`](STORAGE_TIERS.md) or reworded to the DB1/DB2 vocabulary.
- **Empty state folders.** `accepted/`, `deferred/`, `rejected/`, and `reviews/`
  contain no proposals (only `.gitkeep`). Prior index prose describing rejected /
  deferred items and an Accepted queue has been removed as unbacked.
- **Orphaned design docs cited by shipped, verified code.** Source headers cite
  `docs/proposals/accepted/*.md` documents absent from the tree, even though the
  code they describe is demonstrably shipped and tested. Verified examples:
  - **Contextual bandits** — `db2/bandit.c` + `kb/kb_bandit.c` (789 LOC),
    `bandit_decisions` table (`db2/schema.sql`), `test_bandit.c` +
    `tools/bandit_replay.py`, endpoints `/v1/intelligence/bandit/{export,replay-record,sample}`.
  - **Statistical decision systems** — `kb_ranker.c`, `kb_features.c`,
    `kb_detect.c`, `db2/feature_rows.c`, `kb/kb_ranker_fit.c` + `scripts/rank-fit.py`
    (the [LTR weight-fitting proposal](proposals/done/learning-to-rank-weight-fitting.md)
    shipped the Calibrate half — fitter + benchmark gate, default-off, bench-only
    until the outcome-wiring prerequisite).
  - **Graph reasoning / case recall** — `kb_reasoning.c` (474 LOC), `db2/cases.sql`.
  - **MDL-guided synthesis** — `kb_mdl.c` (239 LOC).
  - **Deliberate planning** — `kb_planner.c` + `scripts/mcts-planner.py`.
  - Plus `neural-assisted-guardrails`, `ingest-poison-gate` (`integrity_gate.c`),
    `prompt-cache-aware-deferred-payload-rewrite`, `memory-public-contract`
    (`memory_effective.c`), `aimee-kb-service-and-public-api`,
    `aimee-unified-presence`, and the placeholder `example.md` (cited by two tests).
  - *Stale-but-present:* `agent-roundtable-authoring-pipeline.md` exists in `done/`;
    its `See docs/proposals/accepted/…` header just needs repointing to `done/`.
  - Recommended follow-up: restore the orphaned design docs to `done/` as post-hoc
    records of shipped work, or replace each header comment with the shipped
    filename. Tracked separately from this index refresh.

## Notes

- The pending list is small by design. Proposals that ship move to `done/`.
- The Architecture Charter is the review gate for any new intelligence-surface
  proposal (neural, symbolic, statistical, planning, or deterministic): it must
  name its charter role(s). The Memory Public Contract is the review gate for any
  new caller-facing memory surface (CLI flag, MCP tool shape, or HTTP endpoint).
