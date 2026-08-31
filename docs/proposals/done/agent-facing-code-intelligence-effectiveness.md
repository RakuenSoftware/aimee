# Proposal: Agent-facing code intelligence effectiveness

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE. E0–E6 merged or reconciled; E6 retains `observe` and links bounded residual work
- **Author:** JBailes
- **Date:** 2026-07-29
- **Charter roles:** Recall, Rank-Fuse, Calibrate / Evaluate-Optimize, Enforce, Gate-Promote
- **Supersedes:** no prior proposal
- **Related:** [Code-graph intelligence](../done/code-graph-intelligence.md),
  [Recall abstention confidence gate](../done/retrieval-abstention-confidence-gate.md),
  [Binding retrieval context-contract](../pending/proposal-retrieval-context-contract.md),
  [KB ingest content-push deltas](../pending/kb-ingest-content-push-deltas.md), and
  [Agentic supervised SWE-bench](../pending/agentic-supervised-swebench.md)

## Decision summary

Aimee already indexes symbols, calls, imports, embeddings, and memories. The missing contract is the
path from a current coding task to a precise, current-project result that an agent can discover and
use cheaply. Another index would not supply it.

We will make that path effective through seven bounded changes:

1. publish a truthful, searchable MCP surface for the capabilities installed guidance names;
2. make the active project the protected head of every mixed-scope code and memory result set;
3. resolve every code query to one explicit project identity, with lifecycle controls for stale
   generations;
4. repair dependency and blast-radius matching across path and language import forms;
5. add bounded task-conditioned code retrieval with confidence-based abstention to session/turn
   context assembly;
6. make KB unavailability observable and recoverable without fabricating useful retrieval; and
7. gate rollout on an attribution-safe benchmark that separates availability, tool adoption,
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
3. **Local results own the head.** Every ordered, mixed-scope code or memory result surface ranks
   active-project evidence before active-workspace evidence, shared/global evidence, and explicitly
   requested other-project evidence. Relevance orders results within each scope bucket. Broadening
   scope may extend the tail but may not displace the local head before a result or token limit.
4. **No false empty.** Unavailable, unauthorized, stale, abstained, and genuinely empty are distinct
   machine-readable outcomes.
5. **No noisy fallback.** A failed project-scoped code lookup must not silently fall back to global
   episodic memory.
6. **Bounded injection.** Task-conditioned context has a token/result cap, provenance, freshness,
   confidence, and an off switch. A low-confidence result injects nothing.
7. **Fail open for coding, fail closed for claims.** KB failure cannot prevent local inspection and
   editing, but the agent and benchmark may not report an Aimee-assisted success for that turn.
8. **Lifecycle is recoverable.** Unregister, detach, tombstone, purge, and rescan are distinct. Purge
   requires an explicit target and confirmation/authority.
9. **Tool names do not drift.** Generated help, MCP registry, discovery aliases, plugin prompt, and
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

### 4.1a One local-first return policy for code and memory

The request boundary resolves the active project and workspace once and propagates that context to
the KB. Every ordered result-bearing surface consumes the same scope policy:

1. active project;
2. active workspace;
3. shared or global;
4. other projects only after an explicit `scope:"all"` request.

This order applies to code symbol/search/caller results and to all memory retrieval surfaces,
including fact search, answer evidence and citations, graph search and entity edges, episode search,
recall/context assembly, and briefing sections. Relevance, freshness, and confidence continue to
order candidates within a scope bucket. Exact-ID, exact-key, and explicit exact-scope retrieval are
not mixed-scope rankings and therefore retain their direct semantics.

Scope must constrain candidate selection before a per-request `LIMIT`, result cap, rerank cutoff, or
token budget can exclude active-project evidence. Implementations may query scope buckets separately
and merge them, or push a scope-rank expression into the backing query, but sorting a bounded global
candidate pool after retrieval does not satisfy this contract. An explicit broader scope extends the
tail; it never turns the local bucket into an ordinary relevance competitor.

When active context cannot be resolved, code queries return `scope_required`. Memory queries may
return shared/global evidence only when the response labels the missing active context; they may not
silently expose another project's memory. The existing canonical memory visibility rank becomes the
shared policy primitive, and each transport must carry request-local identity rather than derive it
from the KB service process's working directory.

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
| E0: reproducible red baselines | QA; no code dependency | checked-in fixtures for discovery mismatch, project duplicates, Python blast radius, irrelevant-memory abstention, and KB outage status |
| E1: truthful capability catalog | MCP/client integrations | direct/discoverable tools, project-aware schemas, generated guidance parity check, compatibility aliases |
| E1-memory: local-first memory returns | MCP/server + memory retrieval; depends on E1 | request-local project/workspace propagation and protected local-first ordering for every ordered memory surface, with explicit-scope compatibility |
| E2: current-project identity | workspace + DB2 ingest; coordinates with content-push deltas | stable identity mapping, session inference, explicit all-project scope, duplicate suppression, detach/purge/gc contract |
| E3: graph resolution repair | DB2 code index/extractors; depends on E2 | normalized Python imports, dependency/caller merge, provenance/freshness, corpus regression |
| E4: task-conditioned retrieval | ingress + hybrid ranker + abstention; depends on E1/E2 | off/observe/on packet, no-answer path, bounded telemetry and automatic suppression |
| E5a: dependency status and recovery | server/KB runtime | typed status, breaker/backoff, single-flight recovery, bounded client behavior |
| E5b: deployment and runbook | deployment/operations; depends on E5a | liveness/readiness probes, restart policy, diagnostics and recovery runbook |
| E5c: resumable experiment execution | benchmark/QA; depends on E5a | infrastructure-invalid result, artifact preservation, named checkpoint/resume without result splicing |
| E6: evaluation and promotion | benchmark/QA; depends on E0–E5c | multi-layer harness, larger corpus, standard/observe/on/ceiling arms, versioned prompt fixture, fresh full run against a pinned merged Aimee version |

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
- {id: 2a, tier: integration, check: "for fact search, answer evidence/citations, graph/entity/episode search, recall/context assembly, and briefings, more than one limit of equally relevant global or other-project candidates cannot crowd out active-project memory; returned mixed scopes are ordered project, workspace, shared/global, then explicit-all other projects"}
- {id: 2b, tier: mechanical, check: "every agent-facing ordered memory route propagates request-local project/workspace context to the shared visibility policy; exact-ID/key and explicit exact-scope routes preserve direct compatibility semantics"}
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

- **Round 1 (2026-07-29) changes requested, 3/3 participants, not degraded.** One blocking
  finding required an explicit proposal-review/PR/merge gate before implementation. Five refinements
  required durable benchmark provenance, exact guidance-source ownership and compatibility policy,
  audited destructive lifecycle operations, split resilience ownership, and a versioned ceiling-arm
  prompt. All six are incorporated in §§1.1, 4.1, 4.2, 4.6, 6, 7, and 8. Roundtable run:
  `roundtable-1f12fd823a2996a7cc77e733`.
- **Round 2 (2026-07-29) changes requested, 3/3 participants, not degraded.** One blocking
  wording inconsistency could misclassify an undocumented hidden-test vocabulary mismatch as an
  Aimee product defect; §1.1 now explicitly treats it as an ambiguous benchmark exclusion. The
  suggested ordering/adoption amendments make E0 the first merged implementation slice and require
  an installed-surface blast-preview smoke test. Roundtable run:
  `roundtable-03528eec3912e5af2335d18d`.
- **Round 3, 2026-07-29, APPROVED, 3/3 participants, no findings, not degraded.** The panel
  confirmed the benchmark-validity ruling, E0 ordering, E1 end-to-end adoption smoke, and the full
  accepted scope. Roundtable run: `roundtable-ea954d6d077d0cceaa0c143e`.
- **Proposal PR / accepted implementation scope:** PR #2147 merged to `testing` as
  `5cdb681621c22938f6e7372ecc709e623408551e` after all 23 CI checks passed. The accepted scope is
  E0–E6 exactly as listed in §6; E0 branches first from that merge commit.
- **E0 red-baseline slice:** PR #2148 merged to `testing` as
  `352b4682205800ed41714ce7bdd53b4f081f81db` after all 24 CI checks passed. It preserves the five
  untreated failure classes and checksummed representative evidence consumed by E1–E6.
- **Local-first amendment round 1 (2026-07-29) changes requested, 3/3 participants, not
  degraded.** The panel raised four blocking process/alignment findings because the proposal-only
  artifact was reviewed against the full multi-PR implementation request. The policy itself received
  no blocking technical finding. The ruling accepts the useful sequencing correction: this
  amendment is the proposal gate required by §6, so its next review is explicitly scoped to that
  gate; the dependent E1/E1-memory implementation receives a separate frozen-diff roundtable before
  its
  PR. The state line now also explains that `pending/` tracks incomplete E1–E6 delivery rather than
  unaccepted base design. Roundtable run: `oprun_g6a69bd8011459d99_1785336494_91`.
- **Local-first amendment round 2, 2026-07-29, APPROVED, 3/3 participants, not degraded.** The
  panel approved the local-first scope hierarchy, pre-limit protection, compatibility boundary, and
  acceptance gates with no blocking findings. Its sole suggestion was to make the memory-slice
  suffix self-explanatory; `E1m` is renamed `E1-memory`, and the exact resulting diff is reconvened
  before commit. Roundtable run: `oprun_g6a69bd8011459d99_1785336702_92`.
- **Local-first amendment round 3, 2026-07-29, APPROVED and converged, 2/3 participants used,
  degraded.** The final policy diff received no findings. One provider seat failed, but the chairman
  and remaining seat completed and approved the artifact. Roundtable run:
  `oprun_g6a69bd8011459d99_1785336851_93` (artifact
  `c4b68b07f28c23817533b1c66a41bac88ea60c7b79c2181090babc184f728351`).
- **Local-first amendment PR:** PR #2150 merged to `testing` as
  `e9626cda560b7e9b1bbf96e48c05c94b62a72c8a` after all 23 CI checks passed. E1 applies the same
  pre-limit rule to code discovery; E1-memory applies it to every ordered memory surface.
- **E1 capability-catalog slice:** PR #2153 merged to `testing` as
  `a8d3214c2d057fa95820e6acb60b788e403d7c68` after all 24 CI checks passed. The installed surface
  now exposes and discovers the canonical code-intelligence tools and protects active-project code
  discovery before limits.
- **E1-memory implementation round 1 (2026-07-29) changes requested, 2/3 participants used,
  degraded.** The review was explicitly ruled to be the E1-memory slice gate required by §6, not a
  claim that E2–E6 were already complete. Three actionable findings were accepted: generic
  conversation-window search did not carry request scope; briefing activity chose the newest
  episode per session before applying scope; and the conflict-alert query had tightened a
  compatibility `LEFT JOIN`. The frozen implementation was amended to propagate/suppress legacy
  unscoped windows unless `scope:"all"` is explicit, choose the local-first episode within each
  session, and preserve the left-join behavior. Roundtable run:
  `roundtable-9a24b97397594ae1898ad222` (artifact
  `efd100cf3bd976cacc403e13a9f358734e7b5243ef7194af412e36e0041c0d8a`).
- **E1-memory implementation round 2 (2026-07-29) changes requested, 3/3 participants, not
  degraded.** The blocking finding showed that the legacy session-scope `LIKE` fallback discarded
  every `memory_workspaces` row before the canonical rank could recognize an active-workspace
  memory. The prefilter is removed and an adversarial project-then-legacy-workspace assertion now
  protects the two-row head. The panel's suggestions are also incorporated: scoped assembly's
  deliberate omission of ID-less legacy `entity_edges` is documented as E2 migration debt, and the
  MCP scope-begin helper now has a `void` contract that cannot be mistaken for an activation result.
  Roundtable run: `oprun_g6a69bd8011459d99_1785345098_97` (artifact
  `baeeae4567524c11409c8592e2b8711f29c35a9e9b69b38964b6a412e9573276`).
- **E1-memory implementation round 3, 2026-07-29, APPROVED and converged, 3/3 participants,
  not degraded.** The complete frozen implementation received no blocking findings. The panel's
  two readability suggestions (the entity-profile SQL literal and qualified aliases) and one CLI
  coverage nit (`memory.read --scope all`) are incorporated before the final exact-diff review.
  Roundtable run: `oprun_g6a69bd8011459d99_1785345428_100` (artifact
  `b88cc6a991d1c7deb682345ddccef4ca27fb7123f2dea434e8f808aac8aeb622`).
- **E1-memory implementation round 4 (2026-07-29) changes requested, 3/3 participants, not
  degraded.** One blocking finding exposed a real Postgres semantic-path parity gap: pgvector used
  denormalized embedding scope columns and could discard a legacy workspace-only row before the
  canonical rank saw it. Pgvector now resolves memory and offset unit points to their owning memory
  and uses the same `memory_scopes`/`memory_workspaces` filter and rank as every SQL reader. The
  companion link finding was disproved by the full build and source audit: `hook_scope_labels_for_cwd`
  is exported by `cmd_hooks_scope.c`, declared in `cmd_hooks_scope.h`, included at the call site, and
  already used by two older paths in that file. Roundtable run:
  `oprun_g6a69bd8011459d99_1785346142_101` (artifact
  `2b566a923d83201ec109ab4f413e47674afd3d5a436f68988a28b38e23a19bc6`).
- **E1-memory implementation round 5 (2026-07-29) changes requested, 3/3 participants, not
  degraded.** The reported MCP default-context gap was disproved by the installed stdio path:
  `cli_mcp_serve` injects its real cwd into both the request envelope and tool arguments, and
  `handle_mcp_call_inner` preserves that metadata before `mcp_memory_scope_begin` resolves the
  repository identity. The existing search-memory proxy regression already exercised this exact
  no-override call. The proof is made explicit in code and test comments, and `cwd` is now documented
  in every ordered-memory MCP schema for direct clients that bypass the stdio proxy. Roundtable run:
  `oprun_g6a69bd8011459d99_1785346763_103` (artifact
  `5cd60248e8555ca6a24249a4ac4026a10350d3c5c7a1bf85c0698c9a31ca58e2`).
- **E1-memory local-first slice:** PR #2154 merged to `testing` as
  `ea612958c51b3a0541d950db1578350911740255` after all 23 CI checks passed. Every ordered memory
  reader now protects active-project and active-workspace evidence before relevance limits, while
  cross-project memory requires explicit `scope:"all"`.
- **E2 current-project identity candidate:** the implementation resolves a stable project from an
  explicit manifest ID, canonical forge identity, or persisted UUID; tracks checkout aliases and
  current generations; applies the same active-project default to code and knowledge retrieval; and
  gives detach, purge, and garbage collection exact-row confirmation manifests with verified audit
  attribution. Its frozen implementation diff receives a separate roundtable gate before PR.
- **E2 implementation round 1 (2026-07-29) changes requested, 3/3 participants, not degraded.**
  The panel caught two lifecycle defects: an ordinary active-checkout move incorrectly advanced the
  index generation, and the confirmation digest omitted its purge/GC operation discriminator. Moves
  now update aliases within the current generation, only a detached-project re-add advances it, and
  the digest and audit detail bind the operation. Regression coverage exercises move, detach,
  re-add, purge, and GC. The remaining findings arose because this intermediate E2 gate was submitted
  against the parent E0–E6 completion request rather than the accepted per-slice contract in §6; the
  reconvened review binds E2 as one mergeable step without claiming E3–E6 completion. Roundtable run:
  `roundtable-2e22a253e6ceac8fd078c4b3` (artifact
  `4933bdea264174938331ec34fc32a71312db7f9a43a8d498798cde21816457b9`).
- **E2 implementation round 2 (2026-07-29) changes requested, 3/3 participants, not degraded.**
  The panel found six boundary defects: all-project code queries applied their limit before the
  active-project preference; detach lacked verified WORM attribution; SQLite upgrades omitted the
  lifecycle tables and backfill; lifecycle CLI failures could exit successfully; PDF reads admitted
  unowned legacy rows; and malformed identified manifest entries could bypass ID validation. All six
  are accepted. The resulting fixes split active and excluding-tail candidate selection before
  limits across code, hybrid, ranked knowledge/document, and facet searches; make detach audit part
  of the same transaction; complete the compatibility migration; make lifecycle CLI errors nonzero;
  require exact PDF ownership; and fail closed on malformed explicit identities. Adversarial tests
  cover each boundary and the active-project-plus-`scope=all` transport shape. Roundtable run:
  `roundtable-d0acffc5106da13670646e30` (artifact
  `c71fd48c08c2a3bd78daa9c0cd9fc88c2310987816f7854009d37b73f5e1d74d`).
- **E2 implementation round 3 (2026-07-29) changes requested, 3/3 participants, not degraded.**
  The panel found two remaining request-boundary gaps: direct agent `search_docs` discarded the
  active project when `scope=all`, and code HTTP returned before checking a supplied preferred
  project's generation in that mode. Both are accepted: direct dispatch now uses the scoped client
  with the resolved project retained, and `project` plus `scope=all` validates its generation before
  retrieval. The third reported item was an index-visibility false positive: all five named
  PostgreSQL adapter helpers are declared and implemented in `db_postgres`, are already used by
  older DB2 modules, and both the shipping link gate and lifecycle test pass. Roundtable run:
  `roundtable-2393706c135262f501a9f1f8` (artifact
  `dad606e0f396399e8a78bcb8bf8ab0cdf6cfb21d3cd97dd5ccf9309e9df3915d`).
- **E2 implementation round 4 (2026-07-29) changes requested, 3/3 participants, not degraded.**
  The panel found that source rows were generation-keyed but several derived code/knowledge paths
  still overwrote or returned retired data. E2 now stamps and fences code embeddings, knowledge
  documents/assets/indexes, sketch buckets, curator work and artifacts, CSS migration/render state,
  and code projections; current-only force rebuild and startup sanitation preserve retained history,
  while exact manifest GC removes it deliberately. SQLite and PostgreSQL migrations make the
  derived uniqueness keys generation-aware, and adversarial fixtures retain the same logical key in
  two generations. Roundtable run: `roundtable-ff7f94ff43b4ec6dfff2882d` (artifact
  `55d334d8fd023d50826f89594f5c0deaa6a1ec01d87491cff9e8162dfa759619`).
- **E2 implementation round 5 (2026-07-30) changes requested, 3/3 participants, not degraded.**
  The sole reported blocker was disproved by the shipping build: the two
  `kb_search_json_scoped_ex` definitions are the mutually exclusive disabled and enabled arms of the
  file-level `#if AIMEE_DB2_DISABLED` guard. A comment now makes that compatibility layout explicit;
  no runtime behavior changed. Roundtable run: `roundtable-f779252e71a67116c2192e2d` (artifact
  `dbe4f9d3c9318296ae0bec683456de31b6599ec9862746920dfdbdf07747fc40`).
- **E2 implementation round 6 (2026-07-30) changes requested, 2/3 participants used,
  degraded.** One finding exposed a real all-project hybrid-search defect: reciprocal-rank fusion
  keyed candidates only by path, so identical paths in different projects collapsed and inherited
  the active-project label. Candidates are now interned and fused by exact project plus path, with
  an adversarial same-path fixture. The implementation also adopts the panel's stricter expression
  of the already-correct local-first bound: other-project symbol, text, and caller tails are excluded
  in SQL before their remaining-slot limit is applied. The two other claims were disproved: direct
  MCP code dispatch returns an explicit `project` argument (covered by the registry contract test),
  and the disabled/enabled search definitions remain preprocessor-exclusive. Roundtable run:
  `roundtable-b9a43c6c69927ce42470d6ff` (artifact
  `f7b8b9f8f1a65c34f02ba83537e6c276c9e5b72a950125289ca07ce1404b501d`).
- **E2 implementation round 7 (2026-07-30) changes requested, 3/3 participants, not degraded.**
  The exact full-diff call exceeded the MCP response deadline, but its completed durable synthesis
  was recovered as job 212. Its repeated blocking claim was disproved again: the helper returns a
  nonempty explicit project, the registry regression calls it and compares the returned value, and
  only the forbidden cwd-basename fallback returns `NULL`; that positive branch is now entirely
  visible in the diff. All five nonblocking hardening findings are accepted: lifecycle audit JSON
  preserves control bytes as Unicode escapes, the client no longer serializes an ignored principal,
  overlong project IDs fail at the HTTP boundary, in-transaction WORM appends assert transaction
  state, and purge tolerates absent legacy projection tables. Roundtable run:
  `roundtable-6e5200896cabbc1215616c18` (artifact
  `e2ccf65bd32da0a6a91aa6008d3f0450906f21c4de9d99a97fc3d17467abca59`).
- **E2 implementation round 8 (2026-07-30) changes requested, 1/3 participants used,
  degraded.** Two seats failed before review, but the surviving reviewer identified ambiguous SQL
  placeholder construction when artifact facets combined project, kind, and release filters. The
  release is safely inlined and the previous kind/project bindings were already distinct; E2 still
  adopts explicit monotonic parameter allocation so later predicates cannot reuse a slot. Combined
  release-plus-kind-plus-project regressions prove both positive projects and a mismatched kind.
  Roundtable run: `roundtable-d6b5adead972ca968a7132e5` (artifact
  `068609efd696a07bdd62a7cceaeb02ff97cc8298b4c1f703348f107de936635c`).
- **E2 final exact-diff review, 2026-07-30, APPROVED and converged, 3/3 participants, not
  degraded.** The final operator-health and explicit-scope smoke repairs received no blocking
  findings. Roundtable run: `roundtable-0b8300caa27872843d839e63` (artifact
  `96a646d79a1169a6f0d810b879f4db3ea4eaed6af66b9e286319a1f574161da6`).
- **E2 current-project identity slice:** PR #2161 merged to `testing` as
  `a70ebc23e1facd0cc199fbfacd2c13e2a38b1dca` after all 23 CI checks passed. Stable identity,
  active-project-first code/knowledge/memory ordering, generation fencing, explicit all-project
  scope, and audited detach/purge/GC are now the base for E3–E6.
- **E3 graph-resolution candidate:** the implementation replaces path-substring blast matching
  with exact normalized Python module identities, merges unique-symbol call edges without
  duplicates, admits cross-project tails only through resolved structural routes, resolves legacy
  projection basenames uniquely, and emits provenance/confidence/project/generation/freshness for
  every edge. Its frozen implementation diff receives a separate roundtable gate before PR.
- **E3 implementation round 1 (2026-07-30) changes requested, 3/3 participants, not
  degraded.** All three seats found that local co-edit projection edges were appended after the
  resolver's cross-project tail. A shared stable local-first partition now runs after additive
  projections in both public blast-radius paths, and an adversarial fixture combines a local-only
  projection with a route-gated external import and asserts that no local edge follows an external
  edge. Roundtable run: `roundtable-5d582db9c64174793488f921` (artifact
  `a309798e4843249ac313deecef3b9749cc318845c4b95498325216b374f3d4b1`).
- **E3 implementation round 2 (2026-07-30) changes requested, 3/3 participants, not
  degraded.** The corrected local-first ordering was accepted. The synthesis repeated a projection
  capacity concern that was already bounded by both loop conditions; E3 nevertheless adds the same
  guard at each insertion site so the invariant survives future loop refactors. A valid finding
  showed that the client accepted legacy-only arrays with empty metadata. New E3 clients now require
  resolved top-level identity plus complete structured edge arrays and fail closed on legacy-only or
  partial metadata; servers continue emitting legacy arrays additively for older consumers.
  Roundtable run: `roundtable-15472c66ae1dccd6620b4b97` (artifact
  `a8e925f87312534a4092416d4dec985621f5e0053be367bbab37d15b5be889fe`).
- **E3 implementation round 3, 2026-07-30, APPROVED and converged, 2/3 participants used,
  degraded.** The panel reported no blocking findings. E3 immediately adopts its schema-accuracy
  suggestion by splitting dependent/path and dependency/identity OpenAPI edge types so the required
  identity field matches the client validation contract. Its distinct CLI error-class suggestion is
  assigned to the accepted E5 typed-status slice. Roundtable run:
  `roundtable-a2953bf8d139a351d64af27d` (artifact
  `ce370ec61931cdfb91c1e73417de930af801280d5a486ab2de9cd7adec98ae85`).
- **E3 final exact-diff review, 2026-07-30, APPROVED and converged, 2/3 participants used,
  degraded.** The schema-aligned frozen diff received no findings; the chairman confirmed the
  earlier ordering, capacity, complete-metadata, exact-resolution, route-gating, and concrete edge
  identity findings remain closed. Roundtable run: `roundtable-327db1be95dc7e7c3abd2035`
  (artifact `ebe90912dc8e54a133c45453fd1c8b376c0c5845954acf66fe37134f1e0b17e4`).
- **E3 graph-resolution slice:** PR #2162 merged to `testing` as
  `fdc64b4abf3db262ea33efd0812b4208c654f57d` after all 25 CI checks passed. Exact normalized Python
  module resolution, deduplicated call/import/projection edges, current-generation provenance, and
  stable local-first graph ordering are now the base for E4–E6.
- **E4 task-conditioned retrieval candidate:** the implementation adds a strict
  `/v1/code/context` contract over hybrid RRF, exact active-project memory gating, explicit
  `no_answer`, a four-item/1,200-token packet, complete current-generation provenance, and
  `off|observe|on` first/new-task ingress behavior. `observe` remains the shipping default;
  unavailable, slow, stale, or incomplete evidence automatically suppresses model-visible packet
  injection and never widens to global memory. Its frozen implementation diff receives a separate
  roundtable gate before PR.
- **E4 implementation round 1 (2026-07-30) changes requested, 3/3 participants, not
  degraded.** Two concrete findings closed real grounding gaps: memory-only hybrid rows no longer
  count as answerable code, and explanatory project memory now requires an explicit path or symbol
  relationship to one accepted code result. The reported missing generation fence was already
  enforced by the shared `code_request_project` boundary; E4 adds stale/current route regressions so
  that inherited contract is visible in this slice. Three whole-program findings assessed E4 as if
  it claimed E5/E6 completion; the accepted delivery table requires one reviewed PR per slice, so
  the reconvened review binds this artifact to E4 without weakening the parent completion request.
  Roundtable run: `oprun_g6a69bd8011459d99_1785389737_122` (artifact
  `11776ad9f6708568dcc1dce374884b4cd164b2b3483438b06261beb39180462a`).
- **E4 implementation round 2, 2026-07-30, APPROVED and converged, 3/3 participants, not
  degraded.** The panel reported no blocking findings after the memory-only, explicit-anchor, and
  generation-regression repairs. Its schema nit is adopted by removing redundant undocumented
  top-level caller fields; the span remains the canonical symbol/line surface. The validation record
  now distinguishes 85 routed KB endpoints from 92 OpenAPI operations, calls out the legacy hybrid
  route's local-first memory tightening, and states the direct-handler compatibility boundary. The
  eight-row memory read is deliberately a bounded pre-filter pool, not an emitted packet allowance:
  kind, exact scope, grounding, and duplicate rejection run before the shared four-item output cap.
  The resolver is covered through ingress behavior here; a standalone resolver fixture is a useful
  follow-up but not a blocking gap in the E4 contract. Roundtable run:
  `oprun_g6a69bd8011459d99_1785391257_123` (artifact
  `743c03a1a1ec1191dbff96008f88cdb064ef51adda8fccc3634a1e9959f05852`).
- **E4 final exact-diff review, 2026-07-30, APPROVED and converged, 3/3 participants, not
  degraded.** The panel confirmed the post-review schema and documentation cleanup with no blocking
  findings. E4 immediately adopts its bounded-confidence suggestion: ingress now rejects code or
  memory confidence outside `(0,1]`, with adversarial packet tests. Retrying a failed first-turn
  retrieval on a related follow-up is assigned to E5's typed recovery policy; E4 continues to
  suppress the failed packet and never widens recall. Roundtable run:
  `oprun_g6a69bd8011459d99_1785391610_124` (artifact
  `b8def8883da6a7e97997f223dad4d1c4b9ac1ea9f88b0a91e251e9c5c36b1a09`).
- **E4 confidence-amended exact-diff review, 2026-07-30, APPROVED and converged, 3/3
  participants, not degraded.** The panel approved the confidence-bounded artifact with no blocking
  findings. Its remaining suggestions do not change the accepted contract: `scope=project` is the
  normative memory field while numeric rank remains diagnostic; the hybrid HTTP boundary has no
  durable workspace identity to forward; redundant ambient fields are ignored when an explicit
  scope is present; and token-heuristic/duplicate-check refinements require observed evaluation
  failures rather than speculative widening. Named policy constants are a readability follow-up.
  Roundtable run: `oprun_g6a69bd8011459d99_1785391948_125` (artifact
  `abdfa92e2489d4c73cdb05c18ec067fb8463296c5ed45eb376fe66bb70e986ac`).
- **E4 task-conditioned retrieval slice:** PR #2171 merged to `testing` as
  `7a8a20753c37eba57fefdbe91fd256b2b81e0f91` after all 24 CI checks passed. Strict current-project
  context, explicit abstention, bounded observe/on ingress, complete provenance, and automatic
  suppression are now the base for E5–E6; `observe` remains the shipping default.
- **E5a dependency-status candidate:** the implementation adds exact `ok|empty|abstained|stale|
  unavailable|unauthorized` classification, independent KB-transport and external-embedder circuit
  breakers, bounded exponential jitter, one half-open recovery probe, dependency-specific outage
  metadata, and exact session/project retry after an unavailable first-turn lookup. A reachable KB's
  embedder/vector-store outage does not poison its transport breaker, local coding remains
  independent, and no agent-facing memory/index reader converts an outage into an empty result. Its
  frozen implementation diff receives a separate roundtable gate before PR.
- **E5b deployment-readiness candidate:** server liveness remains independent of recoverable
  dependency failure, while readiness now fails closed on the complete retrieval contract and
  reports the failed boundary, E5a breaker state/retry delay, and last successful query/ingest.
  Shipped Compose restart policy remains `unless-stopped` with liveness healthchecks; the operator
  runbook pins separate readiness admission, queue diagnostics, evidence preservation, and safe
  dependency-specific recovery. Its frozen implementation diff receives a separate roundtable gate
  before PR.
- **E5b final exact-diff review, 2026-07-30, APPROVED and converged, 3/3 participants, not
  degraded.** The final runbook, explicit dependency boundary, JSON-safe diagnostics, and
  retrieval-only/null-diagnostics regressions received no findings. Roundtable run:
  `oprun_g6a6b1ed93980fd5d_1785408651_7` (artifact
  `0f0e27dbb596f737174e815004baf9a71b904127185a7af05809c747bdceb8e0`).
- **E5b deployment and runbook slice:** PR #2177 merged to `testing` as
  `456444cad5a1f4b8e8595ae91e5c3923f4cef468` after all 23 CI checks passed. Liveness remains
  restart-safe during recoverable outages; readiness now enforces the retrieval contract with
  bounded dependency diagnostics and a safe recovery runbook.
- **E5c resumable-experiment candidate:** a repository-owned runner binds one immutable plan,
  run ID, and named checkpoint; writes every cell attempt before checkpoint advancement; preserves
  stdout/stderr/timing/return code; marks failed infrastructure `score_eligible:false`; refuses
  overwrite and cross-run checkpoint provenance; and resumes only unfinished cells. Its frozen
  implementation diff receives a separate roundtable gate before PR.
- **E5c final exact-diff review, 2026-07-30, APPROVED and converged, 3/3 participants, not
  degraded.** The final runner provides byte-lossless, digest-bound attempt evidence, durable
  checkpoint ordering, strict plain-path/provenance/plan validation, infrastructure-invalid score
  exclusion, and non-splicing named resume. Roundtable run:
  `oprun_g6a6b1ed93980fd5d_1785414928_21` (artifact
  `50dd19a6189e0cb0173910380ac9c054d4733da567603dd39db3277a18d1dfce`).
- **E5a implementation round 1 (2026-07-30) changes requested, 3/3 participants, not
  degraded.** The implementation received no bounded-slice technical ruling because the review
  brief exposed the unfinished parent E5/E6 program. The next review explicitly binds the frozen
  diff to E5a as the independently mergeable slice required by §6. Roundtable run:
  `oprun_g6a69bd8011459d99_1785396259_129`.
- **E5a implementation round 2 (2026-07-30) changes requested, 2/3 participants used,
  degraded.** The scoped review found that the status envelope omitted a concrete bounded retry
  delay and that vector-dimension staleness evidence was lost across the KB client boundary. E5a now
  carries both contracts end to end with regressions. Roundtable run:
  `oprun_g6a69bd8011459d99_1785396774_130`.
- **E5a implementation round 3 (2026-07-30) changes requested, 3/3 participants, not
  degraded.** Four remaining boundary findings were accepted: valid no-answer is `abstained`, retry
  delays have a one-second floor, an exact memory miss remains distinct from transport failure, and
  vector-status documentation applies unconditionally. The fixtures and docs now enforce all four.
  Roundtable run: `oprun_g6a69bd8011459d99_1785397781_131`.
- **E5a implementation round 4 (2026-07-30) changes requested, 2/3 participants used,
  degraded.** The panel found three concrete non-2xx/parser gaps: mTLS GET/POST bodies could bypass
  HTTP status rejection, a malformed project-list object could become an empty list, and external
  embedder 401/403 could become transient unavailability. E5a now preserves the typed mTLS status
  while rejecting its body, requires a real `projects` array, and returns non-retryable embedder
  unauthorized without poisoning the transient-failure breaker. Adversarial fixtures cover each
  transport. Roundtable run: `oprun_g6a69bd8011459d99_1785399544_132` (artifact
  `0691c47bbc47d1854c00df172d01745bd29eab47956d43e22bca01fe3462d812`).
- **E5a dependency-status slice:** PR #2176 merged to `testing` as
  `ada6d21d9ac4461b9a144acdc1d8b5c1b43718c3` after all required checks passed. Typed dependency
  outcomes, bounded breakers, truthful authorization/staleness metadata, and exact recovery probes
  are the merged base for deployment and evaluation.
- **E5c resumable-experiment slice:** PR #2179 merged to `testing` as
  `930bbe995502c9f584d895f4900b0e3562582030`. The repository now owns durable named checkpoints,
  byte-lossless attempt artifacts, infrastructure-invalid exclusions, and anti-splicing validation.
- **E6 evaluation candidate:** the scorer, 16-case retrieval corpus, eight-task/four-arm coding
  matrix, and prompt fixture v1 are pinned to the E5c merge. The deterministic retrieval layer
  passes, but every provider-backed paired arm has zero eligible fresh cells. Promotion therefore
  fails closed: `observe` remains the default, no historical artifacts are spliced, and the bounded
  paired matrix is assigned to `agent-facing-code-intelligence-paired-evaluation.md`.
- **E6 evaluation review round 1 (2026-07-30) changes requested, 3/3 participants, not
  degraded.** Three blocking fail-closed gaps were accepted: a partial arm could count as complete,
  a partial retrieval corpus could pass, and missing/unknown evidence could improve or disappear
  from metrics. The scorer now requires exact corpus and arm/task coverage, explicit evidence
  fields, unique known IDs, and complete eligible denominators before promotion. Roundtable run:
  `oprun_g6a6b1ed93980fd5d_1785417195_27` (artifact
  `96c46ab9ca452d5a1e04576dd196eab5704549743cc094df015667c1e684904f`).
- **E6 evaluation review round 2 (2026-07-30) changes requested, 2/3 participants used,
  degraded.** The exact coverage repairs passed, but eligible coding metrics still had presence-only
  validation. The scorer now requires explicit booleans plus finite non-negative token, wall,
  retrieval latency, and packet metrics, and bounded finite edge rates before scoring. Roundtable
  run: `oprun_g6a6b1ed93980fd5d_1785417454_28` (artifact
  `51d2314b8a2b98e8a2b59e23068097726bb898c0ae0695e40ba8d0f3eee0e792`).
- **E6 evaluation review round 3, 2026-07-30, APPROVED and converged, 3/3 participants, not
  degraded.** The complete fail-closed scorer, pinned corpus/results, truthful zero paired-agent
  denominator, `retain-observe` decision, archived parent, and bounded residual proposal received no
  findings. Roundtable run: `oprun_g6a6b1ed93980fd5d_1785417816_29` (artifact
  `fa1a46d6eb522bc9e398a6df49a79e68aae728e07f2c399e137649eda2a2f1d1`).
- **E6 evaluation and reconciliation slice:** PR #2180 merged to `testing` as
  `6969b2bca56f7d1d278e0796ce1213f40c3bad51` after all 24 CI checks passed. E0–E6 are reconciled;
  deterministic retrieval gates pass, paired-agent promotion does not, `observe` remains the
  shipping default, and the exact remaining provider-backed matrix is bounded by the linked
  residual proposal.
- **Provider-backed promotion completion:** the residual 8-task × 4-arm matrix ran from merged
  `aa8c40e9d75449774c9b0b630bb8f1037efb8097` with 32/32 eligible cells. `on` improved task success
  from 5/8 to 6/8, improved median wall by 13.59%, consumed packets in 8/8 answerable cells, and
  passed the confidence, latency, and packet bounds. The accepted default is therefore promoted
  from `observe` to `on`; both rollback modes remain supported.
