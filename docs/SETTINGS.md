# Settings

The browser Settings page and `aimee config` edit the same allowlisted configuration fields.

```bash
aimee config show
aimee config get <key>
aimee config set <key> <value>
```

The exact field list, defaults, bounds, environment overrides, and restart behavior are generated in
[Configuration reference](gen/configuration.md).

## Browser behavior

The Settings page is at `/settings`. It groups fields by dotted prefix and chooses a toggle, number,
or text control from the field type. Save writes one key; Reset restores its descriptor default.

It is an allowlist, not a raw YAML editor. Secrets, credential commands, sensitive endpoints, and
fields without a safe runtime setter do not appear. Put credentials in the vault and deployment
secrets in the environment or secret manager.

## Precedence

From highest to lowest:

1. an explicit environment override;
2. the active profile's `aimee.yaml` value;
3. the descriptor default.

## A change applies on the next turn unless it was bound at startup

Almost every field is read per request, so a change is live as soon as the server picks it up. You do
not need to restart to change a model, an endpoint, a budget, or a feature flag.

Each field carries a reload class in the metadata shipped by
`github.com/RakuenSoftware/aimee-module-config`:

| Class | What it means | Examples |
| --- | --- | --- |
| `RELOAD_HOT` | Read per request. Live immediately. This is the default, so a field that names no class is hot. | provider and model, `openai_endpoint`, `embedder_url`, `synthesis_endpoint`, economizer and memory flags |
| `RELOAD_REAPPLIABLE` | Bound state with a live re-applier. | fields being migrated out of the restart set |
| `RELOAD_RESTART` | Bound once at startup, with no live re-applier. **Needs a restart.** | `db2_url` (the Postgres pool opens at startup), `kb_api_*` (the KB client initialises once), the deploy-topology record |

22 fields are `RELOAD_RESTART` today. The Settings page and the setup wizard both mark them, and the
generated reference lists them per field.

### It picks up an edit you made outside the API

The server polls for a changed config file on its main-loop tick, so a `config set` from the CLI, a
hand edit of `aimee.yaml`, and an autonomous write all take effect the same way. You do not need to
send `SIGHUP`.

The reload validates before it publishes. A file that does not parse or does not validate is
**rejected and the running configuration is kept**, which `$AIMEE_HOME/server.log` says plainly:

```text
2026-08-01T00:07:00Z INFO  config: config file change: reloaded
2026-08-01T00:07:00Z WARN  config: config file change: rejected (kept running config)
```

That file, not `docker logs`. The container's stdout carries the entrypoint and the browser service;
the C server logs to `$AIMEE_HOME/server.log`.

A rejected reload is not a failed start. The server keeps serving the last good configuration, so a
bad edit degrades to "your change did nothing" rather than to an outage. Check the log if a change
seems to have been ignored, before assuming the field is a restart one.

## High-impact settings

### Economizer

```yaml
economizer: safe   # off | safe | aggressive
```

- `off`: provider payload passes through without reduction.
- `safe`: lossless cache alignment, folding, and tool-output condensation with recoverable spills.
- `aggressive`: adds lossy compression or ingress mutation where the provider contract permits it.

`modules.economizer: false` is the hard off switch. See [Economizer](features/economizer.md).

### Remote writes

```yaml
aimee:
  api:
    mtls: optional       # off | optional | required
    remote_writes: off   # off | data | full
```

`remote_writes` is a legacy value retained so old files load. It no longer authorizes user `/v1`
writes; non-off values warn and feed `remote_writes.global_ignored`. The first wizard user's grant
is bound to its enrolled certificate, while additional users use server identity trust and per-user
grants. See [Security](SECURITY.md#remote-access).

The managed server image sets `AIMEE_API_MTLS=optional`, overriding older persisted configs so
enrolled clients present their certificates. The durable presentation ramp promotes the listener
to required after all active certificates have presented. Set the environment variable explicitly
to `off` or `required` only when the deployment calls for it.

### Delegate isolation

Sandbox, source authority, network, package, image, worktree, and concurrency fields change what a
delegate can touch. Treat them as security policy. See [Delegate sandbox](DELEGATE_SANDBOX.md).

### Autonomous workflows

`autonomy.*` fields set scheduler concurrency, turn, resume, wall-clock, stale-run, and delegate
lease limits. Trigger scan and admission policy lives under `trigger.*`. The Go workflow service
reads these values live, so a browser or config API save does not need a restart. An explicit process
environment override still wins.

These limits never authorize a human gate. Node retry and fan-out budgets such as `max_rounds` and
`max_children` remain part of the workflow definition.

### Sub-agent ban

`subagent_ban_enabled` routes sub-work through aimee delegates when usable delegates exist. It
controls server guardrails and client setup for supported tools. The internal delegate tool strip is
independent and stays on.

## Editing YAML

Use `aimee config set` for ordinary scalar changes. It validates the key and updates only that field.
If you edit `aimee.yaml` directly:

- preserve unknown sections owned by newer components;
- quote strings that YAML could parse as booleans or numbers;
- keep the file private;
- restart the owning process when required;
- run `aimee config show` afterward to inspect the resolved value.
