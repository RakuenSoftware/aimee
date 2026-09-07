# Deployment

## Standard local Server

`compose.yaml` starts a KB-free Server, standardized PostgreSQL, and a local embedder.
`compose.server-managed.yaml` adds Docker-socket access so the browser can manage model containers.
Both use the same application image and PostgreSQL module. Follow the [Quickstart](QUICKSTART.md)
for first-boot credentials and Linux host preparation.

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
pgvector, pgvectorscale, and the module's LUKS2 storage lifecycle. Both roles use it. Server personal
memory uses local tables and vectors; the optional KB holds shared knowledge behind its own API.

## Optional shared KB

Install a KB separately using `compose.kb.yaml` and a distinct Compose project. It has its own
Vault, encrypted PostgreSQL store, embedder, and optional synthesis. Never reuse a Server home or
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
export AIMEE_DEVICE_MAPPER_MAJOR=$(awk '$2 == "device-mapper" {print $1}' /proc/devices)
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

## Encrypted PostgreSQL and volumes

Each composition owns these durable volumes:

| Volume suffix | Contents |
| --- | --- |
| `aimee-server-home` | Instance identity, local Vault, configuration, audit and application artifacts; the shared name also applies to KB |
| `aimee-postgres-encrypted` | LUKS2 container, volume UUID, and nonsecret initialization manifest |
| `aimee-server-workspaces` | Workspace files |
| `aimee-store-tls` | PostgreSQL public certificate for clients |
| `aimee-postgres-control` | Local unlock control socket; no persistent key |

The three model TLS volumes are tmpfs. PostgreSQL data, its private TLS key, and database logs live
inside the encrypted filesystem. The 32-byte LUKS passphrase persists **only in the owning Vault**.
No TPM, external KMS, keyfile, or environment variable supplies it. Protected transient memory and
private pipes carry it during unlock. The Vault must be available before database startup, so the
application waits for the PostgreSQL container to start, not for it to become healthy.

The Linux host must allow loop and device-mapper operations. The PostgreSQL container receives
`SYS_ADMIN`, the control devices, bounded device rules, and a locked-memory limit. It fails closed
if these are unavailable. `AIMEE_POSTGRES_VOLUME_MIB` defaults to 32768 MiB; choose the size before
first boot. Changing it on an existing volume is refused. Online resizing is not implemented.

SQL role passwords are separate PostgreSQL bootstrap credentials. The PostgreSQL container
receives them for role initialization. Before creating the application, `scripts/compose-local.sh`
streams its fixed SQL credentials and optional KB authority into Vault through a disposable
container. Long-lived application metadata contains no SQL or enrollment credentials. The
PostgreSQL Go module retrieves only its runtime and migration DSNs through an attested local
Vault resource. These credentials are distinct from the LUKS passphrase.

## Volumes and backup

Back up the application home and encrypted PostgreSQL volume as a matched instance. Losing the
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
