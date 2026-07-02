# Proposal: dedicated Proposals web page (author → autonomous implement → status/history)

- **State:** done
- **Completed:** 2026-07-02
- **Moved from:** `docs/proposals/pending/proposals-ui-page.md`
- **Feature doc:** [`docs/WORKFLOW_ACTIONS.md`](../../WORKFLOW_ACTIONS.md)
- **Renamed after ship (2026-07-02):** this design refers to the page as "Proposals"
  and the def editor as "Workflows"; the shipped UI names them **"Workflow Actions"**
  and **"Edit Workflows"** respectively (routes `/workflow-actions`, `/edit-workflows`).
  The design text below keeps the original names as a historical record; the feature
  doc above is authoritative for the current names.

## Goal

Give the aimee web UI a **Proposals** page, separate from Workflows, that carries a
change from an empty editor to a merged PR:

1. **Author** a proposal from scratch (markdown editor + scaffold; optional
   delegate-assisted drafting/refinement).
2. **Implement** it end-to-end by handing it to the existing autonomous-development
   driver (submit → the wfe scheduler runs the `build` workflow: roundtable gate →
   human approve → plan → impl → verify → PR → CI → merge), parking at human gates.
3. **Watch** it: a per-proposal **status + history** view the user can scroll back
   through to see everything that happened — every stage transition, gate decision,
   pause reason, cost, and the resulting PR.

The point is a single home for the whole proposal lifecycle. Workflows stays the
*graph editor / def authoring* surface; Proposals is the *do-the-work* surface.

## §0 What already exists (so we don't rebuild it)

The autonomous-implementation backend is **already built and merged** (see
`done/full-autonomous-development.md`, `done/autonomous-dev-execution-substrate.md`).
This proposal is mostly a **read surface + UI** on top of it.

- **Submit** — `POST /api/dev/submit {proposal_md, workflow?, repo?}` → Go
  `handleDevSubmit` (`webchat/dev.go:12`) → `POST /v1/dev/submit` (`rh_dev_submit`,
  `server_http_routes.inc:183`, `CAP_DELEGATE`). It writes the markdown to
  `$AIMEE_HOME/workflows/proposals/wi-*.md`, calls `wfe_work_item_create` (one
  `lifecycle_work_item` row + a `create` `lifecycle_event`), and
  `wfe_scheduler_notify()`. Returns `{work_item_id, workflow, state}`.
- **Data model** (`src/db1/schema.sql:119`, typed in `src/db1/wfe_store.h`):
  - `lifecycle_work_item`: `work_item_id, repo, proposal_path, workflow_name,
    workflow_version, current_stage, state, mode, pause_reason, pr_ref,
    cum_cost_usd, work_item_max_cost_usd, override_count, submitter, created_at,
    updated_at`. `state ∈ {active, accepted, rejected, abandoned}`;
    `mode ∈ {interactive, autonomous}`; `pause_reason ∈ {"", pending_human,
    panel_degraded, budget_exceeded, panel_unreachable, ci_pending, merge_pending,
    failed, max_attempts}`.
  - `lifecycle_event` (append-only): `work_item_id, stage, kind, actor, detail,
    content_hash, cost_usd, created_at`. `kind ∈ {create, advance, loop, pause,
    failed, terminal, resume, approve, reject, reject_retry, override, rejected}`.
    This **is** the scrollable history — it just isn't served anywhere yet.
- **The driver** — `wfe_scheduler` (`src/server/wfe_scheduler.c`, concurrency 1,
  wakes on notify + 30s backstop) runs `wfe_autonomy_run` on every
  `state=active && mode=autonomous` item; `wfe_engine_advance`
  (`src/workflow/wfe_engine.c:192`) executes one node per lifecycle txn and records a
  `lifecycle_event` per transition. Live forge stays **default-off** behind
  `wfe_live_forge_enabled` (unchanged here).
- **Gates** — `POST /v1/workflow/items/<id>/gate {decision, gate?}`
  (`rh_workflow_gate`, `CAP_WORKFLOW_ADMIN`): gate read from the row's
  `current_stage` (never trusted from the request), signed approval, TOCTOU-guarded
  apply, reject-retry capped at 3. The Workflows page already calls this
  (`/api/workflow/items/<id>/gate`).
- **Run snapshot** — `GET /v1/workflow/items[/<id>]` (`wf_api_items`/`wf_api_item`,
  `server_workflow_api.c`) returns the thin `RunItem`
  `{id, workflow, version, stage, state, mode, pause_reason, repo}`.

**We reuse all of the above.** We do **not** add a second work model, and we do
**not** touch `work_queue` (`/v1/work/*`, the older CLI kanban) or
`roundtable_pipeline_*` (CLI-only). A proposal remains *markdown + a work item*;
its lifecycle *is* the work-item state machine.

## §1 Exposed read surfaces (backend, additive, no new tables)

Three thin, read-only additions — the load-bearing gap is that the timeline and
rich fields exist in the DB but are never served.

**Access model for the per-item reads (R1 — closes the IDOR).** `/events`,
`/proposal`, and the enriched single-item GET are **ownership-scoped, not just
`CAP_DASHBOARD_READ`**: the server compares the row's `submitter` to the caller's
attested subject and serves only if they match, **or** the caller holds
`CAP_WORKFLOW_ADMIN`. This is enforced in the C handler (the authoritative seam),
not the UI. Fail-closed: a row with a NULL/empty `submitter` (CLI/legacy/system
items) is visible **only** to `CAP_WORKFLOW_ADMIN`. Same rule scopes the list (§2).

- **§1a — Timeline API.** New `GET /v1/workflow/items/<id>/events` →
  `db1_lifecycle_event_list(id, &out)` (already implemented, oldest-first), serialized
  as `{events:[{id, stage, kind, actor, detail, cost_usd, created_at}], next_after}`.
  **Paginated** via `?after=<event_id>&limit=<n≤200>` (default 200): the page fetches
  the tail once, then polls with `after=<last id>` to append only new events — so a
  long/looping run never refetches its whole history. Go proxy
  `GET /api/workflow/items/<id>/events`. Ownership-scoped as above.
- **§1b — Richer item serialization.** Extend `item_to_json`
  (`server_workflow_api.c:398`) to also emit **only cheap DB columns** —
  `proposal_name` (basename of `proposal_path`, never the server FS path), `pr_ref`,
  `cum_cost_usd`, `work_item_max_cost_usd`, `override_count`, `submitter`,
  `created_at`, `updated_at`. Additive keys only (existing fields unchanged), so the
  Workflows page — which reads the same endpoint via a strict TS interface that
  ignores extra keys — is unaffected (verified by its build). **No `title`
  derivation here** (see R1): computing an H1 would mean reading+parsing every
  proposal file on every list/poll (O(N·P) synchronous IO in the request path). The
  title is instead shown in the detail view, derived client-side from the §1c content
  (one file read, only when a proposal is opened).
- **§1c — Proposal content read-back.** New `GET /v1/workflow/items/<id>/proposal` →
  serves the run's own `proposal_path` → `{proposal_md, truncated}`. `proposal_path`
  is **server-minted** at submit (`wi-<time>-<pid>.md`), never caller-supplied, so the
  base risk is low; still hardened defense-in-depth: `realpath()` the target and the
  proposals dir and require the resolved path to sit under
  `$AIMEE_HOME/workflows/proposals/` (rejects `..`, absolute, symlink escape,
  null-byte); `open()` then `fstat()` the fd and re-check (no prefix→open TOCTOU);
  refuse non-regular files; **cap the read** (submit already bounds the body at 1 MB;
  the endpoint enforces the same cap and sets `truncated`). Missing file → 404. Go
  proxy `/api/workflow/items/<id>/proposal`. Ownership-scoped as above.

Note: `lifecycle_event.detail` is free-form operator-facing text. Access control
(ownership scoping) is the containment; v1 does not additionally sanitize `detail`.

## §2 Proposals list + status/history page (frontend)

New page `frontend/src/pages/Proposals.tsx`, nav entry, route `/proposals`.

- **List** — proposals = work items (`GET /api/workflow/items`, enriched by §1b),
  **server-scoped to the caller's `submitter`** by default. A "show all" view exists
  but is **gated server-side on `CAP_WORKFLOW_ADMIN`** (not a mere client toggle) —
  the server filters, so a non-admin can never receive another user's rows (this,
  with §1's per-item ownership check, closes the enumeration/IDOR path). Each row:
  workflow, short id, stage, a lifecycle **badge derived strictly from
  `state` + `pause_reason` + `current_stage`** (e.g. *active / awaiting human
  approval / CI running / merging / merged (accepted) / rejected / parked:
  panel_degraded|budget_exceeded|failed|…*), cost, updated-at. (No "drafting" badge —
  an unsubmitted draft is a client-only state in §3; a submitted item is `active`.)
- **Detail / status** — for a selected proposal:
  - **Status header**: `current_stage`, lifecycle badge, cumulative vs. cap cost, PR
    link (`pr_ref`), timestamps, and the title (H1 parsed client-side from the §1c
    markdown).
  - **Timeline** (§1a): a scrollable, oldest→newest feed of `lifecycle_event`s —
    each entry shows stage, a human label for `kind`, actor, `detail`, cost, and
    time. Fetched as a paginated tail, then extended incrementally on poll. This is
    the "scroll back and find the history of what happened" surface.
  - **Proposal** (§1c): the rendered source markdown.
  - **Actions**: Approve / Reject are shown **only when `pause_reason ==
    pending_human`** (the human-approval gate — distinct from a roundtable/CI/merge
    park, which are read-only status), with the parked `current_stage` labeled so the
    user knows *which* gate they're deciding. Reuses the existing gate endpoint; the
    call requires `CAP_WORKFLOW_ADMIN` server-side, and the UI disables/annotates the
    buttons when the caller lacks it rather than letting the call fail opaquely.
  - Live refresh: poll the item + incremental events every ~4 s **while
    `state == active`**, and stop once the item reaches a terminal `state`
    (`accepted`/`rejected`/`abandoned`) — no polling of finished proposals.

## §3 Author from scratch (frontend)

- A **New proposal** composer: title + markdown body with a template scaffold
  (Goal / Motivation / Approach / Risks / Tests), workflow picker (default `build`),
  optional repo. Client-side **draft** persistence in localStorage, **keyed by the
  authenticated user id and cleared on logout** (so a shared browser doesn't leak a
  draft across accounts), size-capped, with a visible "draft saved locally on this
  device" caveat (not cross-device; lost on browser-data clear). A server-side draft
  store is explicitly deferred (see Non-goals).
- **Submit** posts to `/api/dev/submit`; on success the page switches to the new
  proposal's status view (§2), so authoring flows straight into watching it run.

## §4 Delegate-assisted drafting (optional, later slice)

A **Draft/refine with a delegate** action: send the working title + notes to a
delegate (via the existing delegate/roundtable surface) to produce or improve the
proposal markdown, returned into the editor for the user to edit before submit.
Kept as its own slice so §2/§3 ship without it.

## §5 Separate Workflows from Proposals (migration)

Today the Workflows page (`frontend/src/pages/Workflows.tsx`) mixes two concerns:
def authoring (defs list, blocks palette, graph canvas, node inspector, personas)
**and** proposal/run concerns (a "Submit proposal" panel → `/api/dev/submit`, a
"Runs" list, a "Run state" inspector panel, and the gate Approve/Reject). Those
proposal/run concerns move to the Proposals page so each page owns one job:

- **Workflows** becomes the **def/graph editor only** (author, validate, save
  workflow definitions; assign personas to steps). It keeps `/api/workflow/{blocks,
  defs,validate,save}`.
- **Proposals** owns submission (`/api/dev/submit`), the run/work-item list, the
  status/history detail, and gate decisions.

**Sequencing to avoid any functionality gap** (each still an independent, shippable
slice):
- The **Runs list + Run-state panel + gate Approve/Reject** are removed from
  Workflows in **Slice 1**, the same slice that introduces the richer Proposals
  status view — so the capability never disappears, it relocates.
- The **"Submit proposal" panel** is removed from Workflows in **Slice 2**, the same
  slice that adds the Proposals composer — so submission is always reachable
  somewhere.

The `submitProposal`/`decideGate`/`runItem` state and their handlers are deleted from
`Workflows.tsx` as those panels move; the Proposals page reimplements them against the
same endpoints. No backend endpoint is removed (Workflows and Proposals share
`/api/workflow/items` and the gate route).

## Data model summary

**No new tables, no new columns.** Everything reads existing `lifecycle_work_item` /
`lifecycle_event` state. Ownership uses the existing `submitter` column (populated for
webchat submits by the intake-auth path, `done/full-autonomous-development.md`;
NULL/legacy rows are admin-only per §1). Title is parsed client-side from the proposal
markdown in the detail view; drafts live client-side.

## Surface

New endpoints (all additive, read-only except the pre-existing gate/submit):
- `GET /v1/workflow/items/<id>/events?after=<id>&limit=<n>` (paginated timeline),
- `GET /v1/workflow/items/<id>/proposal` (source markdown, path-confined),
- enriched `GET /v1/workflow/items[/<id>]` with a **`?scope=mine|all`** filter
  (`all` requires `CAP_WORKFLOW_ADMIN`; default `mine` filters on `submitter`
  server-side),

each with a `/api/*` webchat proxy and all ownership-scoped (§1). New page
`/proposals`. Reuses `POST /api/dev/submit` and
`POST /api/workflow/items/<id>/gate` unchanged.

## Phasing (each independently shippable + roundtable-approved + merged)

- **Slice 0 — read surfaces (§1a/§1b/§1c):** backend only; verifiable via curl +
  unit tests (dispatch/http/conformance). No UI. Ship first so the page has data.
- **Slice 1 — Proposals list + status/history page (§2):** read-only over Slice 0 +
  existing submit/gate. Delivers the "watch it end-to-end" value. **Also removes the
  Runs/Run-state/gate panels from Workflows** (§5) — the capability relocates, not
  disappears.
- **Slice 2 — Author from scratch (§3):** the composer + client-side draft + submit.
  **Also removes the "Submit proposal" panel from Workflows** (§5).
- **Slice 3 — Delegate-assisted drafting (§4):** optional enhancement.

## Flags

The read surfaces are additive dashboard reads (no flag, like `agent.stats`). The
page is additive UI. **Autonomous execution / live forge remains default-off behind
`wfe_live_forge_enabled`** — this proposal does not change that; the page only drives
what the operator has already enabled, and surfaces gate parks for human decision.

## Non-goals

- No second work model; no changes to `work_queue` or `roundtable_pipeline_*`.
- No per-delegate drill-down inside a stage in v1: `agent_jobs`/`token_audit`/
  `execution_trace` do **not** carry `work_item_id`, so stage-level events (with the
  `detail`/`cost_usd` already on `lifecycle_event`) are the granularity. Threading
  `work_item_id` through execution tables for deep drill-down is future work.
- No server-side draft store in v1 (client-side only).
- No new authoring of workflow *defs* — that stays on the Workflows page.

## Risks / honest limits

- **Timeline detail is only as rich as `lifecycle_event.detail`.** Some transitions
  write terse details; the page shows what's there and won't fabricate more. `detail`
  is served verbatim and is not sanitized in v1 — access is contained by ownership
  scoping (§1), so only the owner or an admin sees it.
- **Cost is cumulative at the item level** (`cum_cost_usd`), not per-delegate.
- **The read endpoints are the main new attack surface.** Mitigated by: per-item
  ownership scoping (§1), realpath/fstat path confinement + size cap on `/proposal`
  (§1c), and events pagination bounding response size (§1a).
- **Ownership rests on `submitter`.** Rows without it (CLI/legacy/system) are
  admin-only, never shown to a scoped user — fail-closed, but it means the default
  list omits non-webchat-submitted items unless viewed as admin.
- **Gate actions require `CAP_WORKFLOW_ADMIN`** server-side; a non-admin user sees the
  timeline/status but the Approve/Reject call is refused (the UI disables it).
- **Polling, not push:** status refresh is interval-based (no websocket), matching the
  existing dashboard pattern.

## Tests

- Slice 0: unit tests for the new routes — events list shape **+ pagination
  (`after`/`limit`, `next_after`)**; proposal read-back **+ traversal/symlink/absolute
  rejection + size cap**; **ownership scoping (owner allowed, non-owner denied, admin
  allowed, NULL-submitter admin-only)**; `?scope=mine|all` filter (all requires admin);
  `item_to_json` additive-fields snapshot; dispatch-caps / v1-method-coverage /
  cli-v1-routes / server-api-conformance / openapi doc.
- Slice 1–2: `tsc -b && vite build`; verify the Workflows page still builds against the
  enriched item JSON (extra keys ignored); a live end-to-end on pve — submit a trivial
  proposal against a test CT, watch it advance/park, decide a gate, confirm the
  timeline renders and extends incrementally.

## Review revisions (R1 — proposal roundtable)

Roundtable review of this design (7 panelists, 4 survivors). Applied:

- **Closed the IDOR** (findings on §1a/§1c/§2): the per-item reads (`/events`,
  `/proposal`, enriched single-item) are now **ownership-scoped in the C handler**
  (`submitter == caller` OR `CAP_WORKFLOW_ADMIN`), not just `CAP_DASHBOARD_READ`; the
  list filter (`?scope=mine|all`) is **server-side** and `all` is admin-gated. NULL
  `submitter` rows are admin-only (fail-closed).
- **Hardened §1c path confinement**: realpath resolution + confine under the proposals
  dir, reject `..`/absolute/symlink-escape/null-byte, open-then-`fstat` (no TOCTOU),
  regular-file check, and an explicit size cap (`truncated` flag). Noted that
  `proposal_path` is server-minted, so this is defense-in-depth.
- **Removed the O(N·P) title derivation**: `item_to_json` now emits only cheap DB
  columns; the H1 title is parsed **client-side in the detail view** from the §1c
  content, so the list/poll path does no per-row file IO.
- **Added events pagination** (`after`/`limit`/`next_after`) so a long/looping run's
  timeline is fetched as a tail and extended incrementally, never refetched whole.
- **Specified poll cadence** (~4 s while `active`, stop at terminal) and **gate
  semantics** (Approve/Reject only for `pending_human`, with the parked stage labeled;
  read-only for panel/CI/merge parks).
- **Draft hygiene**: localStorage keyed to the authenticated user id, cleared on
  logout, size-capped, with a device-local caveat.
- **`show all` is admin-gated server-side**, not a client toggle.

Triaged as **not-actionable / already-handled**: additive `item_to_json` keys don't
break the Workflows page (TS structural typing ignores extra keys — kept, verified by
its build); "drafting" badge removed (badge derives strictly from
`state`+`pause_reason`+`current_stage`); per-delegate drill-down stays a Non-goal
(execution tables lack `work_item_id`); server-side draft store deferred to Non-goals.

---

## Close-out (shipped)

**DONE — all four slices merged to `testing`.** Built slice-by-slice, each design and
code roundtable-reviewed before merge:

- **Slice 0 — backend read surfaces** (#954): paginated lifecycle timeline
  (`/v1/workflow/items/<id>/events`), path-confined proposal read-back (`/proposal`),
  operator `/all`, and owner-scoped + enriched `item_to_json`. Behavioral tests in
  `test_wfe_webapi.c` (ownership allow+deny, pagination cursor, symlink→403).
- **Slice 1 — Proposals page + Workflows de-scope** (#956): list + status/history
  detail; Runs/Run-state/gate removed from Workflows.
- **Slice 2 — author-from-scratch composer** (#959): composer + client-side draft;
  Submit-proposal panel removed from Workflows. Also repaired pre-existing `testing`
  breakage (clang-format + an `audit_args_hash` test-link `undefined reference`).
- **Slice 3 — delegate-assisted drafting** (#963): a tool-free one-shot completion
  (`agent_generate` → `agent_execute`, non-CLI, no worktree) behind `POST
  /v1/agent/draft`; the composer's "Draft with a delegate" button.

**Design notes that changed under review:** R-Q1 resolved to a constrained,
**non-agentic** draft path — the plan roundtable rejected reusing the agentic
`/v1/delegate/run` (tool access + worktree + zombie jobs behind a browser button). The
cap model landed on **owner-only per-item reads** (no admin bypass; `/all` is the
admin list) rather than a per-request caps accessor.

**No new tables or columns.** A proposal remains markdown + a `lifecycle_work_item`.

**Feature documentation:** [`docs/WORKFLOW_ACTIONS.md`](../../WORKFLOW_ACTIONS.md)
(roundtable-approved).

**Carried as future work / known limits:** headless browser click-through of the live
UI (the remaining manual verification step); per-delegate drill-down inside a stage
(execution tables lack `work_item_id`); a lighter dedicated draft endpoint if the
synchronous worker-hold ever matters; server-side draft persistence; cross-user admin
inspection of another user's timeline/proposal.
