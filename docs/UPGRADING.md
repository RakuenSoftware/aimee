# Upgrading from v0.2.192

There is no route back. 0.3.1 rewrites storage, credentials, and remote identity, and a 0.2 server
will not read what it leaves behind. Your backup is the rollback plan; there is no downgrade
command. Take the backup before step one, not after the first thing goes wrong.

Read [What's new](WHATS_NEW.md) first. This cycle changes deployment, storage, credentials, remote
identity, workflows, and removed commands.

Do not install from the `v0.2.196` or `v0.3.0` tags. Both were promoted in error part-way through
this cycle and are not releases; an installation from either is an untested mid-cycle build missing
the fixes listed under
[If you installed from a mid-cycle tag](WHATS_NEW.md#if-you-installed-from-a-mid-cycle-tag).
Several of those are cases where a fresh install came up healthy and silently did nothing useful.

## Do all of this before you touch the running deployment

1. Stop starting new workflows and wait for active writes to finish.
2. Run `aimee audit checkpoint` and `aimee audit verify`.
3. Back up the server config directory, DB1, workflow store, vault custody, TLS state, and audit
   witness material.
4. Dump DB2 with `pg_dump` or the KB export helper.
5. Export old `work_queue` rows if you need them; the upgrade removes those tables.
6. Record current compose files, image digests, environment, external endpoints, and volume names.

Do not rely on a raw copy of a live SQLite main file. Take a consistent backup with its WAL state.

## The combined image is gone, and the new stack will not adopt your old database

- Replace `aimee-combined` with the managed server or split stack.
- New KB containers start private PostgreSQL when `AIMEE_DB2_URL` is unset.
- The new compose topology does not import an older sibling PostgreSQL volume.
- Keep the old database reachable and set `AIMEE_DB2_URL`, or dump and restore into the embedded
  cluster.
- Never use `docker compose down -v` until the new database has been verified and the backup has
  been restored in a clean test.

## Credentials move into the vault, and clients re-enrol

- Move agent keys and OAuth tokens into the server vault.
- Remove legacy client plaintext only after a successful provider probe.
- Re-enroll each thin client. Verify the server fingerprint before accepting the pin.
- Confirm the first wizard owner completed its certificate-bound enrollment. Give each additional
  user the required remote-write grant. The old global `remote_writes` value authorizes nothing.
- Review mTLS revocation, org catalogs, budgets, rate limits, and egress policy.

## This upgrade is one-way, by design

0.3.1 does not preserve backwards compatibility, and that includes the image itself: once a server
volume has been booted by this release, an older image will not start on it again. It crash-loops
with:

```
[webchat] ERROR: browser UI requires a complete first-boot login sealed in Vault
```

Older images gate startup on `aimee-server --webchat-vault-check`, which wants the vault to hold a
webchat login: the sealed `AIMEE_WEBCHAT_USER`/`AIMEE_WEBCHAT_PASSWORD` pair, or one of the
`legacy_primary` / `legacy_hashes` / `accounts` records. This release holds none of them on purpose:
PAM owns the accounts and the first-boot password is never sealed (see below). There is nothing for
the old check to find, and adding something for it to find would mean persisting a password this
release deliberately does not keep.

Plan accordingly rather than treating the image tag as a rollback: keep the backups from *Before*,
and restore a pre-upgrade volume if you need to go back.

## Browser logins become PAM accounts

The dashboard now authenticates against local PAM rather than credentials held in the server vault.
A host password is not one of aimee's own secrets; keeping it in the vault built a second identity
system beside the PAM and OIDC the KB already implements.

On upgrade, the first-boot pair (`AIMEE_WEBCHAT_USER`/`AIMEE_WEBCHAT_PASSWORD`, or the generated
one) is provisioned as a real system account in the `aimee-webchat` group. Logins are no longer
sealed into the vault, and the shadow verifiers behind them are no longer erased.

**Check this before upgrading.** An appliance that already ran an image which migrated
`webchat/logins` into the vault is in a state this upgrade cannot repair by itself: that migration
sealed the verifiers into the vault's `legacy_hashes` record and then erased them from
`/etc/shadow`. Those accounts therefore have no PAM credential to restore, and only the first-boot
pair is provisioned. Any dashboard account created through the wizard on such an appliance (the
account named in `webchat/bootstrap-replaced`) must be recreated after upgrading.

To tell whether you are affected:

```bash
docker exec <server> sh -c 'ls /var/lib/aimee/webchat/'
```

`bootstrap-replaced` present with no `logins` beside it means the vault migration ran. Sign in with
the first-boot pair after upgrading and recreate the wizard account. If neither credential is
available, provision one directly:

```bash
docker exec <server> sh -c 'useradd --system --no-create-home --shell /usr/sbin/nologin \
  --gid aimee-webchat <name> && passwd <name>'
```

## Restore remote writes

The shared bearer is read-only after this upgrade. The first wizard owner receives an explicit
certificate-bound `full` grant during enrollment. Additional remote users need KB-signed identities
and exact subject grants. `aimee.api.remote_writes=data|full` remains parsed, warns at startup, and
increments `remote_writes.global_ignored`; it does not authorize a user write.

Re-run the managed wizard and use the summary's Deploy action to create the default team, server
workload identity, signed generation-1 JWKS, and root-owned public trust volume.
The one-shot authority image remains separate from the ordinary KB/server
images, and the server mounts only its public output read-only.

For a split or operator-managed authority, configure the server explicitly with
`AIMEE_SERVER_ID`, `AIMEE_SERVER_TEAM_ID`,
`AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE`, and the one-time `AIMEE_KB_CONN`
enrollment string. A complete explicit packet wins over managed enrollment;
partial packets fail closed. Missing team configuration returns
`no_team_configured`; missing or stale signing trust fails closed. The shipped
server Compose file retains the read-only operator mount at
`${AIMEE_SERVER_MANAGEMENT_DIR:-./server-management}:/run/aimee/management:ro`.

For that explicit split path, create the first team through its private
PostgreSQL socket:

```bash
KB_CONTAINER=$(docker ps --filter label=com.docker.compose.project=aimee \
  --filter label=com.docker.compose.service=aimee-kb --format '{{.ID}}')
docker exec \
  -e 'AIMEE_DB2_URL=postgresql:///aimee_shared?host=/var/lib/aimee/run' \
  "$KB_CONTAINER" aimee-kb team create default
```

Enroll the server into the returned team, finalize the matching `kb_server_registry` row, publish
the signed JWKS, and install the exported public bundle as `root:root 0644` under
`server-management/jwks-trust-bundle.json`. The container runs as UID 1000, so a root-owned
`0600` bundle cannot be read. The loader still rejects symlinks, extra hard links, a non-root
owner, and all group/world write bits.

Put all four values in `.env`, recreate the server container, and restart it once after the KB is
ready. Successful enrollment atomically saves the mTLS certificate and private key at
`$AIMEE_HOME/kb-client-identity.json` with owner-only mode `0600`. It does not save the one-time
token, and it validates the stored identity against the connection string's CA pin on every restart.
Startup logs name the missing input when a deployment remains read-only.

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

## What is gone, and what replaces it

- `aimee chat`
- `aimee work` and its routes/tools/tables
- `aimee migrate v2`
- generic `/v1/rpc`
- the combined image
- KB Unix-socket autostart
- per-session credential push

Update scripts to use named `/v1` routes, workflows/jobs, and browser/MCP/ACP/API chat surfaces.

## Verify these before you call the upgrade done

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
