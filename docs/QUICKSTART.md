# Quickstart

Run the services on one machine. Install only the thin client where you write code.

## 1. Start the managed server

You need Docker with Compose v2. The managed server mounts the Docker socket so its browser wizard
can start the KB and inference containers.

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
docker compose -f compose.server-managed.yaml up -d
```

Open <https://localhost:8443> and sign in with:

```text
user:     aimee
password: aimee-local-dev
```

The wizard first creates your operator username and password, disables the published development
login, and moves the current browser session to the new account. The password hash and bootstrap
retirement marker live on the persistent server volume, so `aimee / aimee-local-dev` does not return
after an image update. Then choose the primary agent, CPU or GPU inference, git accounts, and
workspaces.
The final **Deploy** step starts:

- `aimee-kb`, including private PostgreSQL 18, pgvector, and pgvectorscale;
- `aimee-llm`, with the selected inference tier.

For a local managed KB, Deploy also runs two explicit one-shot jobs before it
reports success:

- an isolated authority bootstrap provisions the management-token and manifest
  roots, publishes the signed generation-1 JWKS, and writes only the public
  trust bundle into a root-owned volume mounted read-only by the server; and
- the KB enrolls distinct server client/management certificates and writes the
  resulting workload identity directly into the server's private volume.

The offline provisioner and publisher are shipped in a separate image; they are
not linked into or installed in the ordinary KB/server images. The default
single-host authority is software-backed and appropriate to the local managed
installation. Deployments requiring hardware custody should keep using an
operator-managed authority/KMS and supply the explicit identity packet.
The one-shot locks its address space when the runtime permits it. On an
unprivileged container host that cannot raise `RLIMIT_MEMLOCK`, it proceeds only
after verifying through `/proc/swaps` that the host has no active swap; otherwise
Deploy fails closed.

Deploy also claims the signed-in browser account as the first remote owner. It displays one
`aimee remote set ...` command that provisions that user's bearer, mTLS certificate, and explicit
`full` write grant. Keep that page open until you run the command in step 3.

When the wizard creates a local `aimee-llm`, it also creates a separate, persistent 256-bit service
identity for `aimee-kb`. The managed stack supplies the endpoint, role/tier configuration, and bearer
to both containers, then the KB uses that credential for embedding, reranking, and synthesis. This is
automatic; do not copy the user's enrollment bearer into the LLM configuration. The managed LLM
refuses to start if its service credential is missing.

Complete the account step before exposing the host. Deployments that inject a non-default
`AIMEE_WEBCHAT_USER` and `AIMEE_WEBCHAT_PASSWORD` are already considered secured and skip that step.

### Choosing an image channel

The stack runs the released `:latest` images by default. To run a tested-but-unreleased build, set
`AIMEE_IMAGE_TAG` once — it moves the server, the KB and the LLM together:

```bash
AIMEE_IMAGE_TAG=testing docker compose -f compose.server-managed.yaml up -d
```

Set it for the wizard's **Deploy** step too, not just the server: the server re-runs Compose for the
managed services, so the tag has to be in its environment or the KB and LLM fall back to `:latest`
while the server runs `:testing`. The line above already does this. Mixing versions this way is a
real failure mode, not a theoretical one — a KB and a server from different builds can disagree
about the inference contract and leave the KB permanently unhealthy.

A single service can still be pinned individually (`AIMEE_KB_IMAGE=…`), and an explicit pin always
wins over `AIMEE_IMAGE_TAG`.

Check the containers:

```bash
docker compose -f compose.server-managed.yaml ps
docker compose -f compose.server-managed.yaml logs --tail=100 aimee-server
```

If the server must not control Docker, use the split stack instead:

```bash
docker compose -f deploy/compose/aimee.yaml up -d
```

The old combined image is gone.

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

Copy the exact command shown by the wizard after **Deploy**. It looks like this:

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

cat >>.env <<'EOF'
AIMEE_SERVER_ID=YOUR_ENROLLED_SERVER_ID
AIMEE_SERVER_TEAM_ID=YOUR_NUMERIC_TEAM_ID
AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE=/run/aimee/management/jwks-trust-bundle.json
AIMEE_KB_CONN=aimee://THE_ONE_TIME_ENROLLMENT_STRING
EOF

docker compose -f compose.server-managed.yaml up -d --force-recreate aimee-server
```

The bundle is public verification material. In the shipped container it must be root-owned and
readable by server UID 1000, so use `0644`; group/world write bits, symlinks, extra hard links, and a
non-root owner are rejected. On successful enrollment the certificate and key are atomically saved
at `$AIMEE_HOME/kb-client-identity.json` with mode `0600`. The one-time token is never saved, and the
identity is revalidated against its CA pin after every process restart.

Grant administration is local-socket only. Run it on the server, using the exact subject returned by
the user's PAM or OIDC login:

```bash
aimee kb grant set --server <server-id> --team <team-id> --subject <subject> --tier data
aimee kb grant show --server <server-id> --team <team-id> --subject <subject>
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

List the seeded roster:

```bash
aimee agent list
aimee provider list --available
```

Remote agent and delegate commands require a `full` grant, including `aimee agent list`.

`ON` in the seeded roster means configured, not authenticated. Before probing or delegating, make
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

The embedded KB database lives in the KB home volume. Export it before moving to external
PostgreSQL or replacing compose files:

```bash
./deploy/container/aimee-kb-db-export.sh --help
```

Also back up `~/.config/aimee/` from the server volume. Never run `docker compose down -v` while a
named volume is your only copy.

## Service commands

```bash
docker compose -f compose.server-managed.yaml ps
docker compose -f compose.server-managed.yaml logs -f
docker compose -f compose.server-managed.yaml restart
docker compose -f compose.server-managed.yaml down
```

`down` keeps named volumes. `down -v` deletes them.

## Next

- [Manual](../MANUAL.md)
- [Event bus](EVENT_BUS.md)
- [Security](SECURITY.md)
- [Workflows](WORKFLOWS.md)
- [What's new since v0.2.192](WHATS_NEW.md)
