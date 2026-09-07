# Deployment

## Standard local Server

`compose.yaml` starts a KB-free Server, standardized PostgreSQL, and a local embedder.
`compose.server-managed.yaml` adds Docker-socket access so the browser can manage model containers.
Both use the same application image and PostgreSQL module. Follow the [Quickstart](QUICKSTART.md)
for first-boot credentials. Ordinary storage needs no LUKS host preparation.

```bash
scripts/compose-local.sh -f compose.yaml up -d
# Or, for browser-managed embedding and synthesis:
scripts/compose-local.sh -f compose.server-managed.yaml up -d
```

The application image `ghcr.io/rakuensoftware/aimee` contains both Server and KB compositions.
The Go role module persists an instance UUID and role in the application home before startup.
Later attempts to change role fail. The event-bus host also refuses a second or conflicting role.
This is one application image; PostgreSQL and model services remain separate containers.

The standard PostgreSQL image is `ghcr.io/rakuensoftware/aimee-postgres`, with PostgreSQL 18,
pgvector, pgvectorscale, and optional LUKS2 storage. Both roles use it. Server personal
memory uses local tables and vectors; the optional KB holds shared knowledge behind its own API.

## Optional shared KB

Install a KB separately using `compose.kb.yaml` and a distinct Compose project. It has its own
Vault, PostgreSQL store, embedder, and optional synthesis. Never reuse a Server home or
PostgreSQL volume for a KB. The base Server compose and web wizard do not install this deployment.

A KB requires database role passwords and an authority bearer. Server-to-KB access also requires an
enrolled client identity and a matching service-identity credential. For a connection authorized as
`service:aimee-server`, both independent token values use the prefix `scope:service:aimee-server:`.
Generate separate unpredictable secrets for them. Set `AIMEE_KB_HOST` to the DNS name that clients
will actually reach before first boot; that name is included in the service certificate.

```bash
# Use a separate private environment file containing the KB's three SQL passwords
# and AIMEE_KB_API_BEARER_TOKEN. The service identity is first-boot Vault input.
export AIMEE_KB_SERVICE_IDENTITY_TOKEN="scope:service:aimee-server:$(openssl rand -hex 32)"
scripts/aimee-compose-vault-bootstrap.sh -p team-kb -e kb.env -f compose.kb.yaml kb
unset AIMEE_KB_SERVICE_IDENTITY_TOKEN
scripts/compose-local.sh --env-file kb.env -p team-kb -f compose.kb.yaml up -d
scripts/compose-local.sh --env-file kb.env -p team-kb -f compose.kb.yaml exec -u aimee aimee-kb   aimee-kb enroll --host=kb.example.internal --port=8745 --scope=service:aimee-server
```

The last command returns a sensitive, single-use `aimee://` enrollment string. Enter it in the
Server's **Settings → Knowledge base** together with the two matching service credentials, then
restart Server when Settings requests it. The connection is optional: an unreachable KB does not
move personal data to shared storage or prevent local personal-memory operations.

## Model services

Model services belong to the composition using them. Server creates its own model identities from
its Vault; a KB does the same. Private TLS material is materialized only into private tmpfs volumes.
Embedding and synthesis have separate identities and internal network endpoints.

Local embedding starts by default. Synthesis is optional through the `synthesis` Compose profile or
managed model setup. Both roles can use external model endpoints instead. Remote embedding sends
input text to the configured provider; local embedding keeps it on the deployment host. The native
and Go clients validate the managed model services through fixed mutual-TLS profiles.

## PostgreSQL storage and volumes

Each composition owns these durable volumes:

| Volume suffix | Contents |
| --- | --- |
| `aimee-server-home` | Instance identity, local Vault, configuration, audit and application artifacts; the shared name also applies to KB |
| `aimee-postgres-encrypted` | Ordinary database directory by default; LUKS2 container and manifest when explicitly enabled. Historical volume name retained for safe upgrades |
| `aimee-server-workspaces` | Workspace files |
| `aimee-store-tls` | PostgreSQL public certificate for clients |
| `aimee-postgres-control` (LUKS only) | Local unlock control socket; no persistent key |

The default `plain` storage mode keeps PostgreSQL data and its private TLS key under
`/var/lib/aimee-postgres/plain` on the ordinary volume. It does not provide database encryption
at rest. SQL TLS, scoped database roles, and application Vault credentials remain enabled.
The three model TLS volumes remain private tmpfs volumes.

### Optional LUKS encryption

Add the matching overlay to explicitly enable LUKS: `compose.luks.yaml` for Server or
`compose.kb.luks.yaml` for KB. The overlay sets `AIMEE_POSTGRES_STORAGE=luks`, attaches the
Vault unlock socket, and grants PostgreSQL the required device access. Direct image users may
set that variable themselves, but must also arrange the devices, capabilities, and owning
application's unlock connection. An unknown storage mode is rejected.

The operator is responsible for LUKS support on the **Docker daemon's Linux host**, including
loop and dm-crypt/device-mapper support, control devices, `SYS_ADMIN`, and memory locking.
Docker Desktop users who opt in must provide those features inside its Linux backend;
they are not prerequisites for the default deployment. The launcher does not inspect a
Windows client or mistake it for a remote Docker daemon's host.

For a local Linux Docker host:

```bash
sudo modprobe loop
sudo modprobe dm_mod
sudo modprobe dm_crypt
export AIMEE_DEVICE_MAPPER_MAJOR=$(awk '$2 == "device-mapper" {print $1}' /proc/devices)
scripts/compose-local.sh -f compose.yaml -f compose.luks.yaml up -d
# Separate KB project:
# scripts/compose-local.sh --env-file kb.env -p team-kb -f compose.kb.yaml -f compose.kb.luks.yaml up -d
```

Keep the same overlays for subsequent Compose commands. For browser-managed Server models,
use `-f compose.server-managed.yaml -f compose.luks.yaml`.

In LUKS mode, PostgreSQL data, private TLS keys, and database logs stay inside the encrypted
filesystem. Its 32-byte passphrase persists only in the owning Vault; private pipes and
protected memory carry it during unlock. There is no plaintext fallback if unlock or host
support fails. `AIMEE_POSTGRES_VOLUME_MIB` defaults to 32768 MiB and applies only to LUKS;
changing the size of an existing encrypted store is refused.

Both modes use the same persistent volume identity. Plain mode refuses an existing LUKS
store; LUKS refuses an existing plain store. Enabling or disabling an overlay does not convert
existing data. Use an explicit backup/restore into a separate store when changing modes.
Existing encrypted deployments must retain the LUKS overlay when upgrading.

SQL role passwords are separate PostgreSQL bootstrap credentials. The PostgreSQL container
receives them for role initialization. Before creating the application, `scripts/compose-local.sh`
streams its fixed SQL credentials and optional KB authority into Vault through a disposable
container. Long-lived application metadata contains no SQL or enrollment credentials. The
PostgreSQL Go module retrieves only its runtime and migration DSNs through an attested local
Vault resource. These credentials are distinct from the LUKS passphrase.

## Volumes and backup

Back up the application home and PostgreSQL volume as a matched instance. In LUKS mode, losing the
Vault loses the LUKS key. Losing the application's own Vault root key also makes that Vault
unreadable. Never regenerate either as a recovery shortcut.

Take PostgreSQL-native consistent dumps while the store is open, or stop writes and shut down the
composition before taking matched volume snapshots. Protect plaintext dump exports separately.
Include workspace files, audit ledgers, seals, and off-host witness state. Test restoring into an
isolated project with the same role and original Vault. An encrypted database backup alone is
insufficient to restore service.

`docker compose down` retains volumes; `down --volumes` deletes them. Legacy plaintext PostgreSQL
migration is explicit and preserves the old source for rollback; see [Upgrading](UPGRADING.md).

## Network ports

| Service | Default host port | Exposure |
| --- | ---: | --- |
| Browser | 8443 | HTTPS |
| Server API | 8743 | Enrolled clients |
| Optional KB | 8745 | Mutual TLS |
| KB health | 8741 | Host loopback only |
| PostgreSQL and local models | None | Private Compose networks |

Keep the KB reachable only by intended clients and preserve all three authentication layers:
client certificate, authority bearer, and service identity. Use the standard composition without
Docker-socket access when models are managed outside the browser.

### Git forge credential

`aimee git pr` and the other forge API calls authenticate with one environment-wide
token, held in the **server principal's** Vault as `(git, forge_token)`. It is the
only slot those calls read, and nothing outside the server ever sees it: agents and
sessions can use `aimee git`, but cannot read the token back out.

Supply it at first boot as `AIMEE_FORGE_TOKEN`, which the server seals and then
unsets. If a deployment came up without it, seal it afterwards instead of
re-creating the stack. Run it as the Vault owner, the same way the webchat seal
does, so the entry is not written by root:

```bash
# inside the server container
printf '%s' "$TOKEN" | runuser -u aimee -- aimee-server --forge-vault-seal forge_token
```

No restart is needed: the next forge call reads the new value straight from
Vault.

The secret travels on stdin only, never argv or an environment mapping, so it
cannot leak through a process list or `/proc`. Re-sealing replaces the value, so
this is also how you rotate. Note that `aimee vault set git forge_token ...` is
**not** a substitute: it stores under the calling principal, which the forge
reader never consults.

Symptom of a missing token: every `aimee git pr` action fails with
`no github credential`, while `aimee git push` keeps working because it
authenticates over SSH.

## Upgrade

See [Upgrading](UPGRADING.md). Role changes and PostgreSQL storage migration are distinct from
replacing an application image. Keep the old deployment and its backups until the new one has
passed a restore and application-data check.
