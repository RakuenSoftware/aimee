# Implementation plan: Proposals web page

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Companion to `proposals-ui-page.md` (approved). Slice-by-slice; each slice is
roundtable-approved and merged before the next. Branches off `testing`.

## Resolved seams (from a backend read)

- **Caller identity**: `server_http_identity_principal()`
  (`src/server/server_http_identity.c:111`) returns the attested principal — for
  webchat it is `"webuser:<subject>"` (git handlers already scope per-user with it,
  `server_http_routes.inc:1099`). This is the ownership key.
- **`submitter`**: populated on webchat submits by the intake-auth path
  (`db1_work_item_submit_capped`, `done/full-autonomous-development.md`). Slice 0
  step 1 **verifies** `submitter == principal` at submit and adds a `submitter`
  accessor on `db1_work_item_t` if not already surfaced.
- **Event cursor**: `db1_lifecycle_event_t.id` is a `long` (`wfe_store.h:35`) —
  monotonic per-item; use it for `after`/`next_after` pagination.
- **Cap model — R-Q1 RESOLVED → Option B** (plan roundtable: the per-request caps
  accessor's cross-request-bleed surface isn't worth it for v1). Caps are enforced
  statically per route *before* the handler; `route_req_t` carries no caps and the
  identity layer exposes no caps accessor — and we add **no new one**. Instead:
  - **Per-item reads** (`/events`, `/proposal`, single-item GET) are **owner-only**:
    route cap `CAP_DASHBOARD_READ`, handler enforces `wf_owns` (`strcmp(principal,
    wi->submitter)==0`, both non-empty) → 403 otherwise. No admin bypass in v1.
  - **Global list** is a **separate route** `GET /v1/workflow/items/all` with static
    cap `CAP_WORKFLOW_ADMIN`; the default `GET /v1/workflow/items` is owner-scoped.
  This adds zero core-security surface (no thread-local caps snapshot → no
  privilege-bleed risk). Cost: an admin can't web-read a *non-owned* item's
  events/proposal in v1 (CLI/DB, or a later Option-A follow-up if multi-operator
  admin review is needed). For a single-operator deployment (owner == the gate
  admin) this is a non-issue.
- **Only consumer of `/v1/workflow/items`** is the webchat Workflows page
  (`webchat/workflow.go:86`); no dashboard reads it — so re-scoping it to the caller
  is regression-safe.

---

## Slice 0 — backend read surfaces (no UI)

**Files:** `src/server/server_workflow_api.c` (handlers), `src/server/server.h`
(decls), `src/server/server_http_routes.inc` (routes), `src/db1/wfe_store.h`
(confirm `submitter` on `db1_work_item_t`), `webchat/workflow.go` + `webchat/api.go`
(proxies + methodRoutes), `api/openapi-server-v1.yaml`, `src/cli_v1_routes_gen.inc`
(regen), `src/tests/test_server_http.c` (+ a focused wf-api test). **No core identity
change** (Option B).

**Steps:**
1. **Ownership helper** — static `wf_owns(const db1_work_item_t *wi)` in
   server_workflow_api.c: `wi->submitter[0] && server_http_identity_principal()[0] &&
   strcmp(principal, wi->submitter) == 0` (**string** compare; NULL/empty submitter →
   NOT owned → owner-only reads refuse it, fail-closed). Confirm the submit path sets
   `submitter = principal`; if `db1_work_item_t` lacks a `submitter` field, add the
   accessor (read-only, no schema change).
2. **Enrich `item_to_json`** (`:398`) — add cheap DB columns only:
   `proposal_name` (basename of `proposal_path`; omitted when empty), `pr_ref`,
   `cum_cost_usd`, `work_item_max_cost_usd`, `override_count`, `submitter`,
   `created_at`, `updated_at`. Additive keys; existing keys unchanged.
3. **List scoping** — `wf_api_items` filters to `wf_owns()` rows (owner-scoped). A
   **separate** `wf_api_items_all` backs `GET /v1/workflow/items/all`
   (cap `CAP_WORKFLOW_ADMIN`) returning all rows. No query param needed.
4. **Timeline** — new `wf_api_events(id, after, limit, resp, cap)`:
   `db1_work_item_get` → `wf_owns` (403 else) → `db1_lifecycle_event_list` →
   keep events with `id > after`, take the first `min(limit,200)` (default 200) →
   `{events:[{id,stage,kind,actor,detail,cost_usd,created_at}], next_after}` where
   **`next_after` = the `id` of the LAST event actually returned** (post-limit), else
   echoes `after` when none. Correctness: `lifecycle_event.id` is monotonic and, per
   the scheduler (concurrency 1, `wfe_scheduler.c`), a work item has a **single
   writer** — so `id > after` with `next_after`=last-returned neither skips nor
   duplicates across polls even while new events append. (In-handler windowing over
   the returned list; DB-side `WHERE id > ? LIMIT ?` is a later optimization — per-item
   event counts are bounded.)
5. **Proposal read-back** — new `wf_api_proposal(id, resp, cap)`:
   `db1_work_item_get` → `wf_owns` (403). Then read the file **race-free via a
   dirfd**, exploiting that `proposal_path` is a *flat* file in one fixed dir:
   - take `base = basename(proposal_path)`; reject if it contains `/`, is empty, `.`,
     or `..` (must be a simple filename);
   - `dirfd = open("$AIMEE_HOME/workflows/proposals", O_DIRECTORY|O_RDONLY|O_CLOEXEC)`
     (the dir is a fixed trusted constant — its own confinement);
   - `fd = openat(dirfd, base, O_RDONLY|O_NOFOLLOW|O_CLOEXEC)` — `O_NOFOLLOW` guards
     the only (final) component; there are **no mid-path components**, so no mid-path
     symlink/TOCTOU exists;
   - `fstat(fd)`: require a **regular file**; read up to 1 MB → `{proposal_md,
     truncated}` with `truncated=true` iff the file exceeds the cap (**truncated, not
     rejected**, so a huge file still renders with a banner). Missing file → 404;
     non-regular / symlinked / bad basename → 403.
6. **Routes** (`server_http_routes.inc`, RM_PREFIX for `<id>`): `GET
   /v1/workflow/items/<id>/events`, `GET /v1/workflow/items/<id>/proposal` (cap
   `CAP_DASHBOARD_READ`, ownership in-handler — comment this clearly for maintainers),
   and `GET /v1/workflow/items/all` (cap `CAP_WORKFLOW_ADMIN`). Add `rh_wf_events` /
   `rh_wf_proposal` / `rh_wf_items_all` wrappers extracting `rq->id` + query
   (`after`, `limit`). Note: the `/all` route must be registered so it doesn't collide
   with the `<id>` prefix matcher (exact match before prefix, or a distinct segment).
7. **Go proxies** — `GET /api/workflow/items/<id>/events`, `/proposal`, and
   `/api/workflow/items/all` in `webchat/workflow.go` (extend `handleWorkflowItems`
   path parsing), + methodRoutes entries; pass the query string through.
8. **Conformance** — regen `cli_v1_routes_gen.inc`; document all three routes
   (incl. `/items/all` and the **owner-scoping semantic change** of `/items`) in
   `openapi-server-v1.yaml`; run dispatch-caps / v1-method-coverage / cli-v1-routes /
   server-api-conformance / docs-gen / schema-sync / line-check.
9. **Tests** — wf-api unit test: enriched-fields snapshot; **events pagination
   boundary** (limit truncates a larger set → `next_after` = last *returned* id →
   next page with `after=next_after` continues with no gap/dup); proposal read-back
   happy path + basename-with-`/` reject + symlinked-file reject (create a symlink in a
   temp proposals dir) + non-regular reject + oversize → `truncated`; ownership (owner
   200, non-owner 403, NULL-submitter 403); `/items/all` requires admin cap. Keep
   `test_server_http` + `test_server_dispatch` green (stubs for any new dispatch
   methods).

**Verify:** builds (`-Werror`), Go build/vet/fmt, all conformance + unit tests. curl
against a local/pve server: submit a proposal, GET its events + proposal, confirm a
second principal gets 403 and the `/all` route needs admin.

---

## Slice 1 — Proposals list + status/history page; de-scope Workflows

**Files:** new `frontend/src/pages/Proposals.tsx`; `frontend/src/App.tsx`
(nav + route `/proposals`); `frontend/src/pages/Workflows.tsx` (remove Runs +
Run-state + gate).

**Proposals.tsx:**
- List: `GET /api/workflow/items` (own scope) → rows (workflow, short id, stage,
  lifecycle badge from `state`+`pause_reason`+`current_stage`, cost, updated-at).
  Admin "show all" calls the `/api/workflow/items/all` route (admin-capped).
- Detail: status header (stage, badge, cost vs cap, `pr_ref` link, timestamps, H1
  title parsed client-side from the `/proposal` markdown); **timeline** from
  `/events` (fetch tail, then poll `after=<last id>` every ~4 s while `state==active`,
  stop at terminal, append-only); rendered proposal markdown (with a "truncated"
  banner when `/proposal` returns `truncated:true`); Approve/Reject shown only for
  `pause_reason==pending_human` (posts to the existing gate route). No client caps
  introspection (Option B): if the server refuses with 403 (caller lacks
  `CAP_WORKFLOW_ADMIN`), the UI surfaces that message rather than pre-disabling.
- Reuse the Delegates/Workflows inline-style + `Panel`/`Badge`/`Spinner` idiom.

**Workflows.tsx removals (§5):** delete the "Runs" `Panel`, the "Run state" inspector
`Panel`, and the `runItem`/`decideGate`/`activeStage` state + handlers. Leave defs
list, blocks palette, canvas, node inspector, personas, validate/save intact.

**Verify:** `tsc -b && vite build`; Workflows still builds/renders as a pure def
editor; **pve e2e** (see below).

---

## Slice 2 — Author from scratch; remove Submit-proposal from Workflows

- Proposals.tsx composer: title + scaffolded markdown textarea + workflow picker
  (default `build`) + optional repo; localStorage draft keyed by principal, cleared
  on logout, size-capped, device-local caveat. Submit → `/api/dev/submit` → switch to
  the new item's detail view.
- Remove the "Submit proposal" `Panel` + `proposalMd`/`submitProposal` from
  Workflows.tsx.
- **Verify:** `tsc`+build; pve: author a proposal end-to-end, watch it run.

## Slice 3 — Delegate-assisted drafting (optional)

"Draft/refine with a delegate" action → a delegate call returning markdown into the
editor. Trigger + endpoint chosen at that slice's plan; ships independently.

---

## pve test approach (Slices 1–2)

Use the `.254` aimee-server (already running, TLS `:8743`) OR a throwaway CT if a
clean DB is wanted. Flow: `./aimee` (or curl the webchat) to submit a trivial proposal
against a test repo with `wfe_live_forge_enabled` **off** (so it parks at gates
without pushing), observe the item advance/park via `/events`, decide a gate via the
UI, confirm the timeline extends incrementally and the badge tracks state. Roundtable
each slice's code (diff in the prompt) before its PR; merge after approval.

## Resolved questions (plan roundtable)

- **R-Q1 → Option B.** Owner-only per-item reads + a separate admin `/items/all`
  route; **no** new per-request caps accessor (avoids the cross-request privilege-bleed
  surface the panel flagged). Owner-or-admin per-item reads deferred to a future
  Option-A follow-up if multi-operator admin review is needed.
- **R-Q2 → re-scope `/items` to the caller.** Safe: the only consumer is the migrating
  Workflows page; the semantic change is documented in the OpenAPI spec, and the admin
  global view moves to `/items/all`.
- **R-Q3 → in-handler windowing for v1.** Acceptable given single-writer-per-item +
  bounded event counts; `next_after` = last *returned* id makes it correct. DB-side
  `WHERE id > ? LIMIT ?` is a noted later optimization.

## Plan review revisions (R1 — plan roundtable)

7 panelists, 3 survivors. Applied:
- **§1c path confinement → dirfd + `openat(O_NOFOLLOW)`** (Step 5): eliminates the
  mid-path symlink/TOCTOU the panel flagged in `realpath`+prefix — `proposal_path` is a
  flat file in one fixed dir, so `openat(dirfd, basename, O_NOFOLLOW)` with a basename
  sanity check is race-free.
- **Cap model → Option B** (Resolved seams; Steps 1/3/6): removes the new caps
  thread-local and its privilege-bleed risk; owner-only reads + admin `/items/all`.
- **Pagination contract tightened** (Step 4): `next_after` = last *returned* id;
  documented single-writer + monotonic id ⇒ no skip/dup; added a boundary test.
- **`wf_owns`**: `strcmp` (not pointer eq) + NULL/empty-submitter fail-closed
  (Steps 1); `proposal_name` omitted when empty (Step 2).
- **1 MB cap → truncate + flag, not reject** (Step 5) + a UI "truncated" banner
  (Slice 1).
- **OpenAPI** documents `/items/all` and the `/items` owner-scoping change (Step 8);
  route-collision note for `/all` vs the `<id>` prefix matcher (Step 6).
Triaged as acknowledged/deferred: in-handler windowing scale (bounded for v1; DB-side
later); "Slice 1 is a major structural change" (intended — sequenced to avoid a gap).
