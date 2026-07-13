# Workflow Actions (web page)

The **Workflow Actions** page (aimee web UI, left nav → 📝 Workflow Actions) carries a change from
an empty editor to a merged PR: **author** a proposal (from scratch or
delegate-drafted), hand it to the **autonomous-development** engine to implement, and
**watch** it, a scrollable status/history you can scroll back through to see
everything that happened.

It is deliberately a thin surface over machinery that already exists: the wfe
autonomy engine (see [`AUTONOMOUS_DEVELOPMENT.md`](AUTONOMOUS_DEVELOPMENT.md)). A
proposal is still just *markdown + a `lifecycle_work_item`*; its history *is* the
`lifecycle_event` log. **No new database tables or columns were added.**

- **Nav / route:** `/workflow-actions` (`frontend/src/App.tsx`), page
  `frontend/src/pages/WorkflowActions.tsx`.
- **Separation of concerns:** the **Edit Workflows** page (`/edit-workflows`) is now the
  workflow-*definition* editor only (author/validate/save graphs, assign personas).
  All run/submit/status concerns live on **Workflow Actions**.

---

## User flow

1. **Author**: click **+ New proposal**. Fill in a title, a Markdown body (a
   Goal / Motivation / Approach / Risks / Tests scaffold is pre-filled), pick a
   workflow (the picker is populated from the saved workflow definitions,
   `GET /api/workflow/defs`; it defaults to `build`, the standard end-to-end
   proposal→PR→merge workflow), and optionally a repo. Optionally click
   **✨ Draft with a delegate** to have a delegate expand your title + notes into a
   polished proposal, previewed for you to accept.
2. **Submit**: posts the proposal to the autonomous-development intake. The page
   switches to the new run's status view.
3. **Watch**: the detail view shows the current stage, a lifecycle badge, cumulative
   vs. cap cost, a PR link, and a **scrollable timeline** of every lifecycle event.
   It polls while the run is active and stops at a terminal state.
4. **Decide**: when the run parks at a human gate, **Approve / Reject** in place.

---

## Architecture

The browser never talks to the C server directly. Two servers:

```
browser ──/api/*──▶ webchat (Go)  ──/v1/*, UDS──▶ aimee-server (C)  ──▶ DB1 (sqlite)
                     v1RequestWebuser                lifecycle_work_item / lifecycle_event
                     (X-Aimee-Webuser + server.token)  wfe scheduler (autonomy engine)
```

- The Go webchat proxies each `/api/*` call to the C server's `/v1/*` HTTP surface
  over a trusted unix socket, asserting the caller's identity with the
  `X-Aimee-Webuser` header + `server.token` bearer (`v1RequestWebuser`). This is how
  the C server resolves the caller's `webuser:<subject>` principal, which the
  read endpoints use for ownership scoping (below).
- The C server owns all state. Submitting writes the proposal markdown to
  `$AIMEE_HOME/edit-workflows/workflow-actions/wi-*.md` and a `lifecycle_work_item` row; the wfe
  scheduler drives that item through its workflow, appending a `lifecycle_event` per
  transition.

---

## Backend endpoints

All C routes are in `src/server/server_http_routes.inc`; handlers in
`src/server/server_workflow_api.c` (run-state) and `src/server/server_agent.c`
(drafting). Every Go proxy is registered in `webchat/server.go`.

### Run-state reads (Slice 0)

| Browser (`/api`) | Server (`/v1`) | Cap | Returns |
|---|---|---|---|
| `GET /api/workflow/items` | `GET /v1/workflow/items` | `CAP_DASHBOARD_READ` | the caller's **own** items (submitter-scoped) |
| `GET /api/workflow/items/all` | `GET /v1/workflow/items/all` | `CAP_WORKFLOW_ADMIN` | **all** items (operator view) |
| `GET /api/workflow/items/<id>` | `GET /v1/workflow/items/<id>` | `CAP_DASHBOARD_READ` | one item (owner-only) |
| `GET /api/workflow/items/<id>/events` | `GET /v1/workflow/items/<id>/events` | `CAP_DASHBOARD_READ` | paginated lifecycle timeline (owner-only) |
| `GET /api/workflow/items/<id>/proposal` | `GET /v1/workflow/items/<id>/proposal` | `CAP_DASHBOARD_READ` | source proposal markdown (owner-only) |

- **Enriched item** (`item_to_json`): the existing keys (`id, workflow, version,
  stage, state, mode, pause_reason, repo`) plus additive `proposal_name`, `pr_ref`,
  `submitter`, `cum_cost_usd`, `work_item_max_cost_usd`, `override_count`. Additive
  only, so the Edit Workflows page (which reads the same endpoint) is unaffected.
- **Timeline**: `{events:[{id, stage, kind, actor, detail, cost_usd, created_at}],
  next_after}`, oldest-first, paginated by `?after=<event id>&limit=<n≤200>`
  (default 200). `next_after` is the id of the **last returned** event, so the page
  fetches the tail once and polls with `after=next_after` to append only new events.
  Correctness rests on the scheduler being single-writer-per-item (concurrency 1) and
  event ids being monotonic, so `id > after` never skips or duplicates a row.
- **Proposal read-back**: `{proposal_md, truncated}`. The file is read **race-free**:
  the fixed proposals dir is opened, then `openat(dirfd, basename, O_NOFOLLOW)`. Since
  `proposal_path` is a flat, server-minted file in one directory, there are no
  mid-path components to race and no symlink is followed (`ELOOP → 403`). Non-regular
  files are refused; the read is capped at 1 MB with `truncated` set rather than
  rejected.

### Drafting (Slice 3)

| Browser (`/api`) | Server (`/v1`) | Cap | Returns |
|---|---|---|---|
| `POST /api/proposal/draft` | `POST /v1/agent/draft` | `CAP_DELEGATE` | `{text, agent}` |

- Request `{prompt, model?}`; response `{text, agent}` where `text` is the generated
  proposal markdown and `agent` is the **name of the configured delegate** that
  produced it (the caller's `model` preference if it named a usable non-CLI delegate,
  otherwise the default or first eligible one, surfaced so the operator can see who
  drafted it). `handle_agent_draft` (`server_agent.c`) runs one **tool-free** LLM
  completion via `agent_generate` (below) and returns the text.
  `CAP_DELEGATE` is satisfied by the `agent.*` capability prefix
  (`src/server/server_auth.c`).
- The call is **synchronous**: there is no async job to orphan. A worker thread runs
  the single completion to completion (bounded by the model and the draft token cap)
  and returns. If the browser gives up, the worker still finishes that one
  completion; nothing lingers as a pollable job. Because an LLM completion can exceed
  the webchat default 10 s timeout, the proxy uses `v1RequestWebuserT` (a
  timeout-parameterized variant of `v1RequestWebuser`, `webchat/vault.go`) with a 95 s
  budget.
- **Errors** (`handle_agent_draft`): a missing/short prompt → `400`; no non-CLI
  delegate configured, or the completion failing (model error, timeout, empty
  response) → a `{"error": …}` body the composer surfaces inline next to the button.

### Reused, unchanged

- **Submit:** `POST /api/dev/submit {proposal_md, workflow, repo}` → the existing
  autonomous-development intake (`/v1/dev/submit`, `CAP_DELEGATE`).
- **Gate:** `POST /api/workflow/items/<id>/gate {decision}` → the existing operator
  gate (`CAP_WORKFLOW_ADMIN`); the gate is always the row's parked stage, never
  trusted from the request.

---

## Security model

- **Ownership scoping (per-item reads).** `/events`, `/proposal`, and the single-item
  GET are owner-only, enforced *in the C handler* (`wf_owns`): the row's `submitter`
  must string-equal the caller's `server_http_identity_principal()` (`webuser:<subj>`).
  A NULL/empty `submitter` (CLI/legacy/system rows) is owned by nobody, so those reads
  fail closed. This is why the Go proxies use `v1RequestWebuser` (not the un-attested
  `v1Request`), otherwise the principal would be empty and every read would 403.
- **List scoping.** `GET /v1/workflow/items` returns only the caller's own rows; the
  unscoped operator view is a separate route, `GET /v1/workflow/items/all`, statically
  gated by `CAP_WORKFLOW_ADMIN`. The `/all` route is deliberately *not* owner-filtered,
  so it is the only surface on which NULL/empty-`submitter` rows (CLI/legacy/system
  items, invisible to the owner-scoped reads) appear.
- **How capabilities are acquired.** Browser users authenticate to the webchat; the
  webchat→server hop is over the filesystem-trusted UDS, and the C server treats that
  channel as the operator (this is the established single-trusted-operator webchat
  model, the same one that guards the vault and the workflow gate). The per-route
  `CAP_*` values are the transport/route gates; the *per-user* narrowing that matters
  for this feature is the in-handler ownership check on `submitter`, which is why the
  proxies must forward the webuser identity.
- **Path confinement.** The proposal read-back is dirfd + `openat(O_NOFOLLOW)` on a
  server-minted basename: no traversal, no symlink follow, no TOCTOU (details above).
- **Delegate drafting is tool-free.** `agent_generate` (`src/server/agent_runtime.c`)
  selects a single **non-CLI** (HTTP-provider) delegate and calls the plain-completion
  **`agent_execute()` directly**. The tool-execution loop lives only in the *separate*
  `agent_execute_with_tools_for_role()`, which `agent_generate` never calls, so a
  draft returns text and nothing else, regardless of the agent's `tools_enabled`: no
  tools, no worktree, no writes, no repo access. It refuses if only CLI (agentic)
  agents exist. The user's title/notes are the *subject*, framed by a fixed system
  prompt that tells the model to treat them as data. **Residual risk (bounded, not
  zero):** because there are no tools there is no *server-side* side effect, but the
  model can still produce misleading or malicious *markdown* (e.g. a phishing link) in
  the draft. That draft is shown as a preview and the user must explicitly accept it.
  The human reviewer owns the same trust judgment they would for any hand-written
  proposal. The rendered preview cannot inject script (it goes through the escaping
  `renderMd`, below), and an accepted draft only becomes a normal proposal the user
  then submits.
- **No admin bypass on per-item reads.** `wf_owns` is strictly `submitter == caller`;
  there is *no* `CAP_WORKFLOW_ADMIN` override for `/events`, `/proposal`, or the
  single-item GET. An admin's extra reach is the `/all` *list* only (item rows, which
  do not include event `detail` or the proposal body). Consequently
  **`lifecycle_event.detail`** (served verbatim, not sanitized in v1) is visible only
  to the **owner** of the item. (Cross-user admin inspection of another user's timeline
  or proposal is deliberately deferred; use the CLI/DB for that.)

Where these are stated as guarantees they are *design properties of this code path*,
not absolutes: the tool-free property holds as long as the draft path calls
`agent_execute()` (not the tools variant); the path-confinement property holds because
the proposals directory is server-owned (created `0700`) and only server-minted
basenames are opened.

---

## Frontend behavior (`WorkflowActions.tsx`)

- **Timeline polling.** Keyed on the selected id alone; each tick appends only events
  past the cursor (idempotent: it filters `id > lastId`), and the interval self-stops
  once the item reaches a **terminal** `state`. The terminal set is exactly the
  non-`active` values of the authoritative work-item `state` enum:
  `active | accepted | rejected | abandoned` (`db1_work_item_t`; see
  [`AUTONOMOUS_DEVELOPMENT.md`](AUTONOMOUS_DEVELOPMENT.md)), i.e. `accepted`,
  `rejected`, `abandoned`. A **parked** run is *not* a separate state: it stays
  `active` with a non-empty `pause_reason` (`pending_human`, `failed`,
  `budget_exceeded`, …), so the page keeps polling it every ~4 s (an operator can still
  resume or override a park); polling stops only when the item actually reaches one of
  the three terminal states. `openProposal` guards its async loads with a monotonic
  request token so a rapid re-selection can't let a stale fetch clobber the current
  proposal. A transient poll error is swallowed and retried on the next tick; a
  list/detail load error surfaces a status message.
- **Proposal markdown** (both the run's source and a draft preview) is rendered with
  the shared `renderMd`. It is not a general HTML sanitizer: it escapes the raw source
  (`&`, `<`, `>`) and then emits only a fixed, closed set of tags (headings,
  paragraphs, lists, blockquotes, inline emphasis/code, fenced code, and `<a>` links
  restricted to `http(s)` targets), so no author-supplied HTML, event handler, or
  `javascript:`/SVG payload reaches the DOM. Practically: LLM- or user-authored
  markdown cannot inject script into the page.
- **Composer draft.** The in-progress draft is persisted in `localStorage`
  (`aimee_proposal_draft`), restored on mount, and cleared on a successful submit and
  on logout (`App.tsx`). `localStorage` is origin-wide, so a draft persists on the
  device until one of those clears runs. The cross-account protection is "logout
  clears it," not per-user isolation. A server-side per-user draft store is a noted
  non-goal.
- **Draft-with-a-delegate** shows the result as a **preview**; the body is replaced
  only on an explicit **Use this draft** (never silently). The action is
  re-entrancy-guarded and the composer inputs are disabled while a draft or submit is
  in flight.
- **Gate.** Approve/Reject appears only when `pause_reason == pending_human`; a 403
  (caller lacks `CAP_WORKFLOW_ADMIN`) surfaces its message rather than pre-disabling.

---

## Slices (all merged to `testing`)

Built slice-by-slice, each design-and-code roundtable-reviewed before merge:

- **Slice 0: backend read surfaces** (PR #954): the timeline/proposal/enriched-item
  endpoints + owner scoping. Behavioral unit tests in `test_wfe_webapi.c`
  (ownership allow+deny, pagination cursor, proposal read-back, symlink→403,
  `/all` enrichment).
- **Slice 1: Workflow Actions page + Edit Workflows de-scope** (PR #956): the list + status/
  history detail; removes the Runs/Run-state/gate panels from Edit Workflows.
- **Slice 2: author-from-scratch composer** (PR #959): the composer + client-side
  draft; removes the Submit-proposal panel from Edit Workflows. (This PR also repaired
  pre-existing `testing` breakage from an unrelated merge, a clang-format violation
  and an `audit_args_hash`/`wfe_sha256_raw` test-link `undefined reference`.)
- **Slice 3: delegate-assisted drafting** (PR #963): `agent_generate` + `/v1/agent/draft`
  + the composer's Draft button.

PRs (github.com/RakuenSoftware/aimee): [#954](https://github.com/RakuenSoftware/aimee/pull/954),
[#956](https://github.com/RakuenSoftware/aimee/pull/956),
[#959](https://github.com/RakuenSoftware/aimee/pull/959),
[#963](https://github.com/RakuenSoftware/aimee/pull/963). The design proposal and
implementation plan are under [`docs/workflow-actions/`](proposals/) (`proposals-ui-page.md`,
`proposals-ui-page.plan.md`).

---

## Configuration & operation

The page is read/UI only; the behavior it drives is governed by existing config:

- **Drafting** needs at least one enabled **non-CLI** (HTTP-provider) delegate in the
  agent config (see [`DELEGATES.md`](DELEGATES.md)); with only CLI delegates, the draft
  endpoint returns an error and the button surfaces it.
- **Autonomous execution** is governed by the wfe engine
  ([`AUTONOMOUS_DEVELOPMENT.md`](AUTONOMOUS_DEVELOPMENT.md)). The **live forge is
  default-on** (`wfe_live_forge_enabled`); set it to `false` to exercise the page
  without real pushes — a run then advances and parks at gates instead of opening a
  PR. Even on, the merge-target rail bounds every op (PRs open-only against the
  trunk; merges only to the unprotected autonomous base).
- **Submit** is bounded by the intake's existing caps (a 1 MB proposal body cap and the
  intake-auth per-principal rate/concurrency limits); the user-supplied `repo` is
  handled by the same `/v1/dev/submit` intake that the CLI uses; its validation and
  authorization are the intake's concern, unchanged by this feature.
- **To exercise it end to end** on a running instance: open `/workflow-actions`, **+ New
  proposal**, optionally **Draft with a delegate**, **Submit**, then watch the timeline
  advance and (with live forge off) park at the first human gate, where **Approve**
  resumes it.

---

## Known limits / non-goals

- **No per-delegate drill-down inside a stage.** `agent_jobs` / `token_audit` /
  `execution_trace` do not carry `work_item_id`, so the timeline's granularity is the
  stage-level `lifecycle_event` (with its `detail`/`cost_usd`).
- **Cost is cumulative at the item level** (`cum_cost_usd`), not per-delegate.
- **No server-side draft store** (client-side localStorage only) and **no streaming**
  of the draft (the final result is returned in one call).
- **Synchronous drafting holds a worker** for the LLM latency (≤95 s), consistent with
  the server's other LLM-backed endpoints (chat, delegate).
- **Verification:** each slice's `e2e-docker` CI (full stack) is green and the backend
  read paths have behavioral unit tests; a headless browser click-through of the live
  UI is the remaining manual step.
