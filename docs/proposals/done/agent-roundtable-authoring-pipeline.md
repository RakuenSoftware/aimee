# Proposal: Agent roundtable authoring pipeline (idea → reviewed proposal → implementation → reviewed PR)

- **State:** done — implemented and merged to `testing` across 17 commits
  (P0–P4 + 11 implementation-review rounds + chunked-review + first-class MCP;
  PR #193). Full unit suite green (`-Werror`, build-integrity clean); 6 pipeline
  test files / 37 test fns cover the §10 matrix. A post-merge completeness
  roundtable then closed four findings (§55 gate exactly-once CAS,
  mergeable=UNKNOWN merge policy, fail-reason brief fidelity, per-phase cost
  whitelist). Original branch `feat/roundtable-authoring-pipeline`.
- **Author:** JBailes
- **Date:** 2026-06-11 (consolidated — 23 PR-#183 review rounds / 57 findings folded into the design sections and indexed in Appendix A)
- **Charter roles:** Orchestrate (pipeline state machine), Draft/Review
  (roundtable application), Gate-Promote (human pass/fail gates), Calibrate
  (done-bar + pass ceiling config), Persist (resumable ledger).
- **Scope:** a driving-agent orchestration spec + a durable persisted pipeline
  ledger. Net-new code is small: a namespaced `roundtable_pipeline_runs` ledger
  or an explicit migration/extension of the existing DB1 `pipelines` schema
  (not the transient `/v1/runs` live store), config keys
  (`src/headers/config.h`, `src/config.c`, `src/config_fields.c`,
  `src/config_sections.c`, `src/config_save.c`), and the
  driving-agent runbook/skill. It **depends on** the roundtable engine (already on
  `testing`) and the in-tree `ensemble_review` review surface, with the remaining
  review contract narrowed to stable terminal result shape/payload semantics. It
  routes PR/merge/diff work through Aimee's workspace-aware git/PR tools rather
  than assuming a raw local `gh`/`git` shell, and it may need a narrowly-scoped
  auth/capability-policy change for human gate resolution. No changes to the
  roundtable engine itself.

## Implementation decisions (resolved open questions, §12)

The implementation resolves the §12 open questions as follows; the design sections
below remain authoritative for everything else.

- **Ledger home (#1/#2):** a **new namespaced DB1 table set** —
  `roundtable_pipeline_runs` + `roundtable_pipeline_passes` +
  `roundtable_pipeline_attempts` + `roundtable_pipeline_gates`
  (`src/db1/schema.sql`, domain API in `src/db1/roundtable_pipeline.{c,h}`). The
  autopilot `pipelines` table is left untouched, so old autopilot actions keep
  working and the two namespaces never collide.
- **CLI/API namespace (#3/#45):** a new `aimee pipeline …` CLI/MCP surface
  (`status`, `gate`, `list`, `cancel`, `resume`, plus the loop driver), distinct
  from `autopilot`. Any HTTP exposure uses `/v1/roundtable/pipelines/…`, never the
  KB/corpus `GET /v1/pipeline/status`.
- **Gate authority (#53/#54):** **no new capability bit** (the 16-bit `CAPS_ALL`
  mask is full). Gate *resolution* (`gate pass|fail`) is **operator/local-out-of-band
  only** — it requires a local (UDS) operator principal and is refused over the
  TCP/reverse-channel surface a delegate-driving session uses. The driving agent
  may surface a gate (move to `*_pending`, build the digest), check status, refresh
  the digest, cancel, and resume, but it can never pass its own gate.
- **Parked-gate admission (#48):** controlled by
  `roundtable_pipeline_parked_releases_slot` (default **true** — a parked gate
  releases the single active slot), with branch/PR ownership guards preventing two
  runs from mutating the same recorded branch/PR.
- **Cost scope (#49/#51):** the total cap is **roundtable-child-run cost only**
  (`roundtable_pipeline_max_total_cost_usd` scoped to `roundtable_only`) until the
  cost-accounting `usage_ledger` lands; the pinned `cost_scope`/`cost_source`/
  `cost_version` are recorded per pipeline and a switch requires re-reconciliation.
- **Per-phase config:** one shared key set for v1; per-phase `…_proposal`/`…_pr`
  overrides are deferred (§6).
- **Chunk index (#34/#42/#47):** chunks are **derived on demand from the retained
  whole origin** (an `origin:<hash>` ref + the origin content hash), not a
  separate durable store. Re-deriving each pass gives per-pass freshness (#42)
  and parked-gate release (#47) at no cost, and the origin is always recoverable
  (#34). A chunked review is a **group of passes** (N chunk-passes + 1 synthesis
  pass sharing a `chunk_group`), reusing the existing capture/seam machinery; the
  phase passes only when every chunk member is valid and the synthesis member is
  done (#28). v1 does not use a db2 review-scoped KB index.

## Design at a glance

A closed-loop authoring pipeline that turns a one-line idea into a merged,
panel-reviewed implementation, with **two human gates** and **two roundtable
quality gates**:

```
[Human] idea
  → DRAFT roundtable generates a first proposal skeleton (8KB-capped; expanded in review)
  → REVIEW roundtable ⇄ author-revise  (until done-bar)        ← proposal quality gate
  → open proposal PR
  → [Human gate 1] pass / fail
        fail → back to proposal REVIEW loop (fail reason → brief)
        pass → merge proposal PR → implement
  → REVIEW roundtable ⇄ agent-fix  (until done-bar)            ← PR quality gate
  → [Human gate 2] pass / fail
        fail → back to implement + PR REVIEW loop (fail reason → brief)
        pass → merge implementation PR → done
```

The roundtable runs **as many passes as correctness takes** — depth is the point.
The done-bar is a *correctness* condition, not a pass budget; the configurable
pass ceiling is only an operator cost-backstop that **escalates to the human**
when hit without reaching the done-bar (it never auto-passes a not-done artifact).

**Design invariant — always keep the origin artifact.** Chunking is allowed
anywhere in the pipeline, but it is never a substitute for the whole: every
artifact (proposal, diff) has a durable whole-origin ref/hash in the pipeline
ledger (working blob/path, db2 `docs.normalized_text`, or an explicit artifact
payload), and chunk rows point back to that origin. `kb_documents` is a chunk
index, not the whole-origin store. Chunk to fit a call; retrieve the origin when
correctness needs it.

> The design below is authoritative and self-contained. It was hardened over 23
> review rounds; every `(#NN)` reference resolves to **Appendix A — Decision log**
> at the end, which indexes all 57 findings and their in-tree grounding.

## Relationship to existing proposals

- **The roundtable engine is done** (`docs/proposals/done/agent-roundtable-collaborative-drafting.md`,
  PRs #136/#142, on `testing`). `delegate_roundtable_run`
  (`src/headers/delegate_ensemble.h`) provides both `ROUNDTABLE_DRAFT` (produces an
  improved `artifact`) and `ROUNDTABLE_REVIEW` (produces deduped, severity-tagged
  `items[]` with a corroboration `count`, plus `answered_questions[]` and
  `coverage_gaps[]`), a deterministic `converged` predicate, `best_round`, and
  inherited cost/deadline bounds. **This proposal adds no engine code.**
- **`agent-directed-pr-review.md` (pending) is no longer a broad hard dependency
  for review mode.** The current tree already exposes `ensemble_review` with a
  directed brief and structured roundtable result over the async bridge. This
  pipeline depends only on the stable interface contract that proposal documents:
  terminal result shape, polling/cancellation semantics, payload bounds, and the
  item/artifact provenance fields. Draft callability and durable pipeline
  orchestration remain this proposal's responsibility.
- **`agent-roundtable-collaborative-drafting.md` (pending residual)** lists
  deeper convergence/economics tests; this proposal's validation (§10) exercises
  exactly those at pipeline scale.

## What exists vs. what is net-new

| Capability | Status |
|---|---|
| Multi-round panel, DRAFT + REVIEW modes, deterministic convergence, dedup, severity, cost/deadline bounds | **exists** (engine, `testing`) |
| Directed review (brief), structured items returned, `ensemble_review` MCP tool (review mode) | **in tree on `testing`** (`mcp_tools.c:610`, `handle_mcp_ensemble_review`); verify result contract (§8) |
| A **draft**-capable callable path | **gap** — `ensemble_review` forces review mode; DRAFT needs `/v1/delegate/roundtable --mode draft` or a draft MCP sibling (§8) |
| PR **open** / diff capture | **exists** (`git_pr` create/view/edit, `mcp_git_run`) |
| PR **merge** | **gap — `git_pr` has no merge action** (only `merge_status`); needs a merge executor (#50, §5) |
| Outer REVIEW⇄revise loop, done-bar evaluation, pass ceiling + escalation | **net-new** (§3) |
| The two human gates + the two fail-return edges | **net-new** (§5) |
| Persisted, resumable roundtable pipeline ledger (hybrid state) | **net-new or explicit DB1 pipeline extension** (§4) |
| Config: done-bar, pass ceiling, outer cost cap | **net-new** (§6) |

## §1 The pipeline state machine

States (persisted in the §4 ledger):

`drafting → proposal_review → gate1_pending → gate1_merge_pending → implementing → pr_review → gate2_pending → gate2_merge_pending → done` (plus `failed`/`abandoned`).

Transitions:

1. **drafting** — DRAFT roundtable turns the human idea (+ any seed brief) into a
   first proposal **skeleton**, not a finished proposal. One roundtable call in
   `ROUNDTABLE_DRAFT` mode (through `/v1/delegate/roundtable` / `aimee delegate
   roundtable --mode draft`, or an explicit draft-capable MCP surface) produces the
   working `artifact` — but that artifact is `char[8192]`-capped (#9), so DRAFT
   yields a section outline + goal/scope, which the `proposal_review` author-revise
   loop expands section by section. A full proposal is never expected from one
   DRAFT call. The pipeline creates or selects a dedicated proposal branch/worktree
   before writing the proposal file.
2. **proposal_review** — the §3 outer loop: REVIEW the draft, author-revise from
   the items, re-REVIEW, until the done-bar (§3). Then the agent opens the proposal
   PR through the workspace-aware PR surface (base `testing`, per repo flow) and
   moves to `gate1_pending`.
3. **gate1_pending** — human gate 1 (§5). **pass** records the verdict and a
   durable merge intent, then moves to `gate1_merge_pending`; successful merge
   reconciliation moves to `implementing`. **fail** → the human's reason is
   appended to the brief and the state returns to `proposal_review`, which
   re-revises and **pushes to the same recorded proposal branch/PR** (never a
   duplicate, #43).
4. **implementing** — the agent implements the merged proposal on a dedicated
   implementation branch/worktree (normal coding; not a roundtable activity),
   opens the implementation PR, captures the diff.
5. **pr_review** — the §3 outer loop in REVIEW mode over the diff: REVIEW,
   agent-fix, re-REVIEW, until the done-bar.
6. **gate2_pending** — human gate 2 (§5). **pass** records the verdict and a
   durable merge intent, then moves to `gate2_merge_pending`; successful merge
   reconciliation moves to `done`. **fail** → reason → brief, state returns to
   `implementing`, which pushes the fix to the **same recorded implementation
   branch/PR** (#43).

Every state is durable (§4) so a human gate can be answered hours or days later,
across a server restart or a new agent session. For v1 admission control, only
one pipeline may be **active** in drafting/review/implementing at a time; an
unanswered gate may either keep that active slot or enter an explicit
`parked`/`waiting_human` class that releases it (#48). The implementation must
choose one rule and record it in the ledger before allowing another run to start.

## §2 The two roundtable applications

Both phases use the **same** engine; they differ only in input and brief.

- **Proposal phase (B).** Input = proposal markdown. **Both** modes (per decision):
  DRAFT to generate the first **skeleton** in `drafting` (8 KB-capped, #9), then
  REVIEW to gate it in `proposal_review`, with the driving agent expanding the
  skeleton into the full proposal across the author-revise passes. The full
  proposal lives in the working file/PR, not in the DRAFT artifact; REVIEW reviews
  the file (or chunked sections for large proposals, like the diff path below),
  not the truncated artifact. Pipeline-owned DRAFT and REVIEW calls must use the
  async op-run path so server-worker result capture runs; the DRAFT pass is
  captured as an artifact **ref + hash** (mode-appropriate, #27), with the skeleton
  text written to the working file, not stored as a ledger blob. The brief carries
  the proposal's *goal*, the *invariants* it must satisfy, and the human's seed
  *questions*.
- **PR phase (A).** Input = unified diff (normally `git diff <base>...HEAD`;
  aimee has no PR-fetch and needs none — `agent-directed-pr-review` §6). REVIEW
  mode only; the agent applies fixes between passes. For large diffs, the driving
  agent must pass an artifact file/path or chunked diff slices rather than an
  unbounded inline string, and the ledger stores the diff ref plus content hash.
  Chunking is a pass-level manifest, not a set of unrelated mini-reviews: every
  chunk stores an input hash and result, the pass stores the aggregate, and the
  done-bar can pass only after all chunks are covered plus a whole-artifact
  synthesis/check has considered cross-chunk invariants (#28). **Chunking never
  discards the origin:** the pipeline records a whole-origin ref/hash separately
  from chunk rows (#34), and the **orchestrator** retrieves needed spans from the
  origin plus review-scoped chunks and hands each review call a self-contained
  inline unit — panelists are not assumed to retrieve or read files (#32/#35/#37).
  Each assembled unit records an assembly manifest with selected span refs/ranges/
  hashes, token/byte budget, and omitted candidates; required omissions block the
  aggregate instead of being hidden (#39). **Every revise pass re-ingests the new
  artifact version and supersedes the prior version's review-scoped chunks** (#42),
  so retrieval is always pinned to the current pass's content hash and never serves
  a span from an earlier revision.
  Pipeline-owned PR reviews use the async op-run path with validated pass
  metadata, not a direct synchronous roundtable dispatch. The brief carries the
  *fixes just applied*, the *invariants* the change must not break, and the
  *questions* the author is unsure about.

The brief is **open-mandate** (agent-directed-pr-review §2): it weights attention
and seeds the questions, but every reviewer is still told to report any blocking
issue even outside the focus. Direction must never become a filter.

## §3 The outer loop, the done-bar, and the pass ceiling

Two distinct loop levels — keep them un-confused:

- **Inner rounds:** rounds *within one* `delegate_roundtable_run` call
  (`roundtable_max_rounds`, default 3). Engine-owned, unchanged.
- **Outer passes:** REVIEW → revise → re-REVIEW cycles the *pipeline* drives. This
  is what the human means by "passes" and what §6's ceiling bounds.

**Done-bar (configurable; correctness condition, not a budget).** A phase leaves
its review loop only when the latest REVIEW result satisfies the configured bar,
read from the captured terminal envelope built from real engine fields plus
serialization/attempt state:

Every bar first requires `roundtable_terminal_envelope_valid(envelope) == true` —
the **single canonical validity predicate** (#41/#44), evaluated over the captured
terminal envelope rather than only raw `roundtable_result_t`. It is false on any
of `truncated`, `items_truncated`, `degraded`, `cost_capped`, `deadline_hit`,
`cancelled`, `lost_result`, capture/parse failure, or an
`items_round`/`artifact_round` provenance mismatch. The done-bar, the
chunk-aggregate, and the gate digest all consult this one predicate rather than
re-listing flags inline, so no consumer can pass on a subset. The worker capture
hook records every terminal envelope first and only then marks whether that
envelope is valid for done-bar use; invalid evidence is still durable. For
review-mode done-bars the surfaced `artifact_round`/`best_round` provenance must
match the evaluated `items_round`, and the gate digest explains any mismatch
rather than silently passing.

- `zero_blocking` *(default)* — `converged == true` and no `items[]` of
  `severity == "blocking"`. Suggestions/nits are surfaced in the gate digest but
  don't block.
- `zero_blocking_suggestions` — also requires no `suggestion`-severity items
  (nits allowed). Stricter, more passes.
- `zero_blocking_questions_answered` — `zero_blocking` plus every accepted brief
  question present in `answered_questions[]` and `coverage_gaps[]` empty. The
  brief must contain at most `ROUNDTABLE_MAX_QUESTIONS` (16) questions or be split
  into multiple review passes; overflow is not treated as answered.

The driving agent computes the bar from the structured items
(agent-directed-pr-review P1b); it never re-judges convergence itself — it trusts
the engine's deterministic saturation logic as exposed through `converged`.

For chunked reviews (#28), the done-bar is evaluated on the **aggregate pass
result**, not on any one child chunk. Every manifest entry must have a completed,
valid REVIEW result over the expected chunk hash, and the aggregate must include a
whole-artifact synthesis/check for cross-chunk findings before the phase can pass.
Any missing, stale, truncated, degraded, or failed chunk keeps the aggregate
blocked. The whole-artifact check is **retrieval-backed, but the orchestrator
retrieves — not the panelists** (#32/#37): the pipeline stores the origin
separately (#34) and indexes review-scoped chunks, and the **orchestrator** pulls
the cross-chunk spans the invariants need (the other definition of a symbol, a
missing section, a renamed reference) via vector/FTS + the `prev_chunk_id`/
`next_chunk_id` chain, then passes a **self-contained inline unit** to the
aggregate review call. A heterogeneous, possibly-remote panelist is never assumed
to retrieve or read files; it reviews the inline digest. The full text is never
needed in one context window, and the origin is always retained, never replaced by
its chunks. The aggregate also validates each assembled inline unit's budget and
manifest (#39): selected spans must hash back to the retained origin/chunks, any
omitted required span is a coverage gap, and no unit may pass if the manifest says
the resolved minimum participant context budget was exceeded.

**Pass ceiling (configurable; cost backstop, not an early-exit).** `roundtable_pipeline_max_passes`
bounds outer passes. **Default 0 = unbounded** — review runs until the done-bar,
honoring correctness-over-pass-count. When an operator sets a positive cap and the
loop reaches it *without* meeting the done-bar, the agent **escalates to the
human** (surfaces the current artifact + the remaining blocking items + cost) and
**does not auto-pass**. A cap is a budget guard, never a way to ship a not-correct
artifact.

**Echo guard.** Between passes the brief carries the *author's fixes/changes*, not
the panel's verbatim prior findings, so a fresh panel re-derives rather than
anchors on its own prior output. (The engine already dedupes within a call; this
prevents cross-pass echo.)

**Non-termination backstop.** Besides the optional pass ceiling, the inherited
`ensemble_max_cost_usd` (per call), a cumulative `roundtable_pipeline_max_cost_usd`
(per phase), and a cumulative total cap (§6) bound spend; any tripping escalates
to the human rather than silently passing. **The per-phase cap accumulates across
fail-return re-entries of that phase — a fail-return never resets it** (#46). The
total cap's accounting scope must be explicit (#49): either it covers roundtable
child-run costs only, or it also includes normal implementation-agent spend from
the existing token/cost accounting and treats unknown implementation cost as
incomplete evidence. Infrastructure retries under one pass id are separately
bounded by `roundtable_pipeline_max_attempts_per_pass` (#29) so capture failures
do not consume correctness passes but also cannot spin forever.

## §4 Hybrid state — the resumable pipeline ledger

The agent drives the loop, but pipeline state is **persisted** so it survives a
restart and a human gate can be answered later (the decision). This must be a DB1
checkpoint surface, not `/v1/runs`: the current `openai_runs_store` is a bounded,
in-process live store and is not durable across restart. `/v1/runs` IDs are child
execution handles that may be stored in pass history, never the source of truth
for the pipeline.

Aimee already has a durable DB1 `pipelines` table for autopilot-style plan/job
state (`task`, `status`, `current_phase`, `plan_id`, `job_id`, attempts, and
classification). That schema is too narrow for this workflow, so implementation
must choose one explicit path:

- create a namespaced `roundtable_pipeline_runs` table plus child
  `roundtable_pipeline_passes` / `roundtable_pipeline_gates` tables; or
- migrate/extend the existing `pipelines` table in a backwards-compatible way,
  with the old autopilot actions continuing to work.

Either way, the durable record holds:

- pipeline id, state (§1), phase, admission class (`active` vs `parked` /
  `waiting_human` if parked gates release the single active slot), created/updated
  timestamps, and schema version (#48);
- artifact refs: proposal path/blob/ref, implementation branch, diff ref or
  chunk manifest, whole-origin ref/hash (working blob/path, db2 `docs` row, or
  explicit artifact payload), review-chunk index refs (`kb_documents` rows if that
  backend is used), and content hashes — not unbounded inline text as the primary
  representation;
- assembly manifests for orchestrator-built review units and aggregate checks:
  selected origin/chunk span refs, ranges, hashes, input budget estimates, and
  omitted candidate spans (#39);
- the current brief plus compact gate digest;
- per-pass history: each outer pass's status, aggregate child `run_id` if
  single-call, `converged`, blocking/suggestion counts, `cost_usd`, `rounds_run`,
  `best_round`, result hash, and any payload/chunk refs; for chunked passes, a
  manifest plus aggregate row that records per-chunk results and the
  whole-artifact synthesis/check (#28);
- per-item provenance sidecars keyed by item identity/result hash, mapping
  findings back to assembly spans, source refs, and participant sources; compact
  `item.sources` strings are display hints only (#40);
- per-attempt history under each pass: `attempt_no`, child `run_id`, submitted and
  terminal timestamps, captured/lost status, terminal envelope parse status,
  HTTP serialization flags (`items_truncated`), engine terminal flags, result
  snapshot/hash, and cost if the result was available (#44);
- repository/worktree state: repo root, remote, base branch, head branch,
  workspace id/provider (`shared`, `detached`, or `mirror`), dedicated worktree
  path if any, head/base commit SHAs, dirty-state snapshot, PR number(s), PR URLs,
  merge SHAs, forge-token/`gh` authority status, and last checked mergeability;
- the human-gate verdicts, fail reasons, actor/timestamp, resume action taken,
  merge-intent / merge-pending records keyed by PR number + expected head SHA
  (#56), and cost-scope evidence for total-cap accounting, including whether
  implementation-agent spend is included, unknown, or out of scope (#49).

**Result capture is server-worker-owned, not agent-poll (#10/#12/#18).** The
done-bar is read from the roundtable run's structured result, but that lives in
`/v1/runs` / `openai_runs_store`, which is bounded (`OPENAI_RUNS_STORE_MAX 256`,
oldest-reused) and non-durable. Capture must not depend on the *driving agent*
being awake to poll — under Hybrid the agent owns the loop, not the child-run
completion path. Instead, the worker that **already runs the roundtable** owns
capture: `op_run_worker_run` (`server_http_routes.inc:339`) executes the async
`delegate.roundtable` and calls `openai_runs_store_finalize` at terminal. The
pipeline creates a DB1 pass row first, then the pipeline-owned callable surface
forwards a normal `pipeline_pass_id` that is validated against the active
pipeline/pass state and injected internally as private `__pipeline_pass_id` (raw
double-underscore fields are not user API). `server_http_submit_op_run` /
`rh_dispatch_op_async` must preserve that private metadata alongside the existing
`__run_id` injection (`:419`). Extend the finalize path, preferably through a
narrow op-run-finalize hook rather than hard-coding roundtable ledger writes into
generic HTTP routing, to **also persist the pass result** into the DB1 ledger row
before the run record can be evicted. The hook **no-ops when `__pipeline_pass_id`
is absent**, so ordinary `ensemble_review`/roundtable calls are untouched. What it
persists is the terminal **capture envelope** first (#44): run id, attempt status,
raw/sanitized result snapshot hash, parse status, HTTP serialization flags, and
engine flags when available. The mode-specific parsed fields then hang off that
envelope (#27): for a REVIEW pass, the items + severities + `count`, `converged`,
validity flags, `items_round`, `artifact_round`, counts, `cost_usd`, `rounds_run`,
and `items_truncated` when the HTTP response serialized only a prefix of the item
list (#38); for a DRAFT pass, the artifact **ref + content hash** +
`best_round`/`artifact_round` + validity flags (the skeleton text itself lands in
the working file, not as a ledger blob, per #9). Failed, cancelled, malformed, and
capture-failed terminal states are recorded too. The canonical validity predicate
is evaluated **after** this record is durable; it controls done-bar eligibility,
not whether the attempt is captured. The `/v1/runs` id is retained only as a
historical pointer; the ledger row is the source of truth for the done-bar and the
gate digest.

Cost evidence in that ledger is not just a number. Each pipeline and phase records
the accounting scope/source/version used to compute it (#52): roundtable-result
child-run cost, DB1 `token_audit` / `cost_fold_log`, or the cost-accounting
proposal's db2 `usage_ledger`. A resumed pipeline may continue with the pinned
source, or explicitly reconcile to a newer authoritative source and mark prior
gate evidence stale until the digest is regenerated; it cannot mix sources inside
one cap decision.

**The agent reads terminal from the ledger, not `/v1/runs` (#26).** Because the
worker writes the result to DB1 at terminal, the driving agent learns a pass
completed by polling the **durable ledger pass row** (`aimee pipeline status` / the
row's `captured`/terminal marker), never by polling `/v1/runs/{id}`. `/v1/runs`
remains a best-effort live view; the ledger row is what the agent waits on, so the
eviction race is closed on the read side as well as the write side.

If a result is still not captured (a genuine fault), the attempt is marked
`lost_result` and the pipeline **auto re-runs the review pass** under the same
stable `pipeline_pass_id` but a new `attempt_no`/`run_id`. That retry is idempotent
only if the stored artifact/diff hash still matches; if the input changed, the old
pass is stale and the pipeline starts a new pass instead. Retries are bounded by
`roundtable_pipeline_max_attempts_per_pass` plus the phase cost cap (#29), with
lost attempts booked as overhead when cost is known. A human is involved only if
capture keeps failing across bounded re-runs (#19) — never on a single transient
miss.

Async workers are allowed to finish after the pipeline state changes, so ledger
writes must be conditional. The finalize hook records every terminal
`run_id`/`attempt_no`, but only the **current active attempt** for a non-cancelled,
non-abandoned pass may update the pass aggregate, satisfy the done-bar, or advance
the state machine. A late terminal result from a cancelled, abandoned, failed-back,
or superseded attempt remains audit history and cannot resurrect stale state
(#30).

This makes the human gate a durable checkpoint: `status` shows where it is and
the evidence; `gate pass|fail --reason "…"` records the verdict and resumes the
loop. The driving agent reconstructs its position from the ledger on any new
session and validates branch/PR drift before continuing.

**Parked gates release re-creatable resources (#47).** On entering a `*_pending`
gate the pipeline keeps only the durable ledger + whole-origin ref and **releases
what it can rebuild**: the review-scoped db2 index is dropped (and re-ingested from
the retained origin, #42, when the loop resumes), and the worktree may be released.
A pipeline parked for days therefore does not pin a growing review index or a
worktree per open pipeline. An optional `roundtable_pipeline_gate_ttl_h` (§6) moves
a gate left unanswered past the TTL to `abandoned` with full child-run-stop +
cleanup (#31) — never an auto-pass.

If v1 keeps the "one active pipeline" limit, this parked state must also say
whether the gate still occupies that slot or has released it; releasing the slot
requires branch/PR-level ownership checks so a second active pipeline cannot
mutate the same recorded branch or PR (#48).

## §5 The two human gates

At `gate1_pending` / `gate2_pending` the agent **pauses** and surfaces a compact
digest, then waits for the verdict:

- the PR link;
- the converged-review **digest**: blocking/suggestion/nit counts, the
  highest-corroboration items (`item.count`), answered questions, and any
  `coverage_gaps`;
- pipeline economics: total outer passes, cumulative `cost_usd`, rounds, and the
  pinned cost scope/source/version behind that number (#52).

**pass** advances (merge → next phase). **fail** captures the human's reason; the
reason becomes a brief `focus`/`questions` entry for the next loop, and the state
returns to the prior review phase. The fail reason is durable in the ledger so the
re-review is genuinely directed by it.

**Resolving a gate requires authority the driving agent does not have (#53).** The
agent may *surface* a gate — move the pipeline to `*_pending`, build the digest —
but `gate pass|fail` is the one action it must not be able to call on itself,
because passing triggers the merge. Since the driving agent holds `CAP_DELEGATE`
(to run the roundtable) and every existing pipeline/delegate method is
`CAP_DELEGATE`-gated, the gate resolution must require a **separate** authority not
present in a delegate-driving session: a distinct capability (e.g.
`CAP_PIPELINE_GATE`), an operator/human principal, or an out-of-band confirmation.
Without that separation the two human gates are decorative — the agent could pass
its own.

**Gate resolution is exactly-once (#55).** `gate pass|fail` both records a verdict
and (on pass) merges + advances, so it must be a guarded, idempotent transition,
not a fire-and-forget command. It acts only when the pipeline is in the matching
`*_pending` state and atomically leaves that state by writing a durable
`gate*_merge_pending` intent (#56), so a repeat or concurrent call sees a
non-pending state and returns "already resolving/resolved" rather than merging
again; the merge itself is keyed by the recorded PR + expected head SHA (the drift
check above), so a retried merge of an already-merged PR is a no-op. A flaky client
must not be able to merge twice or skip a phase.

Because the merge is an external side effect, `gate*_merge_pending` is a recovery
state, not just an implementation detail (#56). On restart/resume, the pipeline
reconciles the recorded PR + expected head: already merged at that head records the
merge SHA and advances; still unmerged retries or parks with the latest merge
blocker; merged at a different head marks the gate evidence stale and stops for
operator review.

**The unanswered-gate TTL does not apply here (#57).** `roundtable_pipeline_gate_ttl_h`
abandons an *awaiting-human* `*_pending` gate; `*_merge_pending` is post-pass —
the human already approved — so it is **never** auto-abandoned for slowness. A
blocked merge retries with backoff and surfaces the blocker, leaving the state only
by a successful merge, an explicit operator `cancel`/`abandon`, or a separate
merge-block escalation that pings a human rather than discarding the verdict.

If the implementation chooses the distinct-capability path, it must first make the
capability model able to represent that authority (#54). The current mask has no
spare low bit (`CAPS_ALL` is `0xFFFFu`), so gate resolution is not just another
method-registry row. A v1 implementation may instead make `gate pass|fail`
operator-principal or local-UDS-only/out-of-band while keeping the driving agent's
surface limited to gate creation, status, digest refresh, cancel, and resume.

The command/API surface must be explicit in implementation. The proposal may add
`aimee pipeline status|gate` as a new first-class CLI/MCP surface, or extend the
existing `autopilot` pipeline handler, but it must not leave two unrelated
"pipeline" namespaces with different IDs. If an HTTP API is added, it must avoid
the existing KB/corpus route `GET /v1/pipeline/status`; use a namespaced path such
as `/v1/roundtable/pipelines/...` or `/v1/authoring/pipelines/...` (#45). The gate
action is net-new; today's autopilot actions cover `start`, `advance`, `status`,
`list`, `cancel`, `resume`, `link-plan`, and `link-job`, not pass/fail gates.

Pipeline `cancel`/`abandon` cannot be ledger-only. If a child roundtable op-run is
active, the action must request cancellation through the child `run_id`
(`/v1/runs/{id}/stop` / `openai_runs_store_request_cancel` semantics), mark the
active attempt cancelled or abandoned, and leave the finalize hook free to record
the eventual terminal state without advancing stale state (#30/#31).

Before a **pass** can merge or advance, the resumed agent revalidates the stored
worktree/PR state through Aimee's workspace-aware git surface (`git_pr` /
`mcp_git_run`, not an unqualified local shell): the PR still exists, its head SHA
matches the ledger or the digest is marked stale, the base branch is still the
intended target (`testing`), the dedicated worktree is clean enough for the
operation, `gh`/GitHub authority is available through local auth or the
per-workspace forge-token broker, and mergeability has not changed underneath the
gate. A failed validation returns to the relevant review phase or asks the human
for a fresh verdict with the stale evidence called out.

**Merge is an explicit, policy-aware step, not an assumed `gh pr merge` (#11).**
This repo bases PRs on `testing`, enforces branch policy, and has run with CI
billing suspended (so required checks can hang or fail fast). The pass→merge edge
therefore: (a) runs only after the human pass; (b) surfaces CI/mergeability state
in the gate digest rather than assuming green; (c) may require `gh pr merge
--admin` and states so; and (d) records the merge SHA in the ledger. If the merge
cannot complete under policy (auth, protected branch, hung checks), the pipeline
parks in the merge-pending recovery state with the reason surfaced — it never
reports a phase advanced when the merge did not land.

Because `git_pr` does not currently merge (#50), implementation must either add a
workspace-aware `git_pr action=merge` or explicitly route the merge through
`mcp_git_run`/`gh pr merge` with the workspace/forge-token authority. The ledger
records the executor, command/options, expected head SHA, output, exit code, and
merge SHA.

## §6 Config

New keys (full plumbing: `config.h` field, `config_fields.c`,
`config_sections.c`, `config_save.c`, `aimee config get/set/list`, generated docs,
`test_config`/`test_config_surface`/`test_cmd_config`). Decide nesting
(`roundtable.pipeline.*` section vs top-level) at implementation. If nested under
`roundtable`, the parser/saver must explicitly add nested-object support; today's
roundtable config surface is scalar keys like `roundtable.max_rounds`,
`roundtable.converge_threshold`, `roundtable.deadline_ms`, and `roundtable.turns`.

- `roundtable_pipeline_done_bar` — enum `zero_blocking` (default) |
  `zero_blocking_suggestions` | `zero_blocking_questions_answered`.
- `roundtable_pipeline_max_passes` — int, **default 0 (unbounded)**; >0 = outer
  pass ceiling that escalates (never auto-passes) on hit.
- `roundtable_pipeline_max_attempts_per_pass` — int, default 2; bounds
  infrastructure retries (`lost_result`, capture failure, queue/worker failure,
  cancellation retry) under the same stable pass id without consuming outer
  correctness passes.
- `roundtable_pipeline_max_cost_usd` — double, cumulative per-phase spend cap;
  0 = unbounded; tripping escalates. **Accumulates across fail-return re-entries of
  the phase; a fail-return never resets it** (#46).
- `roundtable_pipeline_max_total_cost_usd` — double, cumulative pipeline spend
  cap; 0 = unbounded; tripping escalates (#46). Implementation-phase spend is read
  from the **cost-accounting proposal's `usage_ledger` / `/v1/usage/*`** (#51), not
  a second accounting path; where that ledger has no row yet, implementation cost is
  marked **incomplete evidence** and the cap cannot be claimed satisfied. If that
  ledger has not landed, the cap is scoped to roundtable child-run cost only
  (a `…_max_roundtable_cost_usd` spelling) and says so (#49). The selected
  accounting scope/source/version is pinned in the pipeline ledger; changing it
  requires a fresh reconciliation and stale gate digest rather than silently
  combining DB1 token-audit totals, child-run estimates, and db2 usage rows (#52).
- `roundtable_pipeline_gate_ttl_h` — int hours, **default 0 (no expiry)**; >0 moves
  an **awaiting-human** `*_pending` gate left unanswered past the TTL to `abandoned`
  with full child-run-stop + cleanup (#31), never an auto-pass (#47). It does **not**
  apply to `*_merge_pending` — a post-pass merge is never auto-abandoned for
  slowness (#57).
- `roundtable_pipeline_parked_releases_slot` — bool, default chosen by
  implementation policy; if true, a parked human gate releases the single active
  pipeline admission slot while retaining branch/PR ownership guards (#48).
- `roundtable_pipeline_unknown_context_tokens` — int, conservative fallback chunk
  input budget used only when a panel participant's context window cannot be
  resolved; alternatively the implementation may make unknown context a hard
  validation error instead of accepting this key (#36).
- *(optional, deferred)* per-phase overrides (`…_proposal` / `…_pr` suffixes) if
  the proposal and PR phases want different bars.

Participants, aggregator, per-call cost, and inner-round bounds are **inherited**
from `ensemble_*` / `roundtable_*`; no duplication.

## §7 Making "converged" mean "correct"

Convergence is only as meaningful as the panel. To keep the quality gate honest
(not "agreeable panelists agreed"):

- **Participant diversity** — the panel reuses `ensemble_*` participants. The
  configured `ensemble.reference_models` values are agent/model names, so the
  validator must resolve each participant through the agent config and compare
  provider/model identity, not just string uniqueness. A single-provider panel can
  converge on its own blind spots. Validation (§10) asserts ≥2 distinct providers
  for a pipeline run or warns.
- **Context-budget resolution** — the same participant-resolution pass must also
  resolve a positive context budget for every panelist, using
  `agent.middleware.context_window`, model defaults, or a configured conservative
  fallback. Unknown budgets are surfaced before chunking (#36), not discovered by
  truncating a reviewer mid-pass.
- **Open mandate** (§2) so direction never suppresses out-of-scope findings.
- **Corroboration surfacing** — the gate digest shows `item.count` so the human
  sees whether a finding was one panelist or all of them.
- **Adversarial framing** inherited from review mode's `review` charter role.

These make a clean done-bar correspond to *correctness*, which is the real target
— the pipeline never trades correctness for fewer passes.

## §8 Dependencies

- **Mostly satisfied (review mode):** `ensemble_review` is already on `testing`
  with the brief (string or `{focus,fixes,invariants,questions}`), the structured
  `items`/`items_round`/`artifact_round` result, and the run-id/poll lifecycle
  (`mcp_tools.c:610`, `handle_mcp_ensemble_review`). The remaining dependency on
  `agent-directed-pr-review` is **narrow**: confirm the `/v1/runs` result shape is
  stable and complete enough to evaluate the done-bar (the items array with
  severities and `count`), and treat any tightening of that contract as a shared
  interface, not a blocker. The pipeline is *not* gated on that proposal merging
  wholesale.
- **Hard for proposal drafting (open gap):** a callable `ROUNDTABLE_DRAFT` path.
  `ensemble_review` forces review mode, so DRAFT must go through
  `/v1/delegate/roundtable` / `aimee delegate roundtable --mode draft`, or a narrow
  draft MCP sibling — never by overloading the review-only tool. And per #9 the
  DRAFT artifact is ~8 KB-capped, so this path yields a skeleton, not a full
  proposal.
- **Hard:** durable DB1 checkpointing for the roundtable pipeline. `/v1/runs`
  remains a child-run polling surface only because its store is bounded and
  non-durable.
- **Hard:** a pipeline-owned async submission contract. The existing
  `ensemble_review` MCP surface does not forward arbitrary metadata, so the
  pipeline needs an explicit `pipeline_pass_id`/attempt submission path that
  validates state and injects private `__pipeline_pass_id` before
  `op_run_worker_run` finalizes the child run.
- **Hard for chunked review:** a pipeline-owned review-artifact indexing contract.
  Existing KB build/update is project/path oriented and `kb.docs.push` stores whole
  staged docs, but neither is automatically a review-scoped, cleanup-safe chunk
  index. The implementation must choose synthetic review projects, per-file/scope
  cleanup, or a direct artifact-index API (#35).
- **Hard:** workspace-aware git/forge authority for PR create/merge and diff
  capture. Pipeline git/gh operations must route through Aimee's git tools or
  workspace provider so shared, detached, and mirror workspaces behave consistently.
- **Hard for merge:** a pass→merge executor. `git_pr` can create/view/edit/check
  PRs and report merge status, but it does not merge today; add `git_pr merge` or
  route `gh pr merge` through `mcp_git_run` with captured evidence (#50).
- **Hard if exposing HTTP:** route namespace selection. `GET /v1/pipeline/status`
  already belongs to the KB/corpus ingest pipeline, so roundtable-authoring routes
  must use a distinct namespace or remain CLI/MCP-only in v1 (#45).
- **Hard:** a gate-resolution authority distinct from `CAP_DELEGATE` (#53). Every
  pipeline/delegate method today maps to `CAP_DELEGATE`, which the driving agent
  holds; `gate pass|fail` must require a capability/principal that a delegate-driving
  session does not have, or the agent can self-approve its own merge.
- **Hard if using a new capability bit:** capability-mask migration (#54). The
  current 16-bit `CAPS_ALL` / low-bit capability set is full; adding
  `CAP_PIPELINE_GATE` requires updating the cap constants/composites, TCP bearer
  scope derivation, route authorization, capability/OpenAPI docs, and tests. If
  that migration is deferred, gate resolution must use an operator-principal or
  local/out-of-band authority instead of a new cap bit.
- **Soft:** the cost-accounting proposal's `usage_ledger` / `/v1/usage/*` for
  whole-pipeline cost that includes implementation-phase spend (#51). Absent it,
  the total cap is roundtable-child-run-cost only and says so (#49). Any
  implementation that bridges from current DB1 `token_audit` / `cost_fold_log` to
  future db2 `usage_ledger` must version and pin the accounting source per
  pipeline, not mix both in one cap decision (#52).
- **Soft:** the `git diff` range helper (agent-directed-pr-review P2) for the PR
  phase input; otherwise the pipeline captures the diff through the
  workspace-aware git surface.
- **Repo flow:** PRs base `testing`; main promotion is separate and out of scope.

## §9 Phasing

- **P0 — Ledger + states + namespace decision.** Choose the DB shape
  (`roundtable_pipeline_runs` tables vs explicit migration of DB1 `pipelines`),
  add the state enum/transitions, artifact origin refs/hashes, chunk-index refs,
  pass/attempt rows, child run references, and the CLI/MCP/HTTP namespace decision
  (`pipeline` vs `autopilot` extension). Choose the review-artifact indexing and
  cleanup contract (#35) before depending on chunked done-bars. Wire the
  server-worker capture seam: validate
  `pipeline_pass_id`, inject private `__pipeline_pass_id`, preserve it through
  async enqueue, and extend `op_run_worker_run`'s finalize path or hook to persist
  every terminal attempt to the ledger (#18/#20-#25). The finalize hook must be
  current-attempt guarded so late workers cannot advance stale state (#30), and
  pipeline cancel/abandon must request child-run stop when one is active (#31).
  Choose the gate-resolution authority shape in this phase too: either migrate the
  capability mask for a real gate cap, or make gate resolution
  operator-principal/local-out-of-band and prove delegate-driving sessions cannot
  call it (#53/#54). No loop yet; states/gates settable manually. Mergeable alone,
  with restart tests.
- **P1 — Outer review loop + done-bar.** The REVIEW⇄revise loop over
  `ensemble_review`, the configurable done-bar evaluator, the pass ceiling +
  escalation, the attempt retry ceiling, the echo guard. Drives the PR phase first
  (single mode, simplest).
- **P2 — Proposal phase (DRAFT + REVIEW).** Add the `drafting` DRAFT step and the
  proposal-review loop; wire proposal PR create/merge through the workspace-aware
  git/PR surface.
- **P3 — Human gates + fail-return edges** end to end, with the durable digest,
  PR/worktree drift validation through the workspace-aware git surface,
  mergeability/auth checks, an explicit merge executor (#50), parked-gate admission
  policy (#48), durable merge-intent / merge-pending recovery (#56), and
  reason-to-brief feedback. This closes the full loop.
- **P4 — Config surface** for all keys + generated docs + tests (lands with the
  phase that first reads each key, not deferred).

P0/P1 are useful standalone (a directed, looped PR reviewer with a durable
ledger) before the full idea→merge pipeline of P2/P3.

## §10 Validation

- **Loop correctness** — a fixture proposal/diff with N seeded blocking issues:
  the loop reaches the done-bar only after all N are resolved; the digest counts
  match the engine items; the ledger records each pass's `run_id`/cost.
- **Per-pass version freshness (#42)** — revise an artifact so a span that existed
  in pass *k* is deleted/renamed in pass *k+1*; the pass *k+1* aggregate retrieval
  returns only the new version's spans (pinned to its content hash), never the
  superseded chunk, and superseded review-scoped chunks are cleaned up.
- **Fail-return updates the same PR (#43)** — fail a gate, re-revise, and assert
  the revision is pushed to the **recorded** branch/PR (head SHA advances, PR
  number unchanged); no second PR is opened, and an orphaned/duplicate PR is a hard
  error.
- **DRAFT/REVIEW callable split** — the proposal phase invokes a draft-capable
  roundtable path that returns an `artifact`; the review phases invoke the
  review-capable path that returns structured items for the done-bar.
- **Mode-appropriate capture (#27)** — a DRAFT pass persists an artifact ref +
  hash + `best_round`/flags (skeleton text in the working file, not the ledger);
  a REVIEW pass persists items/severities/`count`; neither is mis-shaped into the
  other, and the finalize hook no-ops for a non-pipeline `ensemble_review` call.
- **Agent reads terminal from the ledger (#26)** — with `/v1/runs` forced to evict
  the run and the agent asleep through terminal, the agent still detects completion
  by polling the durable ledger pass row; it never depends on `/v1/runs` being
  present to learn a pass finished.
- **Ledger compatibility** — if reusing DB1 `pipelines`, old autopilot
  `start/status/list/cancel/resume/link-*` behavior remains intact; if using a new
  table, pipeline IDs and command names are unambiguous.
- **Run-store separation** — kill/restart after child `/v1/runs` records are gone;
  the pipeline still resumes from DB1 and marks missing child run details as
  historical evidence, not lost state.
- **Server-worker result capture (#10/#12/#18)** — drive ≥256 sibling runs (or
  force eviction) between a child run completing and any agent poll; the pass
  result is already in DB1 because `op_run_worker_run`'s finalize path wrote it
  (keyed by `__pipeline_pass_id`), **with the driving agent killed** for the
  window. The done-bar/digest are unaffected by the evicted `/v1/runs` record.
- **Pass-id forwarding (#20/#21)** — submit a pipeline-owned `ensemble_review` /
  draft roundtable pass and assert the private `__pipeline_pass_id` reaches
  `op_run_worker_run`; an arbitrary user-supplied double-underscore id is rejected
  or ignored, and a stale/non-owned pass id cannot mutate the ledger.
- **Async-only capture (#22)** — prove pipeline-owned DRAFT and REVIEW calls go
  through async op-run capture; any direct synchronous roundtable dispatch is
  either forbidden for pipeline passes or has an equivalent capture hook.
- **Lost-result re-run (#19)** — simulate a genuine capture fault: the pass is
  marked `lost_result` and auto re-runs under the **same `pipeline_pass_id`** (no
  double-counted pass; lost attempt booked as overhead), escalating to the human
  only after `roundtable_pipeline_max_attempts_per_pass` is exhausted or the phase
  cost cap trips, never on the first miss.
- **Attempt accounting (#23/#25)** — successful, failed, cancelled, malformed,
  capture-failed, and lost attempts all get attempt rows and preserve run IDs,
  terminal status, flags, result hashes, and known cost without overwriting prior
  attempts.
- **Replay hash pinning (#24)** — mutate the proposal/diff between a lost attempt
  and retry; the pipeline refuses to reuse the old pass id and starts a new pass
  because the artifact hash changed.
- **Chunk aggregate done-bar (#28)** — split a diff/proposal into multiple chunks
  with a cross-chunk invariant violation; individual chunks can complete, but the
  phase cannot pass until the aggregate whole-artifact synthesis/check records the
  violation resolved. Missing or stale chunk hashes keep the aggregate blocked.
- **Origin retention + retrieval-backed aggregate (#32/#34, origin invariant)** —
  the artifact has a durable whole-origin ref/hash separate from chunk rows; chunk
  rows can be walked by `prev/next` and `chunk_index`, but the full text is
  recovered from the origin ref (`docs.normalized_text`, working blob/path, or
  artifact payload), not assumed to live in `kb_documents`.
- **Review-scoped index cleanup (#35)** — index a review artifact without touching
  the real project KB; cleanup deletes only that pipeline's review chunks/docs.
  A test must prove `/v1/maintenance/clear` or any chosen cleanup path cannot wipe
  unrelated project data.
- **Orchestrator-assembled inline review (#37)** — a participant configured with
  **no file or KB-search tools** still produces a correct cross-chunk finding,
  because the orchestrator pre-retrieved the needed spans and passed a
  self-contained inline unit; the panel's tool access is never load-bearing for
  review correctness.
- **Assembled input budget/provenance (#39)** — build an aggregate review unit
  where retrieved candidate spans exceed the smallest participant budget; the
  assembly manifest records selected/omitted spans and hashes, omitted required
  spans block the aggregate with a coverage gap, and no over-budget inline unit is
  submitted as valid evidence.
- **Item source provenance sidecar (#40)** — generate a finding whose evidence
  spans many chunks/participants so `item.sources` would be clipped; the gate
  digest still links the item identity/result hash to the durable sidecar spans
  and source refs, not just the display string.
- **Chunk threshold from min panel context (#33)** — a heterogeneous panel sizes
  chunks to the smallest participant context budget (resolved per run, reserving
  brief/role/peer-note headroom); a chunk that would overflow the smallest model is
  never submitted to it.
- **Unknown context fallback (#36)** — configure one participant with no
  `context_window` and no model default; the pipeline either rejects the run with a
  clear validation error or uses the configured conservative fallback and records
  that fallback in the chunk manifest.
- **Late finalization guard (#30)** — start an attempt, supersede/cancel/abandon
  it, then let the old worker finish; the terminal attempt row is recorded, but it
  does not update the current aggregate, pass the done-bar, open a gate, or resume
  the pipeline.
- **Pipeline child cancellation (#31)** — cancelling/abandoning a pipeline with an
  active roundtable run requests child op-run cancellation, persists the cancelled
  attempt, and tolerates the worker's later terminal write.
- **DRAFT skeleton + expansion (#9)** — a DRAFT call returns a skeleton within the
  8 KB cap without truncating mid-structure; the author-revise loop grows it to a
  full proposal in the working file, and REVIEW reviews the file/chunks, not the
  capped artifact.
- **Done-bar config** — each of the three bars stops the loop at the right point
  (suggestions block under bar 2; questions must be answered under bar 3).
- **Done-bar validity flags (#38)** — `truncated`, `items_truncated`, `degraded`,
  `cost_capped`, `deadline_hit`, `cancelled`, lost result, or ambiguous
  `items_round` / `artifact_round` provenance prevents a done-bar pass and
  escalates.
- **Single validity predicate (#41)** — a table-driven test trips each invalidity
  flag in turn and asserts that **every** eligibility consumer (done-bar,
  chunk-aggregate, gate digest) rejects it via
  `roundtable_terminal_envelope_valid`; no consumer re-implements the check, so a
  newly added flag cannot be honored by some sites and ignored by others.
- **Capture-before-validity (#44)** — feed successful, failed, cancelled,
  malformed, oversized, and `items_truncated` terminal snapshots through the worker
  hook; every attempt gets a durable envelope row before validity is evaluated, and
  invalid envelopes are retained for retry/audit while blocked from satisfying the
  done-bar.
- **Question cap** — a brief with >16 questions is rejected or split before using
  `zero_blocking_questions_answered`; overflow cannot be silently treated as
  answered.
- **Pass-ceiling escalation** — with a low cap and an unresolvable seeded issue,
  the loop escalates to the human at the cap and never auto-passes.
- **Resumability** — kill and restart between a review pass and a gate; the ledger
  restores state and the gate is answerable post-restart.
- **Git/PR drift** — mutate the PR head SHA, target branch, or mergeability while
  paused at a gate; the pass action refuses stale evidence and requires a fresh
  review or human confirmation.
- **Workspace authority** — run proposal/implementation PR operations in shared
  and detached workspaces; git/gh calls route through `mcp_git_run`/`git_pr` and
  forge-token state is surfaced when required.
- **HTTP namespace collision (#45)** — assert that any authoring-pipeline HTTP
  route does not occupy `GET /v1/pipeline/status`, and that the existing KB/corpus
  pipeline status endpoint and generated docs/tests continue to pass unchanged.
- **Branch/worktree isolation** — dirty user checkout blocks the pipeline unless
  the run has a dedicated branch/worktree or an explicit operator override.
- **Large payload handling** — a diff/proposal larger than a single MCP/op-run
  payload is reviewed through artifact refs or chunks, and the ledger stores
  hashes so stale chunks are detected; chunked results are aggregated before the
  done-bar is evaluated.
- **Fail-return** — a `fail --reason` re-enters the review loop with the reason
  present in the next brief (asserted in the reviewer prompt).
- **Panel diversity** — resolve `ensemble.reference_models` through agent config;
  a single resolved provider warns; a ≥2-provider panel does not.
- **Cost accounting** — cumulative per-phase cost matches the sum of per-pass
  `cost_usd`; the per-phase cap trips correctly.
- **Cost across fail-return + whole-pipeline cap (#46)** — fail a gate and re-enter
  the phase; the per-phase cap continues from its prior total (does not reset), and
  a whole-pipeline cap trips across proposal + implementation + PR phases, each
  escalating rather than auto-passing.
- **Total cost accounting scope (#49)** — run a pipeline with implementation-agent
  work between review phases; assert the total cap either explicitly excludes that
  spend (roundtable-only naming/contract) or includes it from token/cost accounting,
  and unknown in-scope implementation cost prevents claiming the cap is satisfied.
- **Cost-source pinning + usage-ledger fallback (#51/#52)** — start a pipeline while
  only DB1 `token_audit` / `cost_fold_log` and roundtable `cost_usd` are available,
  then resume after a db2 `usage_ledger` implementation is present; assert the
  ledger keeps the original source pinned or explicitly reconciles and marks the
  gate digest stale. Missing in-scope usage rows block "cap satisfied"; a
  roundtable-only configuration uses the explicit roundtable-only key/contract.
- **Gate authority separation (#53)** — a session holding only the driving agent's
  caps (`CAP_DELEGATE`) can surface a gate but is **denied** `gate pass|fail`; only
  the separate gate authority (capability/operator principal/out-of-band) can
  resolve it. The negative test proves an agent cannot self-approve and merge.
- **Exactly-once gate resolution (#55)** — call `gate pass` twice (and concurrently)
  on a `*_pending` pipeline; exactly one merge + advance occurs, the second call
  returns "already resolved," and a retried merge of an already-merged PR is a
  no-op — never a double-merge or a skipped phase.
- **Merge-pending crash recovery (#56)** — crash after the authorized gate verdict
  is persisted but before merge, and again after the remote merge succeeds but
  before `merge_sha`/advance is recorded. On restart, the pipeline resumes from
  `gate*_merge_pending`, reconciles PR number + expected head SHA, either retries
  or records the already-landed merge SHA, and advances exactly once. A PR merged
  at a different head stops as stale evidence.
- **Gate capability representation (#54)** — if a distinct gate capability is
  added, assert `CAPS_ALL`, `CAPS_AUTHENTICATED`, scoped/unscoped TCP bearer
  behavior, `server_http_route_caps`, capability advertising/OpenAPI docs, and
  route-allowed tests all agree on whether the gate route is reachable. If no new
  bit is added, assert the operator/local confirmation path denies delegate-only
  sessions and still lets an authorized human resolve the gate.
- **Parked-gate resource release + TTL (#47)** — at a gate, the review-scoped index
  is dropped and correctly re-ingested from the origin on resume (same content
  hash, retrieval still works); with a TTL set, an unanswered gate moves to
  `abandoned` with child-run-stop + cleanup, never an auto-pass.
- **Merge-pending survives the TTL (#57)** — with a gate TTL set, a pipeline in
  `*_merge_pending` whose merge is blocked past the TTL is **not** abandoned: the
  verdict is preserved, the merge retries/surfaces the blocker, and only a
  successful merge or explicit operator abandon leaves the state.
- **Parked-gate admission (#48)** — with one pipeline parked at a gate, start a
  second pipeline only if the selected policy releases the active slot; either way,
  branch/PR ownership prevents two active/restarted runs from mutating the same
  recorded branch or PR.
- **Merge executor contract (#50)** — pass a gate with a mergeable PR and assert
  the pipeline uses either `git_pr action=merge` or an explicitly authorized
  `mcp_git_run`/`gh pr merge`, records expected head SHA/output/exit code/merge
  SHA, and does not advance when the command fails or merges a different head.

## §11 Non-goals (v1)

- New server-side GitHub review/comment APIs (no PR fetch, no inline-comment
  posting). The pipeline uses Aimee's existing workspace-aware git/PR tool surface,
  which may call `gh`/`git` underneath with the right workspace/forge authority.
- Storing whole proposals or large diffs as unbounded DB blobs. DB1 stores refs,
  manifests, hashes, and compact digests; working files/blobs, db2 `docs`, or an
  explicit artifact payload carry the large origin content. `kb_documents` remains
  a chunk index, not the canonical whole-origin store.
- Expecting DRAFT mode to emit a complete proposal (#9). The 8 KB artifact cap
  means DRAFT produces a skeleton; the author-revise loop expands it in the working
  file. Generating long-form text inside the roundtable artifact is out of scope.
- A fully autonomous, gate-less pipeline — the two human gates are mandatory by
  design; "configurable max passes" bounds cost, it does not remove the human. This
  is **enforced by authority separation (#53)**, not convention: the driving agent
  cannot resolve a gate, so the pipeline cannot self-approve its way to a merge.
- Engine changes to the roundtable. The pipeline is strictly on top.
- Multi-proposal/parallel-pipeline scheduling beyond the explicit parked-gate
  admission rule (#48). v1 may allow many parked/waiting-human rows only if they
  do not count as active and branch/PR ownership prevents mutation overlap; it
  still permits at most one active drafting/review/implementing runner unless a
  later proposal adds scheduling.

## §12 Open questions

- **Ledger home:** a namespaced DB1 `roundtable_pipeline_runs` schema vs an
  explicit backwards-compatible extension of the existing DB1 `pipelines` table.
  `/v1/runs` / `openai_runs_store` is ruled out for checkpoints because it is a
  bounded live store and is not durable across restarts.
- **CLI/API namespace:** add `aimee pipeline ...` as a new surface, or extend the
  existing `autopilot` pipeline actions with gate/status operations? The answer
  must keep IDs and help text unambiguous. If HTTP is exposed, it must not collide
  with the existing KB/corpus `GET /v1/pipeline/status` route; choose a distinct
  roundtable/authoring namespace or defer HTTP.
- **Who is the driving agent at the gate?** When paused at a human gate across
  sessions, does a fresh agent resume from the ledger automatically, or does the
  human re-invoke `aimee pipeline resume <id>`?
- **Gate authority shape:** should gate resolution widen/rework the capability
  mask to add a real gate capability, or should it be operator-principal /
  local-out-of-band only for v1? The current `CAPS_ALL == 0xFFFFu` mask leaves no
  spare low bit for a simple `CAP_PIPELINE_GATE` constant.
- **Parked-gate admission:** does a human gate release the single active pipeline
  slot (`roundtable_pipeline_parked_releases_slot=true`) or block new authoring
  runs until answered/abandoned?
- **Cost scope:** is `roundtable_pipeline_max_total_cost_usd` roundtable-only, or
  does it include normal implementation-agent spend via token/cost accounting?
- **Per-phase config:** do the proposal and PR phases want independent done-bars /
  ceilings by default, or one shared set with optional overrides?
- **DRAFT seeding:** does the `drafting` step take only the idea, or also a
  pointer to sibling/related proposals so the first draft starts grounded?

---

## Appendix A — Decision log (PR #183 review, 57 findings)

This proposal was hardened over 23 review passes against the live tree. Each
decision below is **already implemented in the design sections above**; this log
is the compact traceability index for the `(#NN)` references and records the
key in-tree grounding. Grouped by review round to show how the contract evolved.

**Round 1 — ledger, surfaces, and callable shape (§1, §2, §4, §5, §7–§10).**
1. No blank-slate `pipeline_run` name — a DB1 `pipelines`/`autopilot` table already exists; namespace or migrate deliberately (§4, §12).
2. `/v1/runs` (`openai_runs_store`, 256-bounded, non-durable) is not a checkpoint store; the ledger is DB1 (§4).
3. CLI/API surface explicit — new `pipeline` namespace or `autopilot` extension, no collision (§5, §9).
4. GitHub/worktree state must be resumable (branch/PR/SHA/mergeability/auth in the ledger) (§4, §5).
5. No unbounded inline artifacts — store refs + hashes (§2, §4, §11).
6. Panel-diversity check resolves agent→provider identity, not string uniqueness (§7).
7. DRAFT and REVIEW use different callable paths (review-only `ensemble_review`) (§1, §2, §8).

**Round 2 — engine feasibility (§1–§4, §8, §11).**
8. The `ensemble_review` review surface is already on `testing`; dependency is narrow (§8).
9. DRAFT can't emit a full proposal — artifact is `char[8192]`-capped; it yields a skeleton (§1, §2, §11).
10–12. Result capture must be durable and **server-worker-owned**, not an agent poll, because `/v1/runs` evicts (§4).
13. Done-bar rejects `degraded`/`truncated`/`cost_capped`/`deadline_hit`/`cancelled` (§3).
14. Questions-done bar caps at `ROUNDTABLE_MAX_QUESTIONS` (16) (§3).
15. Git ops route through Aimee's workspace authority, not raw shell (§4, §5).
16. Dedicated proposal/impl branches + clean-worktree preflight (§1, §4, §5).
17. Dependency wording de-duplicated (§8).

**Round 3–5 — capture seam, pass identity, retrieval owner (§2–§4, §8).**
18. Capture rides the existing `op_run_worker_run` finalize hook (the Hybrid seam) (§4).
19. `lost_result` auto re-runs the pass (stable `pipeline_pass_id`), not human-escalate (§4).
20–25. Pipeline-owned async submission: validated `pipeline_pass_id` → private `__pipeline_pass_id`; async-only path; per-attempt rows; hash-pinned replay; persist failed/cancelled terminals (§2, §4, §8).
26. Agent reads terminal from the **durable ledger row**, not `/v1/runs` (§4).
27. Capture is mode-appropriate — DRAFT persists artifact ref+hash, REVIEW persists items (§2, §4).

**Round 6–9 — chunked review and origin (§2, §3, §6, §7).**
28. Chunked review needs an artifact-level aggregate, not independent green slices (§2, §3).
29. Infra retries get their own ceiling (`…_max_attempts_per_pass`) (§3, §6).
30. Late finalization can't advance superseded state (current-attempt guard) (§4).
31. Pipeline cancel/abandon bridges to child `/v1/runs/{id}/stop` (§4, §5).
32. Store the **whole origin** + db2 chunks; the aggregate **retrieves** (no re-ingest) (§2, §3).
33. Chunk threshold = the panel's **minimum** participant context budget (§2).
34. `kb_documents` is a chunk index, not the whole-origin store — keep a separate origin ref (`docs.normalized_text`/blob) (§2, §4).
35. Review-scoped indexing needs a real cleanup-safe primitive (synthetic project / scoped cleanup) (§2, §8).
36. Unknown panel context → conservative fallback or hard validation error (§6, §7).

**Round 10–12 — who retrieves, serialization, validity (§1–§4, §6).**
37. The **orchestrator** retrieves and hands each panelist a self-contained inline unit; panelists aren't assumed to retrieve/read files (§2, §3).
38. `items_truncated` (HTTP-serializer flag, not a struct field) is terminal-invalid evidence (§3, §4).
39. Inline review units carry a budgeted assembly manifest (spans, budget, omissions) (§2, §3, §4).
40. Display-sized item fields aren't durable provenance — keep an identity→spans sidecar (§4).
41. One canonical `roundtable_result_valid_terminal()` predicate; no consumer checks a subset (§3, §4).
42. Each revise pass re-ingests and **supersedes** the prior version's chunks (no stale spans) (§2, §3).
43. Fail-return updates the **same** recorded PR, never a duplicate (§1, §5).

**Round 13–15 — cost scope and parked-gate lifecycle (§3–§6).**
44. The validity predicate runs on a captured **envelope** (incl. serialization/ledger flags), and never gates capture (§3, §4).
45. `/v1/pipeline/status` is the KB/corpus route — authoring needs a distinct namespace (§5, §8, §12).
46. Per-phase cost cap **accumulates across fail-returns**; add a whole-pipeline `…_max_total_cost_usd` (§3, §6).
47. Parked gates release re-creatable resources (drop+re-ingest index from origin); optional `…_gate_ttl_h` → `abandoned`, never auto-pass (§4, §5, §6).
48. Admission: a parked gate either holds or releases the single v1 slot (`…_parked_releases_slot`), with branch/PR mutual exclusion (§1, §4, §5, §11, §12).
49. Whole-pipeline cap must define implementation-spend scope; unknown impl cost = incomplete evidence (§3, §4, §6).
50. `git_pr` has **no merge action** (only `merge_status`) — pass→merge needs a real executor (§5, §8, §9).

**Round 16–19 — cost provenance and gate authority (§3–§6, §8, §10, §11).**
51. Implementation spend reads the cost-accounting proposal's db2 `usage_ledger`/`/v1/usage/*`, not a parallel path (§3, §6, §8).
52. Cost evidence pins `cost_scope`/`cost_source`/version (DB1 `token_audit`/`cost_fold_log` today vs future db2 `usage_ledger`); never mixes sources in one cap decision (§4, §5, §6, §8).
53. The gate verdict needs authority the driving agent lacks — `CAP_DELEGATE` ≠ gate authority, or the agent self-approves its own merge (§5, §8, §11).
54. A new `CAP_PIPELINE_GATE` needs a cap-mask migration (`CAPS_ALL` is `0xFFFFu`, bits 0–15 full); else use an operator-principal/UDS-only path (§5, §8, §9, §12).

**Round 20–23 — exactly-once, crash recovery, and approval safety (§1, §4, §5, §6).**
55. Gate resolution is exactly-once — guarded `*_pending` transition + idempotent merge (PR+head SHA); a double `pass` can't double-merge (§5).
56. The external merge needs a durable `*_merge_pending` saga state: intent → merge → reconcile→`merge_sha` before advancing; recover on restart (§1, §4, §5).
57. The unanswered-gate TTL applies **only** to awaiting-human `*_pending`, never `*_merge_pending` — a slow merge can't auto-abandon a human's approval (§4, §5, §6).
