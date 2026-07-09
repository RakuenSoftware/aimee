# Proposals

This index reflects the actual proposal tree under
[`docs/proposals/`](proposals/). The authoritative state is the directory
listing itself; this file is a navigable summary of it. Ordering within a
section does not imply global priority.

| State | Folder | Count |
| --- | --- | --- |
| Shipped | [`proposals/done/`](proposals/done/) | 65 |
| Pending | [`proposals/pending/`](proposals/pending/) | 11 |
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

## Pending

The genuinely open work — eleven proposals (all but one not yet implemented).

- [kb_hybrid outcome wiring](proposals/pending/kb-hybrid-outcome-wiring.md)
  — closes the learning-to-rank loop on live data. B1 (the loop-closing plumbing:
  a dedicated `ranker_outcome` kind, `ranker.emit_event` / `ranker.record_outcome`
  KB-service endpoints mirroring the memory evidence pattern, and the fitter's
  training view reading it) is implemented with zero `kb.c` hot-path change; B2 (a
  production outcome source for code-search) is the remaining open work. Follow-up
  to the done LTR fitter. **Calibrate / Evaluate-Optimize / Gate-Promote.**
- [Agentic supervised SWE-bench](proposals/pending/agentic-supervised-swebench.md)
  — a true tool-using, iterating agentic SWE-bench harness so the "beats Reddit's
  −75.5% supervisor-token reduction at no wall-clock penalty" claim is
  apples-to-apples; official Docker grader as the sole resolution source; a
  public-claim gate that fails closed (issue #987, builds on PR #986).
  **Reason / Execute / Persist / Calibrate / Review.**
- [Remote-first session-start](proposals/pending/remote-first-session-start.md)
  — the thin client's SessionStart falls back to a recall-only remote path and
  emits nothing when recall is empty; make `/v1/hooks/session_start` first-class so
  a thin client gets the full server-assembled brief. Companion to the memory split.
- [LLM-sidecar productionization — curator extraction + idle reflection](proposals/pending/llm-sidecar-productionization-curator-and-reflection.md)
  — two intelligence steps ship as full scaffolding but stub the LLM call: curator
  extraction (all stages present; only the Phase-0 embedding sidecar exists behind
  `kb_curator_sidecar`) and the idle-reflection scheduler (`kb_reflection.c` runs
  fully; its own header notes LLM candidate generation is stubbed). Graduate both
  onto one versioned sidecar contract behind a shadow → canary → default gate on the
  shipped calibration + bandit rails. **Extract / Synthesize / Judge / Reflect /
  Gate-Promote.**
- [Org-data connectors + source ingestion](proposals/pending/org-data-connectors-and-source-ingestion.md)
  — the missing ingest front door for the every-domain KB: a uniform connector
  contract plus a first adapter set (issue tracker / chat / doc-wiki / email),
  incremental sync with supersession, and ingest-time auth + scope + PII/poison
  enforcement, all feeding the existing Normalize → staged-pipeline → curator path.
  **Extract (Normalize) / Classify-Score / Enforce / Gate-Promote.**
- [First-class operator-audit activity surface](proposals/pending/operator-audit-activity-surface.md)
  — every shareable DB2 row already carries `operator_id` / `content_hash` /
  timestamps and a WORM ledger records privileged actions, but there is no legible
  way to read "who did what, in which scope, when"; add an operator-facing audit
  activity surface over the existing provenance.
- [Proposal-supersession hygiene](proposals/pending/proposal-supersession-hygiene.md)
  — `pending/` is only signal if finished or superseded proposals leave it; adds a
  same-commit move convention plus a documented supersession rule and the reconcile
  drift class to enforce it.
- [Standing LoCoMo / LongMemEval benchmark cadence](proposals/pending/standing-benchmark-cadence.md)
  — acceptance criteria cite absolute retrieval/memory parity numbers but nothing
  runs the full benchmarks on a schedule; adds a standing benchmark cadence beyond
  the PR-only `bench-smoke`.
- [Close out platform phase 7 — v1 API stability tag + distributed-mode validation](proposals/pending/v1-stability-and-distributed-validation.md)
  — the aimee-kb platform arc landed phases 1–6; phase 7 (distributed-mode
  validation + a v1 API stability tag) is the one remaining piece with no closing
  artifact.
- [Binding retrieval context-contract for agents](proposals/pending/proposal-retrieval-context-contract.md)
  — surveys an external context-engine against Aimee (most of its mechanisms
  already exist: attention guard, per-intent budgets, confidence scorer, symbol
  preload) and scopes the one clean gap: surface the confidence + caps the memory
  assembler already computes to the delegate as a *binding* exploration contract,
  enforced by the existing `cli_attention_guard.c` raw-scan redirect.
  **Recall / Rank-Fuse / Calibrate / Plan-Search / Enforce / Gate-Promote.**

## Done (66)

The [`proposals/done/`](proposals/done/) directory holds 66 shipped proposals.
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
