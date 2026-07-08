# Implementation plan: setup wizard + per-tab tutorials

Companion to [`proposal-setup-wizard-and-tab-tutorials.md`](./proposal-setup-wizard-and-tab-tutorials.md).
This plan is the concrete, sliced build-out. Each slice is independently
shippable, **frontend-only**, and verifiable with `npm run check` + `npm test`
(no C-server or Go-webchat rebuild required for the MVP).

## Guiding decisions (design-for-implementability)

1. **No new backend for the MVP.** The proposal floated a server-side
   `GET /api/setup/state`. We defer it. Readiness is computed **client-side**
   from the values already returned by `GET /api/config` (`config.show`), and
   every wizard write goes through the **existing** `POST /api/config/set`
   allowlist. This keeps the whole feature inside `frontend/` and testable
   without a running server. A server-side readiness endpoint is a documented
   follow-up (§Future), not a blocker.
2. **Single mount point, not 11 edits.** Tutorials and the setup chip mount once
   in `App.tsx`, keyed on `location.pathname` / global shell — we do **not** edit
   each of the eleven page components. Content lives in data modules.
3. **Persistence: `localStorage` for the MVP.** "Tutorial seen" and "wizard
   dismissed" live in `localStorage` (per-browser). The proposal's per-user
   server store is a follow-up; localStorage keeps slices frontend-only and is
   the established pattern in this app (`aimee_chat_tabs`, `aimee_active_chat_tab`,
   `aimee_proposal_draft` in `App.tsx`/`LogoutButton`). Trade-off noted for the
   roundtable.
4. **Pure, unit-testable core.** Readiness logic is a pure function over a config
   object, tested with vitest — no network, no DOM. Same for the tutorial
   content registry (shape-validated by a test).

## Baseline (stock, before any change)

- `npm run check` (`tsc -b`): **clean**.
- `npm test` (vitest): **14 pass, 1 pre-existing failure** —
  `Dashboard.test.ts` "populates the guardrail audit". This failure exists on
  stock `testing` and is **out of scope**; no slice may touch Dashboard audit
  logic, and our done-bar is "no *new* test failures", not "0 failures".

## Roundtable outcome (converged, 2 rounds)

Adopted: merge Slices 2+3 into one atomic PR (chip never lands without its wizard
listener → no dead control); wizard wraps every `/api/config/set` in try/catch and
surfaces failures via `Toast` while preserving input; wizard tracks a
`pendingRestart` set from `RESTART_KEYS` and lists it on the summary; chip→wizard
wiring is a `window` `CustomEvent('aimee:open-setup-wizard')`; `readiness.test.ts`
grounds `READINESS_KEYS ⊆ settingsHelp` keys. **One deviation:** the proposed
"tutorial body is a substring of settingsHelp/page text" provenance test is
infeasible for freshly-authored copy — replaced with "every `NAV_ITEMS` route has
a non-empty title+body and any `seeAlso` references a real route."

Net slice set: **Slice 1 (tutorials)** and **Slice 2 (onboarding = readiness +
chip + wizard, atomic)** → both PR into `feat/setup-wizard` → `feat/setup-wizard`
→ `testing`.

## Slices

### Slice 1 — Per-tab tutorials (lowest risk, ships first)

**Files**
- `frontend/src/help/tutorials.ts` — `TAB_TUTORIALS: Record<route, {title; body: string[]; seeAlso?}>` for all 11 routes in `NAV_ITEMS`. Copy is 3–5 lines/tab, grounded in each page's real behaviour (see proposal §2.2).
- `frontend/src/help/tutorialState.ts` — `hasSeen(route)`, `markSeen(route)`, `resetAll()` over `localStorage` key `aimee_tutorial_seen` (JSON string[]). Guarded against malformed/absent storage.
- `frontend/src/components/TabTutorial.tsx` — given the active route: first visit auto-opens a dismissible overlay; after dismissal a "?" button re-opens it. No content for a route → renders nothing (graceful).
- `frontend/src/App.tsx` — mount `<TabTutorial route={location.pathname} />` once inside `<main>`; no per-page edits.
- `frontend/src/pages/Settings.tsx` — add a "Replay tab tutorials" button calling `resetAll()` (small, localized addition).
- `frontend/src/help/tutorials.test.ts` — asserts every `NAV_ITEMS` route has a tutorial entry with non-empty title+body; `tutorialState` seen/reset round-trips (jsdom localStorage).

**Acceptance**
- `npm run check` clean; `npm test` shows **no new failures** (still only the pre-existing Dashboard one).
- Every route in `NAV_ITEMS` has a tutorial (asserted by test). "Replay" clears seen-state.

### Slice 2 — Setup onboarding (readiness + chip + wizard, ATOMIC)

Chip and wizard land together so no non-functional control is ever merged.

**Files**
- `frontend/src/setup/readiness.ts` — `type StepId = 'provider'|'embedding'|'db2'|'kb_api'|'project'`; `READINESS_KEYS` constant (the inspected config keys); `computeReadiness(cfg: Record<string,unknown>, hasProject: boolean): {ready; steps: Record<StepId,{ok; detail; optional?}>}`. Pure. Rules grounded in `settingsHelp.ts` semantics (blank `embedding_command` **and** blank `embedding_endpoint` → hash fallback → not ok; `kb_api` optional).
- `frontend/src/setup/readiness.test.ts` — table-driven cases (all-unset → provider/embedding/project red; fully-set → ready) + **grounding test**: every key in `READINESS_KEYS` exists in `FIELD_HELP` (settingsHelp.ts).
- `frontend/src/setup/wizardSteps.ts` — ordered step defs (keys/step, optional flag, `settingsHelp` copy reused, cheap client-side "test" e.g. embedding-dim match); `isRestartKey(key)` over `RESTART_KEYS`.
- `frontend/src/components/SetupChip.tsx` — fetches `/api/config` once, computes readiness with `hasProject` from the session/project bundle, renders "Setup — N left" (hidden when ready); click → `window.dispatchEvent(new CustomEvent('aimee:open-setup-wizard'))`.
- `frontend/src/components/SetupWizard.tsx` — modal; one screen/step; reads/writes `/api/config` + `/api/config/set`. **Every write in try/catch**: 4xx/5xx/network → `smoothgui` `Toast` + stay on step with input preserved. Tracks `pendingRestart: Set<string>` (adds a saved key that `isRestartKey`); final summary shows "Restart required for: […]" when non-empty; "project" step links to `/projects`; dismissal in `localStorage` (`aimee_setup_dismissed`). Opened by the event; closable anytime (non-blocking).
- `frontend/src/App.tsx` — mount `<SetupChip />` in header + `<SetupWizard />` at root; `useEffect` listener for `aimee:open-setup-wizard`.
- `frontend/src/pages/Settings.tsx` — "Re-run setup" button (clears dismissal + dispatches open event).
- `frontend/src/setup/wizardSteps.test.ts` — every `StepId` has a step def; ordering = documented dependency order; `isRestartKey` matches `RESTART_KEYS`. Component test (mocked `fetch`): (a) success marks step saved; (b) 4xx/5xx → `Toast` + step stays unsaved; (c) `RESTART_KEYS` change populates the summary.

**Acceptance**
- `npm run check` clean; readiness + wizard tests pass; no new test failures.
- Chip shows correct count / hidden when green; wizard writes via `/api/config/set`, Toasts on error, lists restart-pending keys; dismissable + re-openable.

## Per-slice workflow

For each slice: implement → `npm run check` + `npm test` (assert no new failures)
→ `aimee delegate roundtable` review → apply blocking fixes → re-roundtable until
approved → open PR into `feat/setup-wizard` → merge.

## Non-goals / Future

- Server-side `GET /api/setup/state` (replace client inference; enables true
  per-user readiness incl. live DB2/provider pings).
- Per-user **server** persistence of seen/dismissed state (replace localStorage).
- Wizard "Test" buttons that hit real provider/embedder endpoints (MVP does only
  cheap client-side checks like dim-match).
- No change to how config, git, or turns execute; no new writable config keys.

## Risks

- **smoothgui API drift** — we reuse `Panel`/`Badge`/`Toast` already imported in
  the app; no new smoothgui surface.
- **Config key shape** — readiness rules depend on exact key names
  (`provider`, `embedding_command`, `embedding_endpoint`, `db2_url`,
  `kb_api_http_port`); pinned against `settingsHelp.ts`. A rename there breaks a
  readiness rule → caught by the readiness unit test if we encode expected keys.
- **Pre-existing Dashboard test failure** — must remain the *only* failure; CI
  parity checked each slice.
