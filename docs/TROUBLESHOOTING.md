# Troubleshooting

Start at the first broken boundary.

```bash
aimee remote status
aimee status
aimee kb status
aimee audit verify
```

## Client cannot connect

Check URL, DNS, port, server health, certificate fingerprint, bearer rotation, client certificate,
and revocation. A changed pin is a stop, not a warning to bypass.

For local use, check the Unix socket, service manager, server log, and config-directory ownership.

## Reads work; writes fail

Read the structured `403` first. For the first wizard user, confirm the Linux client completed mTLS
enrollment with the exact command shown after Deploy; re-running Deploy as that same user is
idempotent and shows the pairing state. For an additional PAM/OIDC user, check `AIMEE_SERVER_ID`,
`AIMEE_SERVER_TEAM_ID`, the management-JWKS trust bundle, exact subject spelling, grant tier, and
identity-token refusal reason. `aimee.api.remote_writes` cannot fix either denial. Do not widen every
user to test one grant.

## KB is unavailable

Check `AIMEE_KB_API_URL`, service bearer, TLS, KB health, PostgreSQL readiness, extensions, disk, and
connection limits. The server does not autostart a missing KB.

## Search returns nothing

Check scope, ingest status, document commit, code-index freshness, embedder readiness, and filters.
An honest degraded lexical result differs from an empty ingest.

## Delegate fails

Run `aimee agent probe <name>`. Then inspect admission, provider auth, model capability, agent limit,
workspace authority, worktree, sandbox image, package gate, network policy, and the first attempt log.

No network is the container default. Add the narrow egress the task needs; do not disable isolation
globally.

## Workflow parks

Read the named park reason and latest artifact. Common causes are a human gate, no valid panel,
repeated feedback, agent saturation, missing commit, failed verification, merge conflict, lost replay,
forge identity, or spend limit.

Repair the condition and resume the same run so its evidence is preserved.

## Audit or capture gap

Check the event-bus drop counter, sink write errors, free disk, capture classification, and whether the
daemon shut down cleanly. `publish` success means accepted into the producer ring, not yet durable.
Graceful stop drains; a crash may leave the last capture classified open or truncated.

Do not rewrite or prune the WORM store while investigating. Seal a copy first.

## Browser deploy fails

Check the Docker socket mount, daemon access, image pulls, volume ownership, port conflicts, and the
managed server log. If Docker authority is intentionally absent, use the split stack.

## Report a useful failure

Include:

- version and commit;
- deployment shape and platform;
- operation/request ID;
- exact command and structured error;
- first relevant log error;
- health output with secrets removed;
- whether the failure survives restart;
- the smallest reproduction.

Never attach tokens, client keys, vault material, database URLs with passwords, or raw memory that
contains private data.
