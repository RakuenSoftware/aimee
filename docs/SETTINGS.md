# Settings (web page)

The **Settings** page (aimee web UI, left nav → ⚙️ Settings, route `/settings`) is a typed
editor over the server's runtime configuration. It exposes **every allowlisted config
field** (the same `config.show` / `config.set` surface the `aimee config` CLI uses) as a
grouped, filterable form with per-field **Save** and **Reset**.

It is a thin surface over machinery that already exists: each control maps 1:1 to a
`config_t` field in [`src/config_fields.c`](../src/config_fields.c). Adding a field to that
table makes it appear here automatically; there is no per-field frontend code. Changes
persist to `aimee.yaml` and take effect on the next turn (the server reloads config per
request), unless noted otherwise below.

- **Nav / route:** `/settings` (`frontend/src/App.tsx`), page
  `frontend/src/pages/Settings.tsx`.
- **Data path:** `GET /api/config` → `config.show` (all fields + values); `POST
  /api/config/set {key,value}` → `config.set` (the server validates the key against
  `config_fields` and persists `aimee.yaml`). The webchat proxy (`webchat/config_api.go`)
  is the only network hop; there are no server changes per field.
- **Control inference:** the control type is inferred from the value's JSON type: a
  **boolean → toggle**, a **number → number field**, a **string → text field**. Grouping is
  by the dotted key prefix (e.g. everything under `reduce.` forms one section).
- **Quick panel:** a smaller gear dropdown in the top bar (`SettingsPanel`) surfaces the
  five most-used toggles (autonomous mode, cross-verify, eco mode, reasoning cap, max
  iterations). The full page is the comprehensive home for everything else.

> **Allowlist, not raw config.** The page can only read/write keys in `config_fields`.
> Sensitive values (endpoints, `*_key_cmd`, `db2_url`, and **secrets**) are deliberately
> *not* in that allowlist and never appear here. In particular the CI-webhook secret
> (`AIMEE_CI_WEBHOOK_SECRET`) is an environment secret, not a Settings field.

---

## Option groups added recently

### Context economizer: two-tier switches (`economizer.*`) + levers (`reduce.*`)

The economizer has two **tier switches** that gate the individual `reduce.*` levers, plus the
levers themselves. All are booleans except the two numeric tuning knobs. Defaults in
parentheses.

| Setting | Default | What it does |
| --- | --- | --- |
| **`economizer.enabled`** | **on** | **Master switch.** Off = one kill-switch: every reducer is forced off (measurement keeps running and reports zero, proving the kill). The safe tier stays on under it by the levers' own defaults. |
| `economizer.aggressive` | off | **Opt-in ceiling for the aggressive tier** (live **primary** `/v1` mutation). `reduce.gateway_mutate` activates only with **`economizer.enabled` AND `economizer.aggressive` AND** the lever itself; the aggressive flag alone never turns on a live-traffic mutator. |
| **`reduce.command_filter`** | **on** | **Tool-output condensation**: deterministically condense recognized command output (test-runner failures kept, passes elided; compiler diagnostics kept, progress dropped) with the full output spilled for recovery. See [Tool-output condensation](features/tool-output-condensation.md). |
| `reduce.gateway_mutate` | off | Apply the economizer to the live inbound `/v1` request so the **primary** agent's tokens are reduced too. See [Economizer gateway mutation](features/economizer-gateway-mutation.md). |
| `reduce.compress` | on | Size-based compression of oversized tool-result bodies. |
| `reduce.history_fold` | on | Fold old turn history into a rolling skeleton. |
| `reduce.delegate_seam` | on | Run the economizer at the delegate turn loop. |
| `reduce.measure` | on | Collect the baseline/opportunity token ledger. |
| `reduce.freeze_guard` | on | Pin the fold boundary only when the cache-read savings beat the cache-write cost. |
| `reduce.gateway_session_disable_ttl_ms` | 3600000 | Gateway-mutation circuit-breaker window (ms). Must be > 0. |
| `reduce.freeze_guard_horizon` | 1 | Expected reuse turns for the freeze break-even estimate. |

`reduce.gateway_seam` is intentionally **not** on the page. It is synthesized from
`gateway_mutate` and its persistence is explicit-gated; toggle `gateway_mutate` instead.

### Autonomous-development pipeline: `autonomy.*`

The autonomous-development knobs were historically environment-only (`AIMEE_AUTONOMY_*`).
They are now typed config fields on the Settings page. A `config` value is **bridged to the
matching env var at server startup**, so an explicitly-exported `AIMEE_AUTONOMY_*`
environment variable still **overrides** the config value, and **a Settings change to these
knobs applies on the next server start** (they are deployment-level pipeline tuning, not
per-turn toggles). Values are clamped to sane bounds.

| Setting | Default | Bounds | What it does |
| --- | --- | --- | --- |
| `autonomy.skeptics` | 0 (off) | 0–32 | N adversarial skeptics on the implement gate (0 disables the tier). |
| `autonomy.fanout` | off | — | Engine-level fan-out manager loop (vs a single implement dispatch). |
| `autonomy.unit_retry` | 2 | 0–10 | Per-unit retry-different-delegate cap under fan-out. |
| `autonomy.unit_max` | 16 | 1–256 | Maximum fan-out units (a larger decomposition parks for a human). |
| `autonomy.ci_retry_max` | 2 | 0–20 | Per-work-item red-CI retry cap before parking. |

---

## Turning on tool-output condensation

The most common new toggle. In the web UI:

1. Open **⚙️ Settings** (left nav) and filter for `command_filter` (or scroll to the
   **`reduce`** group).
2. Flip **`reduce.command_filter`** **on** and **Save**.

Or from the CLI / config file:

```yaml
reduce:
  command_filter: true
```

When on, a delegate's recognized command output is condensed before it enters context, with
the full output spilled under `<aimee_home>/tool-spills/` and a recovery pointer in the
condensed result. When off, output is byte-identical to today (it falls through to the
size-based `reduce.compress`). See
[features/tool-output-condensation.md](features/tool-output-condensation.md) for the full
behavior, safety contract, and observability.

## When a change takes effect

- **Immediately (next turn):** the economizer `reduce.*` levers and most other fields. The
  server reloads config per request.
- **On next server start:** the `autonomy.*` knobs: they are bridged to `AIMEE_AUTONOMY_*`
  environment variables at startup so the workflow engine (which reads them across a module
  boundary) sees them; an explicitly-set env var always wins.
