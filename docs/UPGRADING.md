# Upgrading from v0.2.192

Read [What's new](WHATS_NEW.md) first. This cycle changes deployment, storage, credentials, remote
identity, workflows, and removed commands.

## Before

1. Stop starting new workflows and wait for active writes to finish.
2. Run `aimee audit checkpoint` and `aimee audit verify`.
3. Back up the server config directory, DB1, workflow store, vault custody, TLS state, and audit
   witness material.
4. Dump DB2 with `pg_dump` or the KB export helper.
5. Export old `work_queue` rows if you need them; the upgrade removes those tables.
6. Record current compose files, image digests, environment, external endpoints, and volume names.

Do not rely on a raw copy of a live SQLite main file. Take a consistent backup with its WAL state.

## Deployment changes

- Replace `aimee-combined` with the managed server or split stack.
- New KB containers start private PostgreSQL when `AIMEE_DB2_URL` is unset.
- The new compose topology does not import an older sibling PostgreSQL volume.
- Keep the old database reachable and set `AIMEE_DB2_URL`, or dump and restore into the embedded
  cluster.
- Never use `docker compose down -v` until the new database has been verified and the backup has
  been restored in a clean test.

## Identity and credentials

- Move agent keys and OAuth tokens into the server vault.
- Remove legacy client plaintext only after a successful provider probe.
- Re-enroll each thin client. Verify the server fingerprint before accepting the pin.
- Give each user the required remote-write grant. The old global `remote_writes` value authorizes
  nothing.
- Review mTLS revocation, org catalogs, budgets, rate limits, and egress policy.

## Restore remote writes

The shared bearer is read-only after this upgrade. `aimee.api.remote_writes=data|full` remains
parsed, warns at startup, and increments `remote_writes.global_ignored`; it does not authorize a
user write.

Configure the server with `AIMEE_SERVER_ID`, `AIMEE_SERVER_TEAM_ID`, and
`AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE`. Missing team configuration returns
`no_team_configured`; missing or stale signing trust fails closed. The managed compose file does not
set these for you.

Grants are keyed by server, team, and exact authenticated subject:

| Subject | Form |
| --- | --- |
| PAM user | `alice` |
| OIDC user | `oidc:<percent-encoded-issuer>:<sub>` |
| mTLS identity | `cert:<issuer>:<serial>` |
| local single-org operator | `owner` |

Grant through the local Unix socket. These routes are never exposed to a remote bearer:

```bash
aimee kb grant set --server <server-id> --team <team-id> --subject <subject> --tier data
aimee kb grant show --server <server-id> --team <team-id> --subject <subject>
aimee kb grant list --server <server-id> --team <team-id> --include-revoked
aimee kb grant revoke --server <server-id> --team <team-id> --subject <subject>
```

`data` permits memory, document, and index writes. `full` also permits agent, delegate, runner, and
workspace control. The first grant uses the local `owner` operator context with team `0`; the
command's `--team` still names the target team. Interactive users obtain their write identity by
the KB's configured PAM or OIDC login. Give unattended callers separate service subjects.

Common refusal reasons are `absent`, `invalid`, `unknown_kid`, `wrong_team`,
`no_team_configured`, `replay`, and `replay_unavailable`. Use the structured `403`, request ID, and
server log. A grant for the wrong spelling is a grant for nobody.

## Removed surfaces

- `aimee chat`
- `aimee work` and its routes/tools/tables
- `aimee migrate v2`
- generic `/v1/rpc`
- the combined image
- KB Unix-socket autostart
- per-session credential push

Update scripts to use named `/v1` routes, workflows/jobs, and browser/MCP/ACP/API chat surfaces.

## After

```bash
aimee remote status
aimee status
aimee kb status
aimee audit verify
aimee memory store upgrade-smoke "write ok"
aimee memory search "write ok"
```

Then:

- ingest one small source tree and check caller lookup;
- run one delegate probe and read its audit row;
- validate a workflow and inspect the Go workflow service;
- verify capture files are created and the event-bus drop counter is zero;
- restart the stack once and confirm active state recovers cleanly;
- restore the backup into a disposable deployment.

Keep the old volumes read-only until these checks pass.
