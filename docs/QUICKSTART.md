# Quickstart

A single user needs Server, its PostgreSQL store, and an embedding service. A shared KB is optional.
Server and KB use the same application image, with the role established permanently on first boot.
Each deployment has its own Vault and standardized encrypted PostgreSQL container.

## 1. Start a local server

Use a Linux Docker host with Compose v2.24.4 or newer, loop devices, and device-mapper. The PostgreSQL
container uses LUKS2; startup fails if encrypted storage cannot be mounted. The thin client can run
on Linux, macOS, or Windows. A Linux VM can host the containers for a non-Linux client.

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
sudo modprobe loop
sudo modprobe dm_mod
umask 077
# Run credential generation once on a new installation; preserve an existing .env.
if [ ! -e .env ]; then
  cp .env.example .env
  for v in ADMIN MIGRATOR RUNTIME; do
    echo "AIMEE_STORE_${v}_PASSWORD=$(openssl rand -hex 32)" >> .env
  done
fi
scripts/compose-local.sh -f compose.yaml up -d
scripts/compose-local.sh -f compose.yaml logs aimee-server
```

The helper discovers the host's device-mapper major; it never creates or supplies a LUKS key.
The application Vault creates that key and sends it over a private local bootstrap channel.
Neither the container environment nor a plaintext keyfile holds the LUKS key. PostgreSQL cannot
start without the owning Vault. Keep the application home and encrypted database together in backups.

The three database passwords configure separate administrator, migrator, and runtime roles. Keep
`.env` private and preserve its values: changing them later does not change existing database roles.
These SQL credentials are distinct from the LUKS key, which is stored only in Vault.

This starts one application container, one PostgreSQL container, and one local embedder. It does
not start a KB or synthesis model. `compose.server.yaml`, `compose.server-standalone.yaml`, and
`deploy/compose/aimee.yaml` are aliases of this standard deployment.

### Browser-managed models

Use `compose.server-managed.yaml` in place of `compose.yaml` in the commands above if the browser
should install or change local model containers. This grants the application access to the host
Docker socket. The wizard manages only embedding and optional synthesis; it never installs a KB.

The first-boot log prints a generated dashboard username and password once. Open
<https://localhost:8443>, sign in, and replace the temporary account in the wizard. To choose your
initial login instead, seal it before the first `up`:

```bash
export AIMEE_DEVICE_MAPPER_MAJOR=$(awk '$2 == "device-mapper" {print $1}' /proc/devices)
export AIMEE_WEBCHAT_USER=operator
read -rsp 'Initial dashboard password: ' AIMEE_WEBCHAT_PASSWORD && echo
export AIMEE_WEBCHAT_PASSWORD
scripts/aimee-compose-vault-bootstrap.sh -f compose.server-managed.yaml server
unset AIMEE_WEBCHAT_PASSWORD
scripts/compose-local.sh -f compose.server-managed.yaml up -d
```

The helper streams credentials through stdin into Vault. The browser uses the resulting local PAM
account. Its private password verifier survives application container replacement; a generated
plaintext password cannot be recovered after the first-boot log is gone.

### Complete local setup

The wizard covers the account, primary provider, **Local memory models**, Git commit identity,
optional Git-host connection, and workspaces. Completed steps may be hidden when you reopen it.
There is no KB-installation or database-selection step.

The default embedder is local Bekko A25M with 384 dimensions. A configured external embedding endpoint
is also supported. Personal memories and their vectors stay in the Server's PostgreSQL store;
connecting a KB does not move them. If you choose a remote embedding provider, that provider receives
the text sent for embedding, so use the local model when that text must stay on your machine.

Synthesis may be off, local, or external. It is not required for personal memory storage and recall.
In managed deployments, save the model choices and apply them from the summary. For a manually
managed deployment, start the optional local synthesis service with:

```bash
scripts/compose-local.sh -f compose.yaml --profile synthesis up -d
```

Register its endpoint `https://aimee-llm:8761` in Providers and discover its served model ID. Model
services use identities issued by their owning Server or KB and require mutual TLS. They do not
need a KB to run. Changing embedding models changes the vector space: personal memory vectors are
recomputed, while an existing shared KB corpus requires the guarded
[embedder migration](runbooks/change-embedder.md).

For external embedding without a local embedder, set `EMBEDDER_URL`, `EMBEDDER_MODEL`, and
`EMBEDDER_DIMS`, then start only `aimee-server` (its PostgreSQL dependency starts automatically).

### Optional shared knowledge

Open **Settings → Knowledge base** to connect an existing KB by setup code or connection details.
For an enrolled `aimee://` connection, supply the connection string and the KB administrator's
bearer and service-identity credentials. Secret fields show only whether a value is stored; leaving
a field blank preserves it. A changed connection string requires an application restart.

A KB is a separate shared deployment, described in [Deployment](DEPLOYMENT.md). It uses the same
application and PostgreSQL images as Server and its own Vault and model identities. Never point a
KB container at an existing Server home or try to change the identity file.

### Choosing an image channel

Set `AIMEE_IMAGE_TAG=testing` to select the testing channel, or pin a release tag. For managed
installs, the application forwards that channel to model deployments. Explicit
`AIMEE_APPLICATION_IMAGE`, `AIMEE_POSTGRES_IMAGE`, `AIMEE_EMBEDDER_IMAGE`, and `AIMEE_LLM_IMAGE`
overrides take precedence. Use a client from the same release channel.

## 2. Install the client

The client and server must use the same release channel. If step 1 used the default `:latest`
images, download the client from the latest GitHub release as shown below.

If step 1 used `AIMEE_IMAGE_TAG=testing`, the latest release client may not know routes added by the
testing server. Build the Linux client from the same checkout instead, then continue at step 3:

```bash
make -C src -j4 ../aimee
install -Dm755 aimee ~/.local/bin/aimee
export PATH="$PATH:$HOME/.local/bin"
aimee version
```

The source build requires the development packages listed by `./install-deps.sh`. Do not pair an
older release client with `:testing` images and treat missing-route or stale-version output as a
server failure.

### Linux

```bash
mkdir -p ~/.local/bin
curl -fL https://github.com/RakuenSoftware/aimee/releases/latest/download/aimee-linux-x86_64 \
  -o ~/.local/bin/aimee
chmod 755 ~/.local/bin/aimee
export PATH="$PATH:$HOME/.local/bin"
aimee version
```

Use `aimee-linux-arm64` instead on ARM64.

### macOS

```bash
mkdir -p ~/.local/bin
curl -fL https://github.com/RakuenSoftware/aimee/releases/latest/download/aimee-macos-universal \
  -o ~/.local/bin/aimee
chmod 755 ~/.local/bin/aimee
xattr -d com.apple.quarantine ~/.local/bin/aimee 2>/dev/null || true
export PATH="$PATH:$HOME/.local/bin"
aimee version
```

### Windows

In PowerShell, download the released client into a directory on your user `PATH`:

```powershell
$bin = "$env:LOCALAPPDATA\aimee\bin"
New-Item -ItemType Directory -Force $bin | Out-Null
Invoke-WebRequest https://github.com/RakuenSoftware/aimee/releases/latest/download/aimee-windows-x86_64.exe -OutFile "$bin\aimee.exe"
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
$paths = @($userPath -split ';' | Where-Object { $_ })
if ($bin -notin $paths) {
  [Environment]::SetEnvironmentVariable("Path", (($paths + $bin) -join ';'), "User")
}
$env:Path = "$env:Path;$bin"
aimee version
```

The client is DB-free. It does not need PostgreSQL, SQLite, the KB, or model libraries.

## 3. Enroll the client

Copy the exact command shown by the summary after **Deploy**. It looks like this:

```bash
aimee remote set https://server.example:8743 <wizard-bearer>
aimee remote status
```

`remote set`:

1. connects to the private server certificate;
2. prints and stores its fingerprint;
3. on Linux, generates the client private key locally and submits only a signed CSR;
4. enrolls an individual mTLS certificate and binds it to the wizard user;
5. activates that certificate's explicit `full` grant;
6. writes private state to `~/.config/aimee/remote.conf`.

Verify the fingerprint against the server through a second channel. Do not accept an unexpected
change. The wizard bearer alone is read-only: write authority requires the matching enrolled
certificate. Re-running Deploy as the same user is idempotent; a different user cannot replace the
first owner.

Automatic first-user certificate enrollment is currently Linux-only. macOS and Windows clients fail
closed instead of silently receiving bearer-only write access; use a Linux client for this quickstart.

Self-signed local servers need no insecure-mode flag: `remote set` pins the leaf and reports its
fingerprint for verification.

The complete write-capable quickstart below currently requires the Linux client. macOS uses Secure
Transport and Windows uses Schannel, but automatic CSR enrollment is not yet implemented on those
two clients. They can connect while mTLS is optional, but remain read-only and will not connect once
the server's enrolled-client roster promotes mTLS to required. Do not mistake a copied bearer for a
client identity.

## 4. Verify the stack

```bash
aimee status       # server, DB1, and KB health
aimee kb status    # detailed store, vector, ingest, and curator state
aimee audit verify
```

### Verify first-user write access

The Linux client enrolled in step 3 already has the first wizard user's certificate-bound `full`
grant. No authority setup or server-side grant command is part of the single-user quickstart. Prove
that the setup is durable before continuing:

```bash
aimee memory store quickstart "Enrollment works"
aimee memory search "Enrollment works"
```

The bearer alone remains read-only, and changing the retired `aimee.api.remote_writes` setting does
not grant access.

### Additional users and authority-managed grants

Skip this section for the first wizard user. The managed wizard now creates the
default team, server workload identity, signed generation-1 JWKS, and public
trust pin automatically. Larger or split installations can instead add
PAM/OIDC users with short-lived, KB-signed identities and grants keyed by
`(server_id, team_id, subject)`. An operator-managed version of that path
requires:

- `AIMEE_SERVER_ID`;
- `AIMEE_SERVER_TEAM_ID`;
- `AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE`, pointing to the root-owned trust bundle for the KB signing
  keys;
- `AIMEE_KB_CONN`, the one-time `aimee://` enrollment string used to establish the server's mTLS
  identity with the KB.

The shipped server Compose files pass explicit values through from `.env` when
present. Without an explicit packet, the managed wizard uses its durable
identity and read-only managed trust volumes. The explicit certificate-bound
first wizard owner remains available for bootstrap administration, and a local
Unix-socket operator cannot be locked out.

For a split/external authority, create the first team locally without exposing
an HTTP admin route:

```bash
KB_CONTAINER=$(docker ps --filter label=com.docker.compose.project=aimee \
  --filter label=com.docker.compose.service=aimee-kb --format '{{.ID}}')
docker exec \
  -e 'AIMEE_DB2_URL=postgresql:///aimee_shared?host=/var/lib/aimee/run' \
  "$KB_CONTAINER" aimee-kb team create default
```

Use the returned numeric team id when the authority enrolls the server. After finalizing the matching
server-registry row and publishing signed JWKS, install the exported public trust bundle and record
the enrollment values:

```bash
sudo install -d -o root -g root -m 0755 server-management
sudo install -o root -g root -m 0644 /path/from/authority/jwks-trust-bundle.json \
  server-management/jwks-trust-bundle.json

cp -n .env.example .env
cat >>.env <<'EOF'
AIMEE_SERVER_ID=YOUR_ENROLLED_SERVER_ID
AIMEE_SERVER_TEAM_ID=YOUR_NUMERIC_TEAM_ID
AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE=/run/aimee/management/jwks-trust-bundle.json
AIMEE_KB_CONN=aimee://THE_ONE_TIME_ENROLLMENT_STRING
EOF

scripts/compose-local.sh -f compose.server-managed.yaml up -d --force-recreate aimee-server
```

The bundle is public verification material. In the shipped container it must be root-owned and
readable by server UID 1000, so use `0644`; group/world write bits, symlinks, extra hard links, and a
non-root owner are rejected. On successful enrollment the certificate and key are atomically saved
at `$AIMEE_HOME/kb-client-identity.json` with mode `0600`. The one-time token is never saved, and the
identity is revalidated against its CA pin after every process restart.

Grant administration happens **on aimee-kb**, using the exact subject returned by the user's PAM or
OIDC login. The server holds no administrative KB identity and does not proxy this operation.
Server-side dispatch for `aimee kb grant …` is removed. Its legacy grammar may still appear in
client help, but those subcommands do not dispatch.

```bash
# on aimee-kb, as a principal with admin or team-lead authority IN the target team
POST /v1/write-tier-grants/set  {"server_id": "<server-id>", "team_id": <team-id>,
                                 "subject": "<subject>", "tier": "data"}
GET  /v1/write-tier-grants?server_id=<server-id>&team_id=<team-id>&subject=<subject>
```

Use `data` for memory, document, and index writes. Use `full` only for users who also need agent,
delegate, runner, or workspace-control operations. See [Upgrading](UPGRADING.md#restore-remote-writes)
for subject forms, first-grant recovery, and refusal reasons.

## 5. Add a workspace

Run this on the machine that holds the source tree:

```bash
cd /path/to/project
aimee workspace add .
aimee index scan .
aimee index overview
aimee index find main
```

The client uploads content to the server and KB. The remote server never reads `/path/to/project`
directly.

Workspace registration and index upload both work for the first wizard user after enrollment. An
additional authority-managed user needs at least a `data` grant.

> **Not available on the Windows thin client.** `workspace add` and `index scan` upload the working
> tree over a POSIX-only path, so on Windows they refuse:
>
> ```
> aimee: remote workspace add is not supported on this platform
> aimee: remote index scan is not supported on this platform
> ```
>
> Register and index the tree from a Linux or macOS client that can reach it, or clone it onto the
> server through the setup wizard's *Workspaces & projects* step. The read side works normally on
> Windows: `workspace list`, `index overview` and `index find` all query the server.

Large repositories ingest in chunks. The client prints one progress line per uploaded batch. Use
`aimee kb status` to inspect the queue. A channel-matched client also provides the dedicated
`aimee kb ingest status` view.

## 6. Connect a coding tool

Client setup registers the local MCP server and supported hooks. To keep global tool configuration
unchanged:

```bash
export AIMEE_NO_CLIENT_INTEGRATIONS=1
```

For manual MCP setup, use a stdio server with:

```text
aimee mcp-serve
```

The process inherits the enrolled remote target. It exposes memory, index, delegation, and other
allowed tools while all state remains on the server.

Use `aimee api status` for OpenAI- or Anthropic-compatible endpoint snippets. The model API binds
server loopback by design; when the coding tool runs on another machine, use the SSH tunnel printed
by the command before pasting the local URL into the editor. ACP editors use the ACP bridge. The
removed `aimee chat` TUI is not part of current builds.

## 7. Add delegates

List the roster. A fresh install contains only the agent created in the wizard;
add delegates explicitly when you want them:

```bash
aimee agent list
aimee provider list --available
```

Remote agent and delegate commands require a `full` grant, including `aimee agent list`.

`ON` in the roster means configured, not authenticated. Before probing or delegating, make
sure `provider list --available` shows a provider you intend to use. Local providers need their
endpoint registered; API or OAuth credentials belong in the server vault, not `agents.json` or the
project:

```bash
aimee vault unlock
aimee vault set <agent> <credential-name> <secret>
```

Probe an agent before relying on it:

```bash
aimee agent probe <name>
```

A failed execution probe exits non-zero, even though the server successfully completed the
diagnostic request.

Run one task:

```bash
aimee delegate review --persona reviewer "Review the current diff"
```

Delegation is asynchronous. The command prints a job id; follow it to completion rather than
treating `pending` as a successful review:

```bash
aimee jobs status <job-id>
```

See [Delegates](DELEGATES.md) for local endpoints, API providers, CLI agents, roles, and sandbox
policy.

## 8. Back up before changing topology

Back up the application home (including Vault), encrypted PostgreSQL volume, and workspace
artifacts. Use PostgreSQL-native consistent dumps or stop writes before taking matched volume
snapshots. See [Deployment](DEPLOYMENT.md#volumes-and-backup) for restore requirements. Never run `docker compose down -v` while a
named volume is your only copy.

## Service commands

```bash
scripts/compose-local.sh -f compose.server-managed.yaml ps
scripts/compose-local.sh -f compose.server-managed.yaml logs -f
scripts/compose-local.sh -f compose.server-managed.yaml restart
scripts/compose-local.sh -f compose.server-managed.yaml down
```

`down` keeps named volumes. `down -v` deletes them.

## Next

- [Manual](../MANUAL.md)
- [Event bus](EVENT_BUS.md)
- [Security](SECURITY.md)
- [Workflows](WORKFLOWS.md)
- [What's new since v0.2.192](WHATS_NEW.md)
