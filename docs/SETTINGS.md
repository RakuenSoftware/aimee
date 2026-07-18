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

### Context economizer: the `economizer` tier

The economizer is a **single tiered control** — one string, not a set of levers:

```yaml
economizer: safe        # off | safe | aggressive   (default: safe)
```

| Tier | What it does |
| --- | --- |
| `off` | Verbatim passthrough — no prompt-cache breakpoint, no reduction. |
| `safe` (default) | Anthropic prompt caching **+** deterministic, freeze-on-first-send tool-output condensation (the full output stays recall-restorable); recall-restorable history fold on OpenAI. Lossless — nothing the model can see is dropped. |
| `aggressive` | Everything in `safe` **+** lossy tool-body compression and live inbound `/v1` request mutation. Mutation/compression apply to **OpenAI-family egress only** — Anthropic context is never mutated at any tier. |

`modules.economizer: false` is an authoritative hard-kill that forces the tier to `off`
regardless of the `economizer` value. The per-tier reduction behavior (fold, condensation,
compression, gateway mutation) is an **internal preset** selected by the tier — there are no
per-lever config knobs. See [The aimee Economizer](features/economizer.md).

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

## Choosing an economizer tier

The economizer is on by default at the `safe` tier (lossless). To change it, in the web UI
open **⚙️ Settings** and set **`economizer`**, or from the CLI / config file:

```yaml
economizer: aggressive   # or: off
```

```sh
aimee config set economizer aggressive
```

Tool-output condensation (recognized command output condensed before it enters context, with
the full output spilled under `<aimee_home>/tool-spills/` and a recovery pointer) is part of
the `safe` tier and on by default. See
[features/tool-output-condensation.md](features/tool-output-condensation.md) for the
condensation safety contract and observability, and
[features/economizer.md](features/economizer.md) for the full tier model.

## When a change takes effect

- **Immediately (next turn):** the `economizer` tier and most other fields. The
  server reloads config per request.
- **On next server start:** the `autonomy.*` knobs: they are bridged to `AIMEE_AUTONOMY_*`
  environment variables at startup so the workflow engine (which reads them across a module
  boundary) sees them; an explicitly-set env var always wins.
