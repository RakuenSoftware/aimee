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

Run the setup wizard. Choose the primary agent, CPU or GPU inference, git accounts, and workspaces.
The final **Deploy** step starts:

- `aimee-kb`, including private PostgreSQL 18, pgvector, and pgvectorscale;
- `aimee-llm`, with the selected inference tier.

Change `AIMEE_WEBCHAT_USER` and `AIMEE_WEBCHAT_PASSWORD` before exposing this host. The defaults are
for local setup only.

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

Download the binary for your platform from the latest GitHub release.

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
$env:Path = "$env:Path;$bin"
aimee version
```

Add that directory to your persistent user `PATH` before opening the next PowerShell.

The client is DB-free. It does not need PostgreSQL, SQLite, the KB, or model libraries.

## 3. Enroll the client

Use the server URL and the one-use bootstrap bearer:

```bash
aimee remote set https://server.example:8743 aimee-local-dev
aimee remote status
```

`remote set`:

1. connects to the private server certificate;
2. prints and stores its fingerprint;
3. trades the bootstrap bearer for a deployment bearer;
4. enrolls an individual mTLS certificate on Linux;
5. writes private state to `~/.config/aimee/remote.conf`.

Verify the fingerprint against the server through a second channel. Do not accept an unexpected
change. The bootstrap bearer stops authorizing ordinary routes after enrollment.

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

### Write and agent access

This section is write-capable on Linux; the macOS and Windows clients remain read-only until their
automatic certificate-enrollment paths are implemented.

There is no extra authority bootstrap in the single-user quickstart. The managed server image
explicitly enables `remote_writes: full`, and `aimee remote set` enrolls the Linux client with its
own mTLS certificate. The image requests that certificate automatically; no server-side TLS setting
is required. Until a per-user authority is configured, that enrolled certificate may use
the configured deployment tier. A copied bearer without a valid client certificate remains
read/query-only.

Test a durable write and read:

```bash
aimee memory store quickstart "Enrollment works"
aimee memory search "Enrollment works"
```

That is the complete write setup for the default managed deployment. `remote_writes: data` limits
an enrolled client to data mutation; `off` makes the TCP service read-only.

Multi-user deployments can opt into strict KB-signed per-user grants. Once `AIMEE_SERVER_ID`,
`AIMEE_SERVER_TEAM_ID`, and the management JWKS trust bundle are configured, the compatibility
path switches off automatically and every remote write requires a valid per-user identity token.
That authority deployment is an operator workflow, not a quickstart prerequisite.

#### Optional: strict multi-user authority

Skip this subsection for the normal single-user setup. For a strict deployment:

1. Create the first KB team through its private database socket:

   ```bash
   KB_CONTAINER=$(docker ps --filter label=com.docker.compose.project=aimee \
     --filter label=com.docker.compose.service=aimee-kb --format '{{.ID}}')
   docker exec \
     -e 'AIMEE_DB2_URL=postgresql:///aimee_shared?host=/var/lib/aimee/run' \
     "$KB_CONTAINER" aimee-kb team create default
   ```

2. Complete the authority enrollment for that numeric team. It produces the enrolled server ID,
   one-use `AIMEE_KB_CONN`, and signed public JWKS trust bundle. Finalize the matching server-registry
   row and publish the JWKS before enabling strict mode.

3. Install the public bundle and configure all authority inputs together:

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

4. From the server's local Unix-socket client, grant the exact authenticated subject:

   ```bash
   aimee kb grant set --server <server-id> --team <team-id> --subject <subject> --tier data
   aimee kb grant show --server <server-id> --team <team-id> --subject <subject>
   ```

Use `full` only for subjects that also need agent, delegate, runner, or workspace-control writes.
Strict mode fails closed if its trust, identity-token, replay, or grant authority is unavailable.
See [Upgrading](UPGRADING.md#restore-remote-writes) for subject forms and recovery details.

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

Workspace registration and index upload both work after the client enrollment above. With strict
per-user authorization enabled, the subject instead needs at least a `data` grant.

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

Large repositories ingest in chunks. Use `aimee kb status` and `aimee kb ingest status` to follow
the queue.

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

Use `aimee api status` for OpenAI- or Anthropic-compatible endpoint snippets. ACP editors use the
ACP bridge. The removed `aimee chat` TUI is not part of current builds.

## 7. Add delegates

List the seeded roster:

```bash
aimee agent list
aimee provider list --available
```

Remote agent and delegate commands require a `full` grant, including `aimee agent list`.

Probe an agent before relying on it:

```bash
aimee agent probe <name>
```

Run one task:

```bash
aimee delegate review --persona reviewer "Review the current diff"
```

Local API or OAuth credentials belong in the server vault, not `agents.json` or the project:

```bash
aimee vault unlock
aimee vault set <agent> <credential-name> <secret>
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
