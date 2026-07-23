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

### Context economizer: `economizer.mode`

The economizer has one mode control:

```yaml
economizer:
  mode: safe             # off | safe | aggressive (default: safe)
```

| Mode | What it does |
| --- | --- |
| `off` | Disables economizer transforms. |
| `safe` (default) | Deterministically compacts strict JSON from a fresh local tool result before its first provider dispatch. It does not rewrite history or cache controls. |
| `aggressive` | Adds the existing lossy history and tool-output reducers on supported OpenAI-family routes. Native Anthropic history is not mutated. |

`modules.economizer: false` is an authoritative hard-kill that forces the effective mode to
`off`. Provider cache controls are never changed, and retries reuse the selected exact-length wire
snapshot. See [The aimee Economizer](features/economizer.md).

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

### Sub-agent ban: `subagent_ban_enabled`

| Key | Default | Effect |
|-----|---------|--------|
| `subagent_ban_enabled` | `true` | When on **and** usable delegates are configured, blocks the primary agent's own sub-agent tools (`Task`/`Agent`/`spawn_agent`/`RemoteTrigger`) and redirects to `aimee delegate`, so delegation stays inside aimee's guardrail + memory + KB model. Set `false` to allow provider-native sub-agents. |

What this key gates: the **server guardrail block** (honors `subagent_ban_enabled: false`
as an opt-out) and the **Claude Code harness enforcement** — a `subagent-guard` PreToolUse
hook plus a `permissions.deny [Task, Agent]` backstop that aimee's client setup
auto-installs. The harness gate is evaluated once per client setup / session-start (config
opt-out is read locally; a one-shot `agent.list` probe decides whether usable delegates
exist), so changing this key — or adding/removing delegates — re-materializes the hook and
deny list at the next setup. Independently, the `/v1` **gateway tool-strip** removes
sub-agent tools from what aimee hands its *own* delegate agents; that strip is always-on
and is not affected by this key.

---

## Choosing an economizer mode

The economizer defaults to `safe`. Select any tier with `economizer.mode`:

```yaml
economizer:
  mode: safe
```

```sh
aimee config set economizer.mode safe
```

Use `off` for byte-for-byte pass-through or `aggressive` when lossy context reduction is acceptable.
See [features/economizer.md](features/economizer.md) for the provider and safety boundaries.

## When a change takes effect

- **Immediately (next turn):** `economizer.mode` and most other fields. The
  server reloads config per request.
- **On next server start:** the `autonomy.*` knobs: they are bridged to `AIMEE_AUTONOMY_*`
  environment variables at startup so the workflow engine (which reads them across a module
  boundary) sees them; an explicitly-set env var always wins.
