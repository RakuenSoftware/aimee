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
  by the dotted key prefix (e.g. everything under `autonomy.` forms one section).
- **Quick panel:** a smaller gear dropdown in the top bar (`SettingsPanel`) surfaces the
  five most-used toggles (autonomous mode, cross-verify, eco mode, reasoning cap, max
  iterations). The full page is the comprehensive home for everything else.

> **Allowlist, not raw config.** The page can only read/write keys in `config_fields`.
> Sensitive values (endpoints, `*_key_cmd`, `db2_url`, and **secrets**) are deliberately
> *not* in that allowlist and never appear here. In particular the CI-webhook secret
> (`AIMEE_CI_WEBHOOK_SECRET`) is an environment secret, not a Settings field.

---

## Option groups added recently

### Context economizer: the single `economizer` tier

The economizer is controlled by **one** provider-aware setting, `economizer`, with three
values. It replaces the old `economizer.enabled`/`economizer.aggressive` switches and the
per-lever `reduce.*` booleans — the tier now decides each reducer's behavior internally.

| `economizer` | What it does |
| --- | --- |
| `off` | Verbatim passthrough: no prompt caching, no reduction. |
| `safe` **(default)** | Lossless. Anthropic prompt caching + deterministic freeze-on-first-send tool-output condensation; OpenAI recall-restorable history fold. |
| `aggressive` | Everything in `safe`, plus lossy body compression and live OpenAI `/v1` gateway mutation (primary-agent token reduction, behind a per-session circuit breaker). |

```yaml
economizer: safe   # off | safe | aggressive
```

`modules.economizer: false` is an authoritative hard-kill that forces the tier to `off`
regardless of its value. A deprecated `economizer: {enabled, aggressive}` object form is still
accepted and mapped onto the tier for back-compat.

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

## Tool-output condensation

Deterministic command-aware tool-output condensation is part of the **`safe`** tier — it is on
whenever `economizer` is `safe` or `aggressive` (i.e. not `off`). There is no separate toggle;
set `economizer: off` to disable all reduction including condensation.

A delegate's recognized command output is condensed before it enters context, with the full
output spilled under `<aimee_home>/tool-spills/` and a recovery pointer in the condensed
result. See [features/tool-output-condensation.md](features/tool-output-condensation.md) for
the full behavior, safety contract, and observability.

## When a change takes effect

- **Immediately (next turn):** the `economizer` tier and most other fields. The server
  reloads config per request.
- **On next server start:** the `autonomy.*` knobs: they are bridged to `AIMEE_AUTONOMY_*`
  environment variables at startup so the workflow engine (which reads them across a module
  boundary) sees them; an explicitly-set env var always wins.
