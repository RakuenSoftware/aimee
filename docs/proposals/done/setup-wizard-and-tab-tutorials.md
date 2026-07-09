# Proposal: aimee-server web GUI — first-run setup wizard + per-tab tutorials

- **State:** done — shipped to `testing` across two slices. Slice 1 (per-tab tutorial
  overlays + "?" re-opener + "Replay tutorials") and Slice 2 (client-side readiness +
  header "Setup — N left" chip + wizard modal writing through the existing
  `POST /api/config/set`, restart-key aware) merged via **#1147 / #1149 / #1150**. A
  follow-up (this close-out) restores AC4 for the two tabs added after the wizard merged
  — **Roles** (#1146) and **Pipeline** (#1167) — which had no tutorial: `NAV_ITEMS` moved
  to `src/nav.ts` and `tutorials.test.ts` now derives its route list from it, so a future
  untutored tab fails the coverage test rather than shipping silently. **Two documented
  MVP deviations** (roundtable-approved in the plan): readiness is computed client-side
  from `GET /api/config` rather than a server `GET /api/setup/state`, and seen/dismissed
  state lives in `localStorage` rather than per-user server state — both listed as Future
  in the plan. All acceptance criteria are exercised by the frontend test suite
  (`readiness.test.ts`, `wizardSteps.test.ts`, `tutorials.test.ts`); `tsc -b` clean.
- **Charter role(s):** none (product/UX surface; consumes existing config + readiness contracts, adds no intelligence pass)
- **Surfaces:** `frontend/` (the aimee-server web GUI), `/api/config`, `/api/config/set`, `/api/git/*`, Dashboard readiness

## Thesis

A fresh aimee-server install drops the operator straight onto `/chat` with no
guidance. Everything needed to make the instance *actually work* — pick a
primary provider, wire an embedding backend, point at DB2, connect a git
project — is reachable but never surfaced. It lives in the Settings tab as a
flat, searchable list of ~80 config keys (`frontend/src/pages/Settings.tsx`,
`settingsHelp.ts`), with no notion of "what do I set **first**, and in what
order, to get from zero to a working turn." The Dashboard already computes a
**Readiness** signal and **LSP Health** — but only *after* you've found the
Dashboard tab and only as a display, not a fix-it flow.

Two gaps, one theme (nobody tells the operator what to do):

1. **No guided setup.** There is no wizard that detects what's unconfigured and
   walks the operator through the minimum viable environment. The knowledge to
   do it exists (every field has a plain-English line in `settingsHelp.ts`; the
   restart-sensitive keys are enumerated in `RESTART_KEYS`) — it's just never
   assembled into a path.
2. **No per-tab orientation.** The eleven tools in the left nav
   (`NAV_ITEMS` in `frontend/src/App.tsx`) are discoverable by clicking, but
   each lands you in a working surface with no "what is this / how do I use it"
   affordance. A new operator learns Workflow Actions vs Edit Workflows vs
   Agents by trial and error.

## Goal

Two additions to the existing GUI, sharing one content store:

- **A) A first-run setup wizard** that runs on an under-configured instance,
  detects readiness, and walks the operator through the minimum path to a
  working turn — writing every change through the *existing* `/api/config/set`
  allowlist (no new mutation surface, no new secrets path).
- **B) A per-tab tutorial layer** — a dismissible "how this tab works" overlay
  on each of the eleven tabs, reusable as a "?" affordance after first run.

Both are **additive and non-blocking**: an operator who knows the product can
skip the wizard and never see a tutorial again. Nothing changes about how
config, git, or turns actually execute.

## §0 What already exists (do not rebuild)

- **Settings tab** — `frontend/src/pages/Settings.tsx` reads `GET /api/config`
  (`config.show`), writes `POST /api/config/set`, infers the control from the
  value's JSON type, and groups keys into sections via `category()`.
- **Plain-English help** — `frontend/src/pages/settingsHelp.ts`: `FIELD_HELP`
  (one line per key, states the default), `SECTION_HELP` (one line per section),
  and `RESTART_KEYS` (`db2_url`, `kb_api_http_port`, `kb_api_bearer_token` — the
  only keys that need a restart rather than applying next turn). **The wizard's
  copy comes from here; it is not rewritten.**
- **Readiness + LSP Health** — the Dashboard (`frontend/src/pages/Dashboard.tsx`)
  already renders "Readiness" and "LSP Health" cards. The wizard should consume
  the *same* server signal, not invent a second definition of "ready."
- **Git project connect** — `frontend/src/pages/Projects.tsx` already does clone,
  per-host credential storage (sealed vault), and GitHub device-flow OAuth
  (`/api/git/oauth/github/*`). The wizard's "connect a project" step **launches
  this existing flow**, it does not reimplement it.
- **Sessions** — a session bundles a chat + its project (`SessionContext.tsx`);
  every tool operates on the active session's project.
- **No wizard, no onboarding, no per-tab help.** Confirmed: `grep -rilE
  'wizard|onboard|first.?run|getting.?started'` over `frontend/src` returns only
  Dashboard/settings incidental matches. This is greenfield UX over an existing
  config plane.

## §1 The setup wizard

### §1.1 Trigger

On load, `App` already gates on `/api/chat/session`. Add a lightweight
`GET /api/setup/state` that returns a **readiness summary** (reusing the
Dashboard readiness computation server-side, not a new heuristic):

```jsonc
{
  "ready": false,
  "steps": {
    "provider":  { "ok": true,  "detail": "claude" },
    "embedding": { "ok": false, "detail": "built-in hash fallback (test-only)" },
    "db2":       { "ok": true,  "detail": "connected" },
    "kb_api":    { "ok": false, "detail": "disabled (port 0)" },
    "project":   { "ok": false, "detail": "no project connected" }
  },
  "dismissed": false
}
```

The wizard auto-opens when `ready === false && dismissed === false`. It is a
modal *over* the app, never a separate route — the operator can close it and the
app is fully usable. A persistent **"Setup — N steps left"** chip in the header
re-opens it. Dismissal is per-user server state so it survives a new browser.

### §1.2 Steps (minimum viable environment)

Each step is one screen: the `settingsHelp.ts` line as the explanation, the live
value, an input, and a **"Test"** button that validates before advancing. Order
is the real dependency order for a working turn:

1. **Primary provider** — `provider`, then the provider-specific keys it reveals
   (`claude_model`; or `openai_endpoint` / `openai_model` / `openai_key_cmd`).
   Test = a trivial provider ping. *Green already when Claude CLI is logged in.*
2. **Embedding backend** — `embedding_command` **or** `embedding_endpoint`, plus
   `embedding_model` / `embedding_dim`. The wizard must **loudly** flag the
   built-in 384-dim hash as test-only (that copy already exists in
   `FIELD_HELP.embedding_command`) — this is the #1 silent-degradation trap.
   Test = embed a fixed string, assert the returned vector width equals
   `embedding_dim`.
3. **Shared store (DB2)** — `db2_url`. Flagged `RESTART` (in `RESTART_KEYS`).
   Test = connection check. Wizard shows the "applies after restart" banner and
   offers a restart-needed reminder rather than pretending it took effect.
4. **Knowledge-base API (optional)** — `kb_api_http_port` + `kb_api_bearer_token`
   (both `RESTART`). Presented as **optional/skippable** with a one-line "what
   you lose if you skip" (no REST access to the KB).
5. **Connect a project** — hands off to the existing `Projects` connect flow
   (clone or GitHub OAuth). On return, the wizard confirms the session now has a
   project bound.

A final **summary screen** re-renders `/api/setup/state` so the operator sees
green across the board (or exactly what remains + why), plus any pending-restart
callout. "Finish" sets `dismissed`.

### §1.3 Constraints (what the wizard is *not* allowed to do)

- **No new write path.** Every set goes through `POST /api/config/set` — the same
  allowlist Settings uses. The wizard cannot set a key Settings can't.
- **No new secret handling.** `openai_key_cmd` stays a *command that prints the
  key* (keeps the key out of config); git credentials stay in the sealed vault
  via the existing `/api/git/*` path. The wizard never accepts a raw API key into
  a config value.
- **Honest about restarts.** Keys in `RESTART_KEYS` are labelled and the wizard
  never reports them as live until a restart happens.

## §2 Per-tab tutorials

### §2.1 Mechanism

A small `<TabTutorial tabId=… />` affordance rendered by each page (or once in
`App` keyed on route). First visit to a tab auto-shows a **dismissible overlay**;
after dismissal it collapses to a **"?"** button in the tab's top-right that
re-opens it. Per-tab "seen" state lives in the same per-user store as wizard
dismissal, so it's stable across browsers and a "Replay all tutorials" toggle in
Settings resets it. Content is data (§3), not hard-coded JSX, so it's editable
without a component change.

### §2.2 Content — one tutorial per real tab

Grounded in what each page actually does today (`frontend/src/pages/*`,
`NAV_ITEMS` in `App.tsx`):

- **💬 Chat** — the session's conversation with aimee; attach files, use
  slash-commands, channels, and turn a reply into a proposal. "This is where a
  turn runs; the active session's project is what it acts on."
- **📊 Dashboard** — live health: Readiness, LSP Health, per-agent success/tokens,
  latency by role, provider mix, cache efficiency, guardrail actions, traces.
  "Start here when something feels wrong." (Cards are reorderable.)
- **📜 Logs** — the audit trail; every row expands to a full-field detail modal.
  "What did aimee do, when, and under whose identity."
- **🔀 Edit Workflows** — *define* multi-step workflows (per-step task, persona,
  delegate, role). The design surface.
- **📝 Workflow Actions** — the *runtime* queue: pending items to **approve /
  reject**. "This is the human-in-the-loop gate; Edit Workflows is where the
  steps came from." (Explicitly disambiguates the two adjacent tabs — the #1
  confusion.)
- **🤝 Agents** — delegates and their run history/stats; edit a delegate's
  persona + role. "Roles are the routing key matched between personas and agents."
- **🎭 Personas** — the shared ROLE vocabulary + PERSONA definitions (identity +
  allowed roles). "Edit *who* aimee can be and *what* each role means; Agents
  binds them to work."
- **📁 Projects** — connect a git repo, store per-host credentials (sealed vault),
  run read + remote git ops. "Connect the code aimee works on; creds never reach
  the browser."
- **🕸️ Graph** — read-only code-projection explorer: rank hubs, expand
  callers/callees/neighbours with provenance, spot "surprising links." "Understand
  the codebase's shape; off the agent's hot path."
- **🖥️ Editor** — in-app VS Code bound to the session's isolated worktree — the
  same tree the agent edits. "See and hand-edit exactly what the agent sees."
- **⚙️ Settings** — every typed config key with plain-English help; boolean→toggle,
  number→field, string→text; restart-sensitive keys badged. "The full control
  plane the wizard walked you through a slice of."

Each tutorial is **3–5 lines max** + one "where this connects" pointer to a
sibling tab. Long-form docs stay in `docs/`; the overlay links out, it doesn't
duplicate.

## §3 Where the content lives

One content module, `frontend/src/help/tutorials.ts`, keyed by `tabId` and by
wizard `stepId`:

```ts
export const TAB_TUTORIALS: Record<string, { title: string; body: string[]; seeAlso?: string }> = { … }
export const WIZARD_STEPS: Array<{ id: string; keys: string[]; test?: TestSpec; optional?: boolean }> = { … }
```

- Wizard step **copy** reuses `FIELD_HELP` / `SECTION_HELP` verbatim — single
  source of truth, so a config-help edit updates both surfaces (the existing
  sync rule between `settingsHelp.ts` and `config.h` comments extends to cover
  it).
- Tab copy is authored fresh (the pages have no help strings today) but kept to
  the length above and reviewed against the page's own header comment so it can't
  drift silently.

## §4 Rollout

Ships in independent slices; each is useful alone:

1. `GET /api/setup/state` + the header **Readiness chip** (no wizard yet) — makes
   readiness visible without the Dashboard.
2. Per-tab tutorial overlays (§2) — pure frontend, no new endpoint, lowest risk.
3. The wizard modal (§1) over the existing `/api/config/set` — the largest slice.
4. "Replay tutorials" / "Re-run setup" controls in Settings.

## Non-goals

- **Not a new config surface.** Zero new writable keys; the wizard is a *view*
  over `/api/config/set`. If Settings can't set it, the wizard can't either.
- **Not provisioning.** The wizard configures an aimee-server that is already
  running; it does not install sidecars (OCR/TSR/rerank), stand up Postgres, or
  deploy containers. Where a key needs a sidecar, it says so and links the doc.
- **Not a replacement for Settings.** Settings stays the exhaustive plane; the
  wizard is the curated first-mile through it.
- **Not blocking.** A configured instance never forces the wizard; a returning
  operator never re-sees a dismissed tutorial.

## Acceptance criteria (validation-pending — this is a proposal)

- On an instance with the built-in hash embedder and no project,
  `GET /api/setup/state` returns `ready:false` with `embedding` and `project` red;
  the wizard auto-opens.
- Completing every step (test buttons green) flips `/api/setup/state` to
  `ready:true`; a `RESTART_KEYS` change is reported as pending-restart, never as
  live.
- Every wizard write is observable as a `POST /api/config/set` — no other mutation
  endpoint is introduced.
- Each of the eleven `NAV_ITEMS` tabs shows its tutorial on first visit and a "?"
  re-opener after; "Replay tutorials" resets the seen-state for all eleven.

> These criteria are the exit test for implementation, not claims about current
> behaviour — nothing here is built yet.
