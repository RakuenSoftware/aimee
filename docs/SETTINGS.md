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
    mtls: optional       # off | optional | required
    remote_writes: off   # off | data | full
```

Without a configured per-user authority, this tier applies only to clients holding an mTLS
certificate enrolled by the server; a bearer alone remains read-only. Configuring server identity
trust activates strict per-user grants, makes this value a ceiling rather than an authorizer, and
feeds missing-authority refusals to `remote_writes.global_ignored`. See
[Security](SECURITY.md#remote-access).

The managed server image sets `AIMEE_API_MTLS=optional`, overriding older persisted configs so
enrolled clients present their certificates. The durable presentation ramp promotes the listener
to required after all active certificates have presented. Set the environment variable explicitly
to `off` or `required` only when the deployment calls for it.

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
