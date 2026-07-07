# Dashboard & Logs (browser UI)

The **Dashboard** and **Logs** tabs of [`aimee-webchat`](../MANUAL.md#23-webchat-and-the-browser-ui)
give an operator an at-a-glance view of what **`aimee-server`** is doing. They
are served by the webchat thin client (default `https://<host>:8443`) which
holds no state and proxies to `aimee-server` over its `/v1` HTTP surface.

## Design principle: server-incurred metrics only

The dashboard shows **things aimee-server incurs**: its delegations, tool
calls, token spend, guardrail verdicts, configured agents, active sessions, and
readiness. It deliberately does **not** monitor `aimee-kb`-internal events
(memory-curation audits, KB decision logs). **`aimee-kb` has its own dashboard**
for those; duplicating them here would blur the boundary between the two
services. The one exception is a state overview the server relies on, the
**Memory** panel (tier counts), which is available but off by default.

A field is "server-incurred" if the server's own agents/workflows produce it,
even when the record is physically stored behind the KB (e.g. delegate token
spend). It is *not* server-incurred if the KB curator produces it.

## Tabs

### Dashboard (`/dashboard`)

A customizable grid of metric panels. The default set fills the viewport in a
clean 3-column layout; you add more from a catalog via **⚙ Customize**.

### Logs (`/logs`)

The server's **tool-action audit ledger**: one row per tool call the agent
makes, with the guardrail **verdict** (`allow` / `block` / `rewrite` /
`approval_required`), the actor (`primary` / `delegate`), the tool, mode, and
reason. Filter by verdict, actor, or tool name. This is `aimee-server`'s own
audit stream (`/var/lib/aimee/audit.log` via `audit_ledger_read`), **not** the
KB's logs.

## Panels

Every panel is derived from data the server incurs. Panels marked *default* are
shown by default; the rest are opt-in via **⚙ Customize**.

| Panel | Default | Source |
|-------|:------:|--------|
| Readiness | ✓ | Synthesized health report (`onboard`): DB/agents/delegations/LSP |
| Agents | ✓ | Configured provider roster (`agents`) |
| Active Sessions | ✓ | Live webchat sessions (injected by webchat) |
| Delegations | ✓ | Recent delegations (`delegations`) with human-formatted latency |
| Metrics | ✓ | Per-role totals/success/latency/tokens (`metrics`) |
| Cost / Tokens | ✓ | Realized token spend rolled up per role (`token_audit`) |
| Guardrail Actions | ✓ | Verdict mix over the tool-action audit (`audit`) |
| Execution Plans | ✓ | Active execution plans (`plans`) |
| Traces | ✓ | Recent tool-call traces (`traces`) |
| Success by Agent | — | Success rate + volume per agent (from `delegations`) |
| Latency by Role | — | p50 / p95 / max per role (from `delegations`) |
| Top Tools | — | Most-invoked tools (from `traces`) |
| Tokens by Agent | — | Token spend per agent (from `token_audit`) |
| Cache Efficiency | — | Cache-read share of input tokens (from `token_audit`) |
| Failures | — | Recent unsuccessful delegations (from `delegations`) |
| Provider Mix | — | Configured agents grouped by provider (from `agents`) |
| Confidence | — | Average delegate confidence per role (from `delegations`) |
| Memory | — | Memory tier/kind counts (`memory_stats.tier_kinds`), a KB state overview |
| LSP Health | — | LSP diagnostics summary (`lsp`) |

Latency is rendered human-readably (`482ms`, `4.7s`, `2m 7s`), not raw
milliseconds; token counts are compact (`4.4M`); cost is USD with a sub-cent
floor (`<$0.01`).

## Customization

Click **⚙ Customize** (top-right of the Dashboard) to toggle any panel on/off
and reorder the enabled ones (↑/↓). **Reset** restores the defaults.

The layout is persisted in the browser's `localStorage` under
`aimee_dash_layout_v1` (an ordered list of panel ids). It is therefore
**per-browser**, not per-user across devices; a server-persisted layout would be
a future enhancement.

## Layout & sizing

The dashboard fills its pane **exactly** (`box-sizing: border-box`; no
negative-margin hacks). The grid is `repeat(3, minmax(0, 1fr))` columns with
`minmax(200px, 1fr)` rows, so:

- it **never** scrolls horizontally;
- with the default panel count it fits the viewport with no scrollbar;
- if you add enough panels to exceed the viewport, only the grid scrolls
  vertically.

## Data architecture

```
browser ──/api/dashboard──> aimee-webchat ──dashboard.all──> aimee-server
        <────── JSON ───────              <──── payload ─────
browser ──/api/audit───────> aimee-webchat ──dashboard.audit─> aimee-server
```

### `dashboard.all` (the `/api/dashboard` payload)

Built by `handle_dashboard_all` (`src/server/server_state.c`). Server-incurred
fields: `delegations`, `metrics`, `traces`, `plans`, `agents`, `token_audit`,
`decisions`, `memory_stats`, `lsp`, plus a synthesized `onboard` readiness
report and a recent `audit` summary (last 300 tool-action rows). The webchat's
`handleDashboardAll` then injects `sessions` (webchat-owned) before returning.

### `dashboard.audit` (the `/api/audit` payload, Logs page)

Backed by the `dashboard.audit` dispatch method → `/v1/dashboard/audit` route →
`audit_ledger_read`, returning up to 5000 most-recent tool-action rows.

### Frontend normalization contract

The server returns some fields in shapes a panel cannot consume directly
(`memory_stats` is an object whose rows live under `tier_kinds`; `onboard` may
be an `{error}` stub; `agents` is the config roster). `toDashData` in
`frontend/src/pages/Dashboard.tsx` normalizes the raw payload into the panel
shapes. That contract is unit-tested against a **captured live payload** in
`frontend/src/pages/Dashboard.test.ts` (run `npm test` in `frontend/`), so a
server shape change that would blank a panel fails a test instead.

## Adding a panel (developer note)

Panels are a registry in `Dashboard.tsx`. To add one:

1. Write a `function MyPanel({ data }: { data: T[] }) { … }` deriving its view
   from an existing `DashData` field (prefer deriving over adding server
   fields).
2. Add an entry to the `PANELS` array: `{ id, title, defaultOn, render }`.
3. If it needs a new server-incurred field, add it to `dashboard.all`
   (`handle_dashboard_all`), thread it through `RawDashboard` + `DashData` +
   `toDashData`, and extend the fixture/tests.

Adding a new `/v1` route (as `dashboard.audit` did) also requires:
`scripts/gen-cli-v1-routes.py` (regenerates `src/cli_v1_routes_d.c`) and an
entry in `api/openapi-server-v1.yaml`; verify with
`scripts/check-cli-v1-routes.py` and `scripts/check-api-conformance-server.py`.
