# Proposal: Agent-facing code intelligence effectiveness

- **State:** ACCEPTED DESIGN — roundtable approved; proposal PR/merge gate pending
- **Author:** JBailes
- **Date:** 2026-07-29
- **Charter roles:** Recall, Rank-Fuse, Calibrate / Evaluate-Optimize, Enforce, Gate-Promote
- **Supersedes:** no prior proposal
- **Related:** [Code-graph intelligence](../done/code-graph-intelligence.md),
  [Recall abstention confidence gate](../done/retrieval-abstention-confidence-gate.md),
  [Binding retrieval context-contract](proposal-retrieval-context-contract.md),
  [KB ingest content-push deltas](kb-ingest-content-push-deltas.md), and
  [Agentic supervised SWE-bench](agentic-supervised-swebench.md)

## Decision summary

Aimee already indexes symbols, calls, imports, embeddings, and memories. The missing contract is not
another index. It is the path from a current coding task to a precise, current-project result that an
agent can discover and use cheaply.

We will make that path effective through six bounded changes:

1. publish a truthful, searchable MCP surface for the capabilities installed guidance names;
2. resolve every code query to one explicit project identity, with lifecycle controls for stale
   generations;
3. repair dependency and blast-radius matching across path and language import forms;
4. add bounded task-conditioned code retrieval with confidence-based abstention to session/turn
   context assembly;
5. make KB unavailability observable and recoverable without fabricating useful retrieval; and
6. gate rollout on an attribution-safe benchmark that separates availability, tool adoption,
   retrieval quality, task quality, and total cost.

The proposal does **not** claim that embeddings are ineffective. The measured failure is that the
standard installed integration almost never asked the vector/hybrid surface, and the few repository
memory searches it did make returned unrelated global episodes. An unused index cannot improve an
agent, and a noisy result can make it worse.

## 1. Problem and measured boundary

### 1.1 What the interrupted Ponytail run established

The July 2026 Ponytail Codex matrix stopped after 97 of 600 result artifacts when the remote KB at
`192.168.1.210:8741` became unreachable while `aimee-server` at `:8743` remained healthy. The runner
failed closed, so the incomplete matrix is diagnostic evidence, not a publishable final comparison.
The raw, uncommitted run artifacts are preserved in the sibling benchmark checkout at
`/home/virant/dev/ponytail-codex-benchmark/battery/codex_results/` against harness commit
`a40fe3d6a58ac4e8b14aaf75320e83912f0bfc56` on `agent/codex-benchmark-matrix`; the representative
readiness trace is
`cells/aimee__t06_semver__r1/aimee-readiness.json`. Because a local path is not durable review
evidence, E0 must publish a checksummed validation record under `docs/validation/` before any
implementation PR cites these numbers as its red baseline.

Across the 23 task/repetition cells for which the baseline and Aimee arms both completed:

| observation | result |
| --- | --- |
| task success | baseline 20/23; Ponytail instructions 20/23; addon 20/23; Aimee 21/23 |
| Aimee MCP adoption | 13/23 Aimee cells made no Aimee MCP call |
| completed repository-memory searches | 9 task cells, 10 including the setup canary |
| code-index use | 1 `find_symbol`; 0 semantic/hybrid code searches; 0 caller queries; 0 successful blast previews |
| embeddings prepared | 35 code embeddings per cell; queried only by readiness, not by the coding agent |
| failure observed only in the Aimee arm | one task patch used `success`/`failure` where an undocumented hidden test required `succeeded`/`failed` |

The 10 repository-specific memory searches all returned unrelated delegation/roundtable episodes.
This is evidence that the selected retrieval scope and abstention behavior were wrong for these
queries; it is not evidence that those episodes were invalid memories.

The arm-specific vocabulary failure is **not classified as an Aimee product limitation**. The patch
implemented the ticket's behavior, did not use Aimee retrieval in a way that selected the disputed
vocabulary, and the required strings were absent from the task contract. E0 records it as an
ambiguous benchmark exclusion. No product slice will tune Aimee to guess that hidden vocabulary.

MCP-using cells were slower and consumed more input credits than non-MCP cells in this small sample,
but tool choice was endogenous and the tasks differ. The result supports an overhead hypothesis, not
a causal claim that MCP use caused failures. Any rollout gate below therefore compares randomized or
paired arms and reports total preparation plus task cost.

### 1.2 Readiness exposed product defects rather than agent preference

The harness built and checked the index before each isolated cell. It found four product defects:

- `find_symbol` and caller results contained duplicates from 3–28 project namespaces. The current
  project was present, but the public `find_symbol` schema has no `project` input and agents received
  the entire ambiguous set.
- all 24 readiness samples returned empty blast-radius `dependencies` and `dependents` for
  `app/dates.py`, although caller lookup found `billing.py`, `invoices.py`, and `reports.py`.
- installed Codex guidance requires `preview_blast_radius`, and `get_help("MCP Tools")` advertises
  it, but the lean tools list does not expose it and `find_tools("blast radius")` finds nothing. A
  generic hidden `index` tool contains `blast_radius` and `preview` commands, but its discovery text
  does not include those command terms.
- repository memory retrieval did not abstain when no project fact supported the question.

The dependency defect has a concrete red-baseline candidate. Both
`canonical_index_blast_radius()` and `db2_code_index_blast_radius()` search imports with a SQL `LIKE`
pattern derived from the literal file path. A Python file `app/dates.py` is imported as `app.dates`
or `from app import dates`, so `%app/dates.py%` cannot match the indexed module name.

### 1.3 The benchmark also has limits that Aimee must not optimize around

The fixture contains 519 production lines across 34 files. Forty-eight of 50 tickets name an exact
`app/` path and 36 name a symbol-like target, so local text search is already near the information
ceiling. One task expects a duplicated traversal implementation instead of updating actual callers;
another hidden test requires event vocabulary absent from the ticket. These are benchmark validity
issues, not Aimee defects.

The readiness gate is also task-independent: every cell probes the same `end_of_month` symbol and
README semantic query, then accepts an empty graph. It proves that a scan and embedding request can
complete; it does not prove that the task's evidence is retrievable.

We will keep readiness as an availability gate, but it cannot be the capability or efficacy score.

## 2. Goals and non-goals

### Goals

- A coding agent sees one stable, documented name for each recommended capability, and discovery by
  the words in installed guidance returns that capability.
- A code query defaults to the active repository and never silently mixes stale generations from
  unrelated checkouts; cross-repository search is explicit.
- blast radius resolves language import identities, beginning with Python modules, and agrees with
  caller/import evidence on a checked-in corpus.
- standard installation gives code embeddings a bounded chance to affect the task without requiring
  lucky voluntary tool use.
- weak or unrelated evidence yields `no_answer`/no injection rather than a plausible-looking global
  memory dump.
- KB failure is visible, bounded, and recoverable; the server and client distinguish unavailable,
  empty, stale, and abstained results.
- evaluations attribute any quality or cost change to a named retrieval mode and include index
  preparation cost separately from amortized steady-state cost.

### Non-goals

- replacing the existing graph, embedding model, RRF hybrid ranker, memory assembler, or ingress
  pre-injection framework;
- forcing every coding task through semantic retrieval when literal/local inspection is sufficient;
- purging durable knowledge as an implicit side effect of `workspace remove`;
- tuning to Ponytail's questionable hidden outputs;
- claiming superiority from the interrupted 97-artifact run; or
- enabling cross-project search by default merely because the backend can do it.

## 3. Invariants and failure model

1. **Current source wins.** Indexed snippets are discovery evidence. A current worktree read remains
   authoritative for file contents and edits.
2. **Scope is explicit.** Omitted project means the active workspace project, not every project.
   `scope: all` is an explicit opt-in and is labeled cross-project in output.
3. **No false empty.** Unavailable, unauthorized, stale, abstained, and genuinely empty are distinct
   machine-readable outcomes.
4. **No noisy fallback.** A failed project-scoped code lookup must not silently fall back to global
   episodic memory.
5. **Bounded injection.** Task-conditioned context has a token/result cap, provenance, freshness,
   confidence, and an off switch. A low-confidence result injects nothing.
6. **Fail open for coding, fail closed for claims.** KB failure cannot prevent local inspection and
   editing, but the agent and benchmark may not report an Aimee-assisted success for that turn.
7. **Lifecycle is recoverable.** Unregister, detach, tombstone, purge, and rescan are distinct. Purge
   requires an explicit target and confirmation/authority.
8. **Tool names do not drift.** Generated help, MCP registry, discovery aliases, plugin prompt, and
   installed skill derive from one capability descriptor or fail a generation check.

Threats and failures include stale checkout namespaces, basename collisions, poisoned or unrelated
memory, malicious task text attempting cross-project disclosure, a dead embedder/KB, partial ingest,
schema drift, oversized prompt injection, and a benchmark that credits prepared-but-unused context.

## 4. Decisions

### 4.1 One generated agent-facing capability catalog

Introduce a descriptor for each agent-facing capability containing:

- canonical tool name and compatibility aliases;
- one-line discovery terms, including subcommands users are told to search for;
- input schema, including `project`/`scope` where applicable;
- presentation tier (`core`, `lean-discoverable`, `full`);
- readiness predicate and unavailable behavior; and
- guidance text references.

`tools/list`, `find_tools`, `describe_tool`, `get_help("MCP Tools")`, the Codex plugin manifest,
installed `SKILL.md`, Claude command text, and generated docs consume this descriptor. Until a full
descriptor refactor lands, a mechanical parity check over the existing tables is acceptable, but
there may be only one canonical public vocabulary.

The current conflicting Codex sources are generated by `src/client_integrations.c`:
`codex_skill_markdown()` names `preview_blast_radius`, while `plugin_buf` and
`compat_plugin_buf` name `search_graph` in `interface.defaultPrompt`. Installed copies under
`plugins/aimee/` and `.codex/plugins/cache/local/aimee/` are outputs, not independent sources.
Correcting `search_graph` in generated guidance is a documentation/presentation correction. If an
existing tool or client still calls that name, keep it as a compatibility alias; do not remove a wire
route in E1.

For the first slice:

- expose `preview_blast_radius` directly in the lean discoverable catalog, with `project` and
  `paths`; keep `index({command:"preview"})` as a compatibility route;
- make `find_tools("blast radius")` and `find_tools("preview")` find it;
- expose project-scoped `find_symbol` and callers inputs; and
- replace installed references to nonexistent `search_graph` with the canonical hybrid/context tool
  name, while retaining any compatibility alias that existing clients require.

### 4.2 Stable project identity and lifecycle

Project identity becomes a stable key derived in order from an explicit workspace manifest ID,
canonical forge remote plus repository path, or a persisted generated UUID for non-git directories.
Checkout paths and ephemeral worktree paths are aliases, not project identities.

Every code-index request carries:

```json
{
  "project": "stable-project-id",
  "scope": "current",
  "generation": "optional-observed-generation"
}
```

The server resolves omitted `project` from the authenticated session's workspace. If resolution is
ambiguous, the tool returns `scope_required` with a bounded list; it never searches all projects.
`scope:"all"` is needed for deliberate cross-repository queries.

Lifecycle operations are separate:

- `workspace remove`: unregister only, preserving today's safe behavior;
- `index detach <project>`: stop treating a generation as current while retaining it for audit;
- `index purge <project>`: explicit authorized destructive deletion with dry-run/confirmation;
- `index gc`: retire duplicate checkout aliases and expired detached generations under a documented
  retention policy.

Every purge and mutating GC operation records actor/principal, stable project ID, affected generation,
timestamp, reason, dry-run/confirmed mode, and row/object counts through the standard action-audit
path. Audit failure blocks the destructive mutation. Dry-run is read-only but returns the same target
manifest that the confirmed operation hashes and records, so confirmation cannot drift to a broader
target.

Ingest upserts by stable identity and generation, so re-adding a moved checkout does not create a new
project. This shares the identity/tombstone contract from `kb-ingest-content-push-deltas.md`; this
proposal owns agent query scoping and duplicate-generation cleanup, not transport sequencing.

### 4.3 Language-aware blast-radius resolution

Replace literal `%file_path%` matching with normalized import identities generated by the extractor
and shared with caller/dependency queries.

For Python `app/dates.py`, candidates include `app.dates`, `app.dates.__init__` where applicable,
package-relative forms resolved against the importing file, and the explicit `from app import dates`
pair. Similar language resolvers remain table-driven follow-ups, but no resolver may use an
unescaped substring match as authoritative evidence.

The query uses indexed relationships first:

- **dependencies:** normalized imports owned by the target file;
- **dependents:** files whose normalized import resolves to the target file/module;
- **callers:** direct call edges, merged without duplicates; and
- **cross-repo dependents:** only through already-resolved structural repository routes.

Each edge includes provenance (`import`, `call`, `projection`, `cross_repo`), confidence, project,
generation, and freshness. Empty results remain valid only after the target file resolved in the
requested current generation.

### 4.4 Task-conditioned code context with abstention

Reuse `/v1/code/context`, the hybrid RRF ranker, `ingress_preinject`, and the retrieval context
contract. On session start and the first repository-specific turn after the task changes, request a
small current-project code-context packet using the user's task text.

The packet is capped initially at 4 results / 1,200 tokens and contains:

- exact literal/symbol matches first;
- vector matches only when they clear project-relative quality and confidence floors;
- graph neighbors only when linked to one of those anchors;
- project memories only when their scope matches and they add a decision/constraint absent from the
  code evidence; and
- provenance, path/symbol span, current generation, and confidence for every item.

Apply the shipped answerability gate to this surface with a code-specific trace. If no result clears
the threshold, return `no_answer` and inject nothing. Do not substitute global memory. A packet is
advisory and cannot override current source or force an edit.

The default rollout is measured and reversible:

1. `off`: current behavior;
2. `observe`: retrieve and record attribution, but do not inject;
3. `on`: inject the bounded packet; and
4. automatic suppression to `observe` when precision, freshness, latency, or availability gates
   fail.

This proposal supplies automatic selection and truthful evidence. The pending binding retrieval
context-contract continues to own exploration-budget enforcement; neither proposal blocks local
fallback when the index is unavailable or abstains.

### 4.5 Availability, recovery, and result status

Add a dependency-health state for the KB and embedder with bounded exponential backoff plus jitter,
a circuit breaker, and a single-flight recovery probe. The server remains available for local and
non-KB operations. Retrieval calls return a typed status:

- `ok` with results;
- `empty` after a valid current-generation query;
- `abstained` with evidence trace;
- `stale` with observed/current generation;
- `unavailable` with dependency and retryability; or
- `unauthorized`.

Container/systemd deployment owns restart policy and liveness/readiness probes. Readiness is false
when the process cannot serve its claimed retrieval contract; liveness stays true while a recoverable
dependency is down. Document a runbook that identifies the failed dependency, pending queue depth,
last successful ingest/query, breaker state, and safe recovery commands.

Clients may fall back to local inspection on `unavailable`; they must not turn it into `empty` or
silently retry for an unbounded interval. Benchmarks record the cell as infrastructure-invalid,
preserve artifacts, and resume from a named checkpoint.

### 4.6 Attribution-safe evaluation

Add a checked-in agent-facing code-intelligence evaluation contract with five layers:

1. **availability:** scan, current generation, embedding count, and one task-specific query work;
2. **retrieval:** labeled relevant file/symbol/decision recall, precision, abstention, duplicate rate,
   and blast-radius edge accuracy;
3. **adoption:** whether context was injected or a tool result was consumed before the decisive edit;
4. **task:** public tests plus adjudicated behavior, with ambiguous hidden contracts excluded from
   the primary claim; and
5. **economics:** index wall/cost, amortized steady-state wall/cost, task tokens, tool round trips,
   and total wall.

Required arms are paired on the same task and base commit:

- baseline local tools;
- standard installed Aimee (the product default);
- standard Aimee in `observe` mode;
- Aimee `on` with task-conditioned packet; and
- guided capability-ceiling Aimee, whose prompt requires a scoped hybrid query and blast preview.

The ceiling arm diagnoses substrate quality; it is never reported as default-product performance.
Its prompt lives as a checked-in, content-hashed evaluation fixture. Every result manifest records
the prompt hash, and any prompt change is a new evaluation version whose diff is included in the
validation report; results from different versions are not silently pooled.
The corpus must include path-blind issues, convention-heavy changes, multi-hop and cross-module
edits, distractor memories from another project, moved/re-added workspaces, and both useful and
unanswerable embedding queries. Larger real repositories supplement the small Ponytail fixture.

No headline lift is accepted unless the Aimee evidence was actually consumed. Report intention-to-
treat (all valid cells) and treatment-on-treated (evidence consumed) separately, with paired
confidence intervals and infrastructure exclusions named.

## 5. Compatibility and migration

- Existing CLI `index` subcommands and generic MCP `index` calls remain compatibility routes.
- Adding optional `project` and `scope` inputs is wire-compatible. The behavioral change from
  all-project to current-project default requires a release note and an explicit `scope:"all"`
  migration example.
- Existing path-keyed project rows are mapped to stable identities in a reversible migration. When
  two rows map to one identity, keep generations until `index gc` proves one current winner; do not
  delete on migration.
- `workspace remove` remains non-destructive. New purge behavior is a separate command/capability.
- Task-conditioned injection starts `off`, then `observe`; promotion to `on` requires the gates in
  §7. One config switch returns to the prior behavior.
- Result-status additions preserve human-readable output while adding structured fields for clients.

## 6. Proposal acceptance gate and delivery slices

Implementation is forbidden until this design record completes the following gate in order:

1. run proposal roundtable rounds until the panel returns approved/converged with no blocking
   findings;
2. record every round and ruling in §9;
3. commit only this proposal on `agent/indexing-effectiveness`, open a ready PR to `testing`, and run
   normal repository/CI review;
4. address PR findings without weakening the roundtable-approved invariants; rerun the proposal
   roundtable if review materially changes scope or a decision;
5. merge the proposal PR to `testing` and record the PR/merge commit plus accepted E0–E6 scope in
   §9 or an immediate reconciliation amendment; and
6. fetch the merged `testing` tip. Only then may E0 or any implementation slice branch from that
   accepted state.

Implementation PRs may amend the proposal when evidence changes a premise, but an amendment lands and
is accepted before dependent code. No code PR can claim this proposal's authority while based only on
the unmerged proposal branch.

### Delivery slices and ownership

| slice | owner / dependencies | deliverable |
| --- | --- | --- |
| E0 — reproducible red baselines | QA; no code dependency | checked-in fixtures for discovery mismatch, project duplicates, Python blast radius, irrelevant-memory abstention, and KB outage status |
| E1 — truthful capability catalog | MCP/client integrations | direct/discoverable tools, project-aware schemas, generated guidance parity check, compatibility aliases |
| E2 — current-project identity | workspace + DB2 ingest; coordinates with content-push deltas | stable identity mapping, session inference, explicit all-project scope, duplicate suppression, detach/purge/gc contract |
| E3 — graph resolution repair | DB2 code index/extractors; depends on E2 | normalized Python imports, dependency/caller merge, provenance/freshness, corpus regression |
| E4 — task-conditioned retrieval | ingress + hybrid ranker + abstention; depends on E1/E2 | off/observe/on packet, no-answer path, bounded telemetry and automatic suppression |
| E5a — dependency status and recovery | server/KB runtime | typed status, breaker/backoff, single-flight recovery, bounded client behavior |
| E5b — deployment and runbook | deployment/operations; depends on E5a | liveness/readiness probes, restart policy, diagnostics and recovery runbook |
| E5c — resumable experiment execution | benchmark/QA; depends on E5a | infrastructure-invalid result, artifact preservation, named checkpoint/resume without result splicing |
| E6 — evaluation and promotion | benchmark/QA; depends on E0–E5c | multi-layer harness, larger corpus, standard/observe/on/ceiling arms, versioned prompt fixture, fresh full run against a pinned merged Aimee version |

Each implementation slice gets its own branch and PR to `testing`, a red baseline captured before the
fix, project verification, and a frozen-diff roundtable review to convergence. A later slice branches
from the merged `testing` tip. E0 is the mandatory first implementation slice: it must merge to
`testing` before E1–E6 branch, so later fixes retain the untreated controls and durable evidence.
E4 cannot default to `on` until E6 passes; code may ship in `observe`.

## 7. Acceptance gates

### Mechanical and integration

- {id: 1, tier: mechanical, check: "generated integration test proves every tool name in installed Codex/Claude guidance is either directly listed or discoverable by those exact words, and its documented schema matches describe_tool"}
- {id: 1a, tier: integration, check: "through the installed agent-facing MCP surface, discover blast radius using the documented words, invoke the returned canonical tool with active-project defaults, and receive the expected project-scoped preview"}
- {id: 2, tier: mechanical, check: "find_symbol/callers/blast preview default to the active project; a fixture with 20 stale duplicate namespaces returns one current result, while scope=all returns labeled cross-project results"}
- {id: 3, tier: integration, check: "workspace move/re-add preserves stable project identity; unregister preserves data; detach hides it from current queries; purge dry-run names exact rows and explicit purge removes only that project"}
- {id: 3a, tier: integration, check: "purge and mutating GC are refused when action audit cannot commit; success records principal, stable project, generation, timestamp, reason, target-manifest hash, and affected counts"}
- {id: 4, tier: integration, check: "stock baseline returns no dependents for app/dates.py; fixed Python fixture returns billing.py, invoices.py, and reports.py with import provenance, plus its direct dependencies, with zero substring-collision false edges"}
- {id: 5, tier: integration, check: "task-conditioned retrieval returns relevant current-project evidence for labeled answerable tasks, abstains for unrelated/global-only evidence, and injects no more than 4 results/1200 tokens"}
- {id: 6, tier: integration, check: "killing the KB produces unavailable (not empty), local work continues, retry rate stays bounded, recovery returns to ok without restarting the client, and readiness/liveness report distinct states"}
- {id: 7, tier: mechanical, check: "make -C src proposal-links-check plus the project verification profile pass for every slice"}

### Evaluation and promotion

- On the held-out retrieval set: current-project duplicate rate = 0; scope leakage = 0; answerable
  precision ≥ 0.90; answerable recall ≥ 0.80; unanswerable abstention ≥ 0.90; Python blast-radius
  edge precision and recall both ≥ 0.95.
- On paired coding tasks, the `on` arm must not regress task success versus standard Aimee by more
  than 2 percentage points at the lower confidence bound, and must improve at least one predeclared
  efficiency metric (uncached input tokens or total wall) by ≥10% without shifting work into excluded
  setup time.
- At least 80% of answerable `on` cells must consume an injected/index result before the decisive
  edit. Otherwise the actuation contract has failed even if task scores are high.
- p95 retrieval latency ≤2 seconds steady-state; packet p95 ≤1,200 tokens; background indexing and
  per-cell isolated rebuild cost are reported separately.
- A full fresh arm uses a merged, pinned Aimee version. The interrupted 97-artifact run remains a
  historical diagnostic and is never spliced together with post-fix cells.

## 8. Documentation and close-out

Before E1, E0 publishes a checksummed `docs/validation/` record for the interrupted Ponytail run,
including the harness commit, exact raw artifact location, artifact counts, extraction commands, and
the fact that the result checkout was uncommitted/incomplete.

Update `docs/CODE_INTELLIGENCE.md`, `docs/WORKSPACES.md`, generated MCP/API references, installed
integration guidance, deployment health/runbook docs, and `docs/BENCHMARKS.md` as their owning slices
land. Validation reports under `docs/validation/` must name the commit, environment, commands, raw
artifact location, exclusions, and measured denominators.

Move this proposal to `done/` only after E0–E6 are merged and reconciled. If the evaluation rejects
default-on task context, leave E4 in `observe`, record the measured result, and link a bounded
residual proposal instead of declaring the intended effectiveness outcome complete.

## 9. Roundtable and acceptance record

- **Round 1 — 2026-07-29 — changes requested, 3/3 participants, not degraded.** One blocking
  finding required an explicit proposal-review/PR/merge gate before implementation. Five refinements
  required durable benchmark provenance, exact guidance-source ownership and compatibility policy,
  audited destructive lifecycle operations, split resilience ownership, and a versioned ceiling-arm
  prompt. All six are incorporated in §§1.1, 4.1, 4.2, 4.6, 6, 7, and 8. Roundtable run:
  `roundtable-1f12fd823a2996a7cc77e733`.
- **Round 2 — 2026-07-29 — changes requested, 3/3 participants, not degraded.** One blocking
  wording inconsistency could misclassify an undocumented hidden-test vocabulary mismatch as an
  Aimee product defect; §1.1 now explicitly treats it as an ambiguous benchmark exclusion. The
  suggested ordering/adoption amendments make E0 the first merged implementation slice and require
  an installed-surface blast-preview smoke test. Roundtable run:
  `roundtable-03528eec3912e5af2335d18d`.
- **Round 3 — 2026-07-29 — APPROVED, 3/3 participants, no findings, not degraded.** The panel
  confirmed the benchmark-validity ruling, E0 ordering, E1 end-to-end adoption smoke, and the full
  accepted scope. Roundtable run: `roundtable-ea954d6d077d0cceaa0c143e`.
- **Proposal PR / accepted implementation scope:** PR #2147 merged to `testing` as
  `5cdb681621c22938f6e7372ecc709e623408551e` after all 23 CI checks passed. The accepted scope is
  E0–E6 exactly as listed in §6; E0 branches first from that merge commit.
