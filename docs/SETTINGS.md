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

Most runtime fields reload on the next request. Process topology, listeners, storage, custody, and
some workflow controls need a restart. The generated reference marks those fields.

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
    remote_writes: off   # off | data | full
```

This legacy value is still parsed so old files load. It no longer authorizes user `/v1` writes;
non-off values warn and feed `remote_writes.global_ignored`. Configure server identity trust and
per-user grants instead. See [Security](SECURITY.md#remote-access).

### Delegate isolation

Sandbox, source authority, network, package, image, worktree, and concurrency fields change what a
delegate can touch. Treat them as security policy. See [Delegate sandbox](DELEGATE_SANDBOX.md).

### Autonomous workflows

`autonomy.*` fields set retry, fan-out, agent, and gate budgets. They never authorize a human gate.
Some are copied into the Go workflow process at startup and therefore need a restart.

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
