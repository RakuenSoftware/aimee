# Proposals

This index reflects the current proposal tree under
[`docs/proposals/`](proposals/). The pending section is organized by
the layers of the
Architecture Charter,
which is the single umbrella contract every intelligence-surface
proposal inherits from, whether neural, symbolic, statistical,
planning, or deterministic. Ordering within a layer does not imply a strict global
priority.

For state folders other than `pending/`, see the authoritative
directory listing:

- [`proposals/accepted/`](proposals/accepted/): locked but not yet
  implemented
- [`proposals/done/`](proposals/done/): shipped (17 proposals)
- [`proposals/rejected/`](proposals/rejected/): considered and
  declined
- [`proposals/deferred/`](proposals/deferred/): parked for later
- [`proposals/reviews/`](proposals/reviews/): review notes

## Shared vision

Aimee is converging on a single architecture, written down in one
umbrella document plus two platform-wide contracts.

1. **Architecture Charter.** Fixes role division across neural,
   symbolic, statistical, planning, and deterministic passes
   (Recall / Rerank / Rewrite / Extract / Synthesize / Judge / Reflect /
   Classify-Score / Plan-Search / Reason / Rank-Fuse / Calibrate /
   Detect-Cluster / Constrain-Verify / Evaluate-Optimize /
   Gate-Promote / Enforce), one artifact schema, one audit schema, one
   retroactive-review UX, the sidecar protocol, the service topology,
   versioning, storage allocation, evaluation discipline, and
   calibration discipline. Every intelligence-surface proposal
   inherits from it. A new proposal in that scope that does not name
   its charter role(s) and cite the charter is not ready for review.
2. **Two-DB Split.** The locked DB1 (sqlite) and DB2 (postgres + pgvector)
   storage topology, with a tier-pinned Data Access Layer rule that keeps
   each tier's backend knowledge inside that tier directory. Storage-boundary
   enforcement is the scaffold every other proposal lands on top of. (The
   original split shipped a separate DB3 Qdrant tier; it was folded into
   DB2 as a pgvector extension in #1575.)
3. **Memory Public Contract.** The caller-facing contract (scope,
   filters, typed mutation verbs, profile packs, and stable `--explain`)
   shared identically across CLI, MCP, and the aimee-kb `/v1/` API.

## Pending

### Evidence pipeline: aimee-kb + curator + learning + ingest

These proposals together implement the charter's one data pipeline.
The Document Corpus Intelligence Pipeline
umbrella maps the document side's stages and tables onto this cluster
and routes the genuine gaps to four new siblings. Platform and curator
split the doc + code side; cross-source-learning owns the session /
feedback / workflow side; ingest-lab is the Normalize stage feeding the
curator; approximate-sketches sits between normalization and dense
recall.

- Document Corpus Intelligence Pipeline:
  umbrella for the document side; maps the 13-stage / 15-table corpus
  design onto the charter spine + shipped work; sets the CPU-first
  curator profile and the size-adaptive vector-index strategy.
  Introduces the four siblings below. **Done.**
- aimee-kb Service and Public `/v1/` API:
  service topology; install-today profile picker; OpenAPI v1; SDKs;
  auth; corpus staging and release gating; reflection HTTP surface.
  Phase 1 shipped, `/v1/health`, `/v1/version`, `/v1/capabilities`; bearer-token
  auth; `kb.api.http_port` config; OpenAPI v1 spec. Phases 2+ require dogfood.
- aimee-kb Service Split and Headless Containerization:
  `src/kb/` / `src/server/` source split; Docker packaging; `/v1/`-only
  access (retiring the legacy `kb.*` RPC); headless completeness so a
  remote container is fully consumable from another host. **New.**
- Distributed-Mode Auth: Zero-Config Enrollment and Pluggable Verifier:
  the remote aimee-server ↔ aimee-kb trust boundary the service proposal
  deferred ("distributed-mode auth"); one-string container enrollment
  (CA-pinned TLS + one-time token), automated mTLS with auto-rotation,
  and a pluggable `Verifier` (kb-token default, BYO OIDC/JWT).
  Container-per-owner by default; single-container multi-tenant is
  opt-in. **New.**
- Deep Curator: Doc and Code Extraction:
  Extract / Synthesize / Judge on docs and code; entity graph;
  doc↔code bridge via `implements`; contradictions; synthesis;
  semantic code indexing and search. Runs inside the aimee-kb service.
  **Accepted. Phase 0 shipped, `scripts/embed-minilm.py` MiniLM-L6-v2 sidecar.**
- Cross-Source Learning Pipeline: Candidate Generation, Promotion Rollout, and Review:
  Continuation of the now-done Cross-Source Learning Substrate. Carries the
  model-assisted / infra-bound remainder, candidate generation, pgvector
  neighbourhood retrieval, synthesis, the rest of the promotion surfaces,
  cross-surface review, and version-bump replay. **New.**
- Ingest Lab and Strategy-Aware Chunking:
  Normalize stage; document-kind-aware chunking; operator-facing
  debug surface; quality / staging recommendations for the curator's
  release gate. **Accepted. Phase 0 shipped, `aimee kb lab` command with chunk preview,
  quality signals, and stage recommendations.**
- Ingest Restoration and Bounded-Hallucination Recall Contract:
  restoration-candidate queueing outside `kb_lab`, single-pass
  curator fusion with provenance and `[unknown]`, and per-result
  verbatim vs synthesised recall evidence mode. **Done.**
- Approximate Sketches for Ingest Pre-Filtering:
  Bloom / MinHash-LSH / Count-Min / HyperLogLog layer between
  Normalize and dense recall; cheap dedupe, near-dup clustering,
  distinct-source counts; feeds `sketch.*` features into the feature
  view. **Done.** Shipped, all four algorithms + DB2 persistence,
  Bloom exact skip, MinHash-LSH near-dup skip and supersession proposals,
  Count-Min / HLL `sketch.*` features, and fixture-backed sketch tests.
- Corpus Staged Processing Pipeline:
  per-document resumable stage machine (`corpus_processing_jobs`) that
  conducts the existing per-kind job queues. Fills the design's central
  gap. **Done.** Shipped, DB2 job/event/version tables, doc-ingest
  seeding, deterministic drain handlers, and `aimee kb pipeline`.
- Corpus Structural Analysis:
  stages 1 / 2 / 5, doc-type classification, `document_sections`,
  `document_references` + staleness. **Done.** Shipped, deterministic
  doc classification, Markdown section trees, document references, and
  reference staleness helpers.
- Corpus Terminology and Gap Detection:
  stages 6 / 12, `term_mapping` and `gap` artifact kinds; corpus gaps
  feed the memory-side curiosity surface. **New.**
- Corpus Vector Index Strategy:
  pgvector HNSW by default, pgvectorscale diskann as an opt-in scale-up;
  memory vectors stay HNSW. **New.**
- [LLM-Sidecar Productionization: Curator Extraction and Idle Reflection](proposals/pending/llm-sidecar-productionization-curator-and-reflection.md):
  graduates the two shipped-but-stubbed intelligence steps — curator LLM
  extraction (all stages ship; only the Phase-0 embedding sidecar exists) and
  the idle-reflection scheduler (runs fully; LLM candidate generation stubbed) —
  onto one versioned sidecar contract, behind a shadow → canary → default gate
  on the shipped calibration + bandit rails. Extract / Synthesize / Judge /
  Reflect / Gate-Promote. **New.**
- [Org-Data Connectors and Source Ingestion](proposals/pending/org-data-connectors-and-source-ingestion.md):
  the missing ingest front door for the every-domain KB — a uniform connector
  contract plus a first adapter set (issue tracker / chat / doc-wiki / email),
  incremental sync with supersession, and ingest-time auth + scope + PII/poison
  enforcement, all feeding the shipped Normalize → staged-pipeline → curator
  path. Extract (Normalize) / Classify-Score / Enforce / Gate-Promote. **New.**

### Retrieval (Recall / Rerank / Rank-Fuse)

- Dynamic Alpha Fusion for KB Hybrid Retrieval:
  score-aware lexical / dense blending shipped as an opt-in fusion mode on
  `POST /v1/search`; benchmark gate did not justify a default flip, so `rrf`
  remains default. **Done, bench-only / no rollout.**
- Cross-repo dependency graph:
  precise, confidence-tiered inter-repo dependency edges over the multi-repo
  corpus; corroboration-gated resolver (import/include resolution + export
  downgrade signal), corpus-derived distinctiveness, signature-aware
  multiplicity, AMBIGUOUS review queue; query-first (read API + CLI). Engine
  (S1–S9) shipped; precision + recall hardened via two follow-up proposals;
  `--reverse`/`--dry-run` built; §9 gates reconciled. **Done** (see
  `done/cross-repo-dependency-graph.md`). Extends code-graph intelligence.

### Synthesis tie-break (Synthesize / Calibrate)

- MDL-Guided Synthesis Selection:
  minimum-description-length tie-break over N-attempt-agreed
  synthesis candidates; emits `mdl.*` features; prompt-bump drift
  guardrail.

### Reasoning (Reason / Constrain-Verify / Case Recall)

- Graph Reasoning, Case-Based Recall, and Contradiction Logic:
  Datalog-over-DB2 rule engine; case library as promotion target;
  deterministic temporal / provenance contradiction checks; shared
  symbolic substrate consumed by curator, learning, and guardrails.
  **Done.** All 6 acceptance criteria complete, enforcing contradiction demotion, case recall (composite kNN+Datalog), 3 surfaces wired (deep-curator/learning/guardrails).

### Ranking, Calibration, Detection (Rank-Fuse / Calibrate / Detect-Cluster)

- Statistical Decision Systems for Ranking and Detection:
  shared feature view; learning-to-rank substrate; clustering /
  anomaly / drift detection; emits `drift_signal` evidence.
- Bayesian Calibration of Promotion Thresholds:
  per-`(target_surface, kind, scope)` Beta-binomial posteriors with
  conformal abstention floor; replaces static `threshold.*` config
  values with fitted `calibration_profile` artifacts.
  **Done.** Includes narrowest-scope fallback, Beta-binomial + conformal
  sidecar fitting, per-surface tau config, dynamic surface discovery,
  working-profile gate consumption, prompt/model version keyed refits,
  fixture-backed calibration tests, and benchmark requests.
- Outcome-Driven Demotion and Poison Resilience:
  per-row retrieval-outcome attribution via `retrieval_event_id`;
  demotion driven by observed downstream outcomes rather than declared
  trust metadata; poison-slice benchmark with a release-blocking
  clean-vs-adversarial delta gate. **Done.**

### Recall evaluation

- Unified Benchmark Suite: Target Adapters, Pinned Judge, Memory + Coding + Reasoning:
  language-neutral target-adapter contract; pinned open-weights
  judge; catalog across memory (LoCoMo, LongMemEval-S/M/L, MSC, DMR,
  BEAM, MRCR, RULER, L-Eval), coding (HumanEval, MBPP+, BigCodeBench,
  LiveCodeBench, Aider polyglot, SWE-bench Lite/Verified, RepoBench,
  TerminalBench), and reasoning (GSM8K, MATH-500, AIME, GPQA, ARC-AGI-2,
  BBH, MMLU-Pro, HLE, FrontierMath, DROP, LogiQA); `model_only` and
  `small_agent` (Qwen3 ~3B CPU) reference targets; `direct` / `llm`
  tracks; token-efficiency gates and 1M-scale production suite.
  **Accepted. Phase PR1 shipped, `benchmarks/catalog.toml`, `benchmarks/targets/aimee/adapter.py`,
  provenance schema extensions, and `benchmarks/suite/` dispatch scripts.
  PR2-PR8 require dogfood and calibration study.**

### Session working-set and outbound payload

Two sibling proposals: one owns the compacted session context, the other
owns the transport-time decision to preserve prompt-cache prefixes.

- Virtual Context Assembly and Recoverable Tool-Chain Paging:
  session-local prompt working-set management; tool-chain stubs;
  budget-aware assembly in `aimee-server`. Phase 1+2 shipped; Phase 3-4 require dogfood.
- Prompt-Cache-Aware Deferred Payload Rewrite:
  cache-preserving outbound payload policy; decouples compaction
  epoch from provider-facing payload rewrites. Done.

### Safety (Classify-Score)

- Neural-Assisted Guardrails and Semantic Risk Scoring:
  multi-head semantic risk scoring in `pre_tool_check`; advisor to
  deterministic policy; exemplar clustering via the charter promotion
  pipeline. Phase 0/1 shipped, sidecar mechanism, shadow dry_run mode, DB1 `guardrail_events` table, score-band policy mapping, `aimee guardrails review` CLI, and `scripts/guardrails-semantic.py` reference sidecar. Phases 2+ require dogfood.

### Planning & Execution (Plan-Search / Constrain-Verify)

- Deliberate Planning, MCTS Search, and SMT Constraint Execution:
  `plan_candidate` / `plan_template` artifacts; MCTS-seeded bounded
  plan search; SMT-sidecar hard-constraint validation; case-based
  plan repair; opt-in gate per task shape.

### Deterministic verify gate (Constrain-Verify)

- Incremental Verify: Change-Scoped Step Selection and Command Scoping:
  skip verify steps whose declared `paths:` saw no change since the last
  passing tree in `.aimee/.last-verify`, plus opt-in per-step command
  scoping to the changed-file set; conservative invalidation
  (`always_run_globs`, undeclared / new steps) so a tree is never reported
  passing that a full run would fail. Extends the shipped
  session-safety verify gate. **Done.**

### Experimentation & Policy (Evaluate-Optimize)

- Contextual Bandits and Counterfactual Replay:
  Thompson sampling over sidecar / ranker / prompt variants;
  synthetic-control + IPW attribution over `audit_outcome` evidence;
  `policy_arm` artifacts flipped through the charter promotion
  pipeline. **Done.** Five decision points wired (`kb_fusion_mode`, `kb_max_results`, `kb_result_format`, `kb_result_count`, `kb_memory_retrieval_limit`); exploration-budget Gate via `db2_bandit_explore_stats` + `is_exploration` column; replay attribution recorded as `benchmark_trace` artifacts through `POST /v1/intelligence/bandit/replay-record` (`aimee kb bandit --record-replay`); `tools/bandit_fixture_replay.py` 6/6 fixtures green.

### Operational Validation Cycles

Two proposals that exist purely to close acceptance items from
shipped code on a real calendar. Same pattern: shipped plumbing +
unshipped operational artifacts + explicit close-by deadline.

- Working-Profile Operational Validation Cycle:
  first end-to-end test of the charter's promotion +
  retroactive-review loop; proving ground before the pattern
  generalizes.
- Dogfood Autolabel Operational Cycle:
  operational loop for the dogfood classifier; first monthly review
  artefact and cross-session reminder demo.

### Imported from the hermes-agent intake

Eleven candidates drafted from the
hermes-agent concept intake
(survey of `nousresearch/hermes-agent`). Each cites its charter role(s)
and the shipped aimee work it builds on. Listed by the intake's priority.

Safety / reliability (P1-P2):

- MCP Supply-Chain Malware Gate:
  OSV `MAL-*` check before launching `npx`/`uvx`-backed MCP servers;
  fail-open; DB1 cache; allowlist. **Enforce. Done.**
- API Error Taxonomy and Failover Classifier:
  typed `failover_reason_t` + recovery routing (retry / rotate / fallback /
  compress / abort), replacing scattered status matching in `http_retry.c`.
  **Classify-Score → Enforce. Done.**
- Multi-Credential Pool and Rate-Limit Failover:
  per-provider key pool, `x-ratelimit-*` capture, rotation on the
  classifier's rate-limit/billing reason. **Enforce / Calibrate. Done.**
- Model Capability Registry and Capability-Aware Routing:
  offline-first `models.dev` metadata behind the catalog; capability-gated
  delegate routing. **Rank-Fuse / Calibrate. Done.**
- Session Search Agent Tool:
  zero-LLM agent-callable FTS over DB1 sessions (discovery / scroll /
  browse + bookends). **Recall.**
- Shutdown and Crash Forensics:
  non-blocking signal-time context capture (signal, sender, in-flight
  state) across daemons; surfaced by `aimee doctor`.

Capability / coordination / research (P3):

- Mixture-of-Agents Ensemble Delegate Mode:
  opt-in quality-up ensemble (diverse references → aggregator synthesis)
  over the delegate fabric. **Synthesize / Rank-Fuse. Marked Done; the P0 repair
  (#131) makes it actually work.** _History: the feature shipped unreachable,
  `delegate_ensemble_run` had no caller in any shipped binary (its only caller,
  `cmd_agent_delegate.c`, is lint-only), so `aimee delegate aggregate` ran an
  ordinary single-agent delegate, and even the engine fan-out routed every
  reference to the one default agent. PR #131 fixes both: a first-class
  `POST /v1/delegate/aggregate` entry point and per-task agent routing (plus the
  temperature/`srand`/clone fixes). The original done-proposal file is absent from
  this tree; the analysis lives in the Agent Roundtable proposal below._
- [Agent Roundtable, Round-Robin Collaborative Drafting and Review](proposals/done/agent-roundtable-collaborative-drafting.md):
  first makes the shipped-but-dead ensemble actually work, wires an entry point
  (no shipped binary calls it today) and fixes the unrouted-references bug (every
  "participant" is the same default agent), then generalizes it into a bounded
  multi-round roundtable: real participant routing, draft/review modes,
  deterministic convergence, keep-best, preflight cost limits.
  **Draft / Review / Reason. Done.**
- [Agent-Directed PR Review](proposals/done/agent-directed-pr-review.md):
  lets an agent call in the roundtable's review mode against a code change and
  direct it with a brief (focus areas, fixes made, invariants, questions), with
  an open mandate so direction reorders priority without suppressing findings.
  Adds an optional `brief` threaded through `build_round_prompt`, returns the
  structured review items (not just prose) plus answered questions, and exposes a
  `CAP_DELEGATE`-gated `ensemble_review` MCP tool. Strictly additive on the
  roundtable. **Review / Reason. Done.**
- Composable Named Toolsets:
  one composable toolset object shared by roles, delegates, gateway
  channels, and the scripted-RPC allow-list. **Done.**
- Computer-Use as a Guarded Capability:
  browser/GUI control via an OSV-gated, sandboxed external MCP server with
  per-action risk bands + approval-required enforcement.
  **Classify-Score / Enforce. Done.**
- Kanban Board over the Work Queue:
  shipped board view over the existing `cmd_work.c` queue.
- Kanban Lanes and Claim Hardening:
  lane claim model + concurrency hardening over the existing queue.
- Trajectory Capture and Compression:
  redacted, compressed, replayable trajectories from the DB1 event log for
  offline eval/training. **Evaluate-Optimize. Done.**

### Skills methodology and capability surface

aimee shipped a complete skill *engine*
(`skill-context-injection`,
`agent-self-improvement-skill-lifecycle`)
but ships no skill *content* and has no proactive dispatcher for the
primary agent. Four sibling proposals adapt the methodology layer of
[`obra/superpowers`](https://github.com/obra/superpowers) (MIT) onto
that engine and expose aimee's own capabilities through the same
surface. Methodology skills *teach*; the deterministic gates
(`tdd-enforcement`, `structured-code-review`) still *enforce*, the
two are defense in depth.

- Bundled Methodology Skill Library:
  curated default skills (TDD, systematic-debugging,
  verification-before-completion, condition-based-waiting, …) seeded at
  install; bundled discovery tier; directory `SKILL.md` format. Loads
  the empty engine.
- Proactive Skill Dispatch:
  SessionStart skill-index injection + dispatch directive + opt-in
  deterministic PreToolUse advisory; prompt-cache-safe. Charter roles
  Classify-Score / Gate-Promote. The "reach for the right skill"
  mechanism the engine lacks.
- Skill Authoring Discipline and Compliance Eval:
  `writing-skills` meta-skill + before/after compliance eval gating
  `skill_change` promotion; `aimee skill lint`. Closes the quality hole
  in the lifecycle proposal. Charter roles Evaluate-Optimize /
  Gate-Promote.
- Capability Skills: Expose aimee's Native Subsystems:
  thin trigger-activated skills routing to `find_symbol`,
  `search_memory`, `search_docs`, `delegate` so agents reach for
  aimee's tooling over grep/re-asking. Charter role Classify-Score.

## Accepted

- Unified Benchmark Suite: Target Adapters, Pinned Judge, Memory + Coding + Reasoning:
  PR1 shipped, `benchmarks/catalog.toml`, `benchmarks/targets/aimee/adapter.py` (wraps AimeeHarness
  behind the stdio JSON protocol), provenance fields in `result_schema.py`, judge-profile / dataset-hash
  refusal check in `verify_scores.py`, and `benchmarks/suite/` dispatch scripts.
  PR2 (pinned open-weights judge) and PR3+ require dogfood and calibration study.
- aimee-kb Service and Public `/v1/` API:
  Phase 1 shipped, `/v1/health`, `/v1/version`, `/v1/capabilities` HTTP endpoints;
  bearer-token auth middleware; `kb.api.http_port` / `kb_api_bearer_token` config; `--http-port=N`
  CLI arg; OpenAPI 3.1 spec at `api/openapi-v1.yaml`. Phases 2+ require dogfood.
- Virtual Context Assembly and Recoverable Tool-Chain Paging:
  Phase 1+2 shipped, DB1 schema, tool-chain events, deterministic stubs, three MCP inspection tools, `conv_ctx_assemble` budget-aware assembly injected into delegate context. Phases 3-4 require dogfood and benchmark validation.
- Prompt-Cache-Aware Deferred Payload Rewrite:
  Done, metadata/observability, opt-in deferral, proxy/delegate transport
  coverage, and adaptive context refresh gating shipped.
- Neural-Assisted Guardrails and Semantic Risk Scoring:
  Phase 0/1 shipped, sidecar mechanism, shadow dry_run mode, DB1 `guardrail_events` table, score-band policy mapping, `aimee guardrails review` CLI, and `scripts/guardrails-semantic.py` reference sidecar. Phases 2+ require dogfood.
## Done

The [`proposals/done/`](proposals/done/) directory holds 17 shipped
proposals. Recent highlights by theme:

- **Architecture / platform contracts.** Architecture Charter (umbrella
  role-division contract for all intelligence-surface proposals),
  three-DB split + pin-backends + tier-pinned DAL, memory public
  contract (typed mutation verbs, profile packs, stable `--explain`,
  thin-client shape), retrieval-ranker-must-not-consume-confidence
  boundary rule.
- **Safety / integrity.** Ingest poison gate (Layer 1 deterministic
  pattern gate; five threat categories; obfuscation-aware normaliser;
  shadow mode; benchmark fixture sets).
- **Session / UX.** Conversation branching and thread exploration,
  live provider catalog and low-context delegate guards,
  learning-signals router phase 2 fixtures and implicit heuristics.
- **Retrieval / memory quality.** Memory quality pillars, cross-encoder
  reranker, embedding model upgrade, HyDE and query decomposition,
  scene clustering two-stage retrieval, adaptive query routing (plus
  eval + graph-and-stage-pruning follow-ups), aggregation-aware
  routing, answer-time citation enforcement, conversational-retrieval
  tuning bundle (entity/signal + rerank/hard-negatives + temporal
  resolution), two-lane retrieval with summary and atomic-fact lanes,
  graph PageRank (context pruning + eval + LongMemEval
  lift report), information-theoretic salience, memory surprise
  scoring, recall economy progressive disclosure (bounded ingress
  envelopes, memory previews with `memory:<id>` pull-handles,
  shadow-mode retrieval shortcuts, and additive use-case intent
  search).
- **Knowledge base.** KB vector collection and retrieval (originally
  shipped against Qdrant; folded into pgvector inside DB2 in #1575),
  vector benchmark rollout, vector index sync and cutover, vector
  observability, vector schema versioning, vector service lifecycle,
  vector write path, LLM-driven cognification, async cognification
  pipeline (+ benchmark + job execution / recovery), codebase
  conventions ingestion, coreference resolution (+ audit + LLM
  bindings), entity profile cards, episodic vs semantic memory split,
  memory unit shape and retrieval planner, negation and absence
  memory, online re-embed and dual-index rollover, memory lifecycle
  states and alerts, memory-kind cognifier and procedural merge,
  memory retrieval golden corpus, memory scope lattice (+ rollout),
  memory semantic dedupe and supersession, typed knowledge graph
  ontology.
- **Agent / identity / learning.** Persistent personal agent core,
  personal agent phases 1-4 (foundations / recall / curiosity /
  identity), curiosity engine, genome and phenotype identity,
  disposition traits (+ config / scoped overrides), epistemic
  directives, functional memory hierarchy, learning-signals router
  (+ phase 2), prospective memory and triggered recall, retrieval
  failure detection, scheduled memory maintenance cycles, dogfood
  operational closeout, project-scoped workflow learning.
- **Guardrails / safety / edit control.** AI slop detection, bash
  command guard, branch ownership enforcement, sandboxed tool
  execution (Linux namespace isolation), session safety (verify gate
  / merged-PR enforcement / worktree rewrite), TDD enforcement in
  guardrails, orchestrator self-discipline, structural budgets and
  ownership guards, autonomous-mode skip-permissions, policy
  scripting.
- **Execution / orchestration.** Agent infrastructure context
  awareness, agent loop middleware, agent streamlining, autonomous
  pipeline, background process management, concurrent tool execution,
  collaborative agent rules, delegate role prompts, delegate token
  budget, delegate web search tool, delegation error recovery,
  session-templated multi-agent workflows (+ channel / programmatic
  surfaces), coordinated parallel execution, configurable iteration
  limits, graceful cancellation, delegate loop guards.
- **Service / ops / platform.** Doctor command + webchat dashboard,
  event notification hooks, Fedora / RHEL compatibility, guided
  onboarding and operator console, multi-provider routing, secret
  store auto-migration (+ Windows backend), self-update notifier,
  Windows OS support, worktree remaining fixes, MCP externalization,
  MCP session-aware git, MCP git context match, lean refactor audit,
  shared logic library transition, stack-detecting init.
- **CLI / UX / session.** Slash commands, persistent input history,
  session history CLI and webchat browser, line editor with vim
  keybindings, channel message SSE broadcast, investigation notes.
- **Benchmarking / eval.** Benchmark comparative baselines (BM25 +
  dense + mem0), benchmark harness v2 (+ derived metrics), density-
  based context assembly.

For the full chronological list, see the directory listing or
`git log -- docs/proposals/done/`.

## Rejected

- Adoption and Onboarding
- DRY Refactoring
- Idempotent Tool Caching
- Operations Runbook
- Project-Scoped Memory
- Relocate Session State Out of the Repository
- Split CLI / Hooks / Chat
- Split cmd / Agent
- Split DB and Memory
- Split Webchat

## Notes

- The pending list is small by design. Near-duplicates are merged or
  split at the boundaries where review concerns differ.
- `docs/proposals/done/`, `accepted/`, `pending/`, `deferred/`,
  `rejected/`, and `reviews/` remain the authoritative state folders.
  Proposals that have shipped move to `done/`.
- The Architecture Charter
  is the single review gate for any new intelligence-surface proposal,
  whether neural, symbolic, statistical, planning, or deterministic. A
  new proposal in that scope that does not name its charter role(s) and
  cite the charter is not ready for review.
- The Memory Public Contract
  is the review gate for any new caller-facing memory surface, whether
  a CLI flag, MCP tool shape, or HTTP endpoint. New surfaces align with
  the contract or explicitly justify a deviation.
