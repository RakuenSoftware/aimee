# Quickstart

Run the services on one machine. Install only the thin client where you write code.

## 1. Start the managed server

You need Docker with Compose v2. The managed server mounts the Docker socket so its browser wizard
can start the KB container.

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
docker compose -f compose.server-managed.yaml up -d
docker compose -f compose.server-managed.yaml logs aimee-server
```

When you do not supply a dashboard login, the server generates one on first boot and prints it once
in that log. The values below show the format. Your values will be different:

```text
[webchat] FIRST-BOOT DASHBOARD LOGIN (shown once — copy it now)
[webchat]     username: aimee-0a901de6e2c3
[webchat]     password: <64 hex characters>
```

Copy the generated password before the log rotates. The plaintext cannot be read back. The data
volume keeps the username and a root-only password verifier so the PAM account survives a container
replacement. If you lose the plaintext, reset that account's password inside the container or start
again from an empty volume. To choose the credential yourself instead, seal it before the first
`up`. Nothing is then generated or printed:

```bash
export AIMEE_WEBCHAT_USER=admin
read -rsp 'Initial webchat password: ' AIMEE_WEBCHAT_PASSWORD && echo
export AIMEE_WEBCHAT_PASSWORD
scripts/aimee-compose-vault-bootstrap.sh -f compose.server-managed.yaml server
unset AIMEE_WEBCHAT_PASSWORD
docker compose -f compose.server-managed.yaml up -d
```

The first-boot path accepts a name that is also a group in the image because it creates the account
with `aimee-webchat` as its primary group. The wizard's replacement-account step refuses that
collision and tells you to choose another name.

Run the bootstrap script; do not simply export the variables and `up`. `compose.server-managed.yaml`
deliberately keeps these two out of the server's `environment:` block, because anything listed there
persists in the container's `Config.Env` for the life of the deployment and is readable from
`docker inspect`. The script streams them into Vault through a one-shot container instead, so
`docker compose up` on its own never sees them and would leave you with a generated login.

aimee also needs a git identity, sealed the same way. Without it every commit aimee makes has no
author and git refuses it, so `aimee git commit`, delegate commits and workflow commits all fail:

```bash
export AIMEE_GIT_AUTHOR_NAME='Your Name'
export AIMEE_GIT_AUTHOR_EMAIL='you@example.com'
scripts/aimee-compose-vault-bootstrap.sh -f compose.server-managed.yaml server
```

There is deliberately no default. aimee points `GIT_CONFIG_GLOBAL` and `GIT_CONFIG_SYSTEM` at
`/dev/null` so a commit cannot silently inherit the machine's identity, and it will not invent a bot
author: a commit that cannot say who made it is not made.

Those variables are first-boot transport, not runtime configuration. On first boot the entrypoint
reads the sealed pair and provisions a real local PAM account. The appliance authenticates against
PAM from the first login onward. The data volume keeps a root-only shadow verifier so the account
survives an image replacement; it does not keep the plaintext password. An operator-supplied
password is not logged. A generated password appears only in the first-boot log shown above.

Open <https://localhost:8443> and sign in.

![The aimee sign-in page](images/login.png)

Signing in lands you in the chat view with **Set up this instance** open at **Secure your account**.
It is a dialog, not a separate page. A fresh local setup currently has seven steps. A remote KB
hides the two local-infrastructure steps, and reopening the wizard hides work you already completed,
so the displayed total can be smaller. Closing the dialog does not lose saved work: the **Setup**
button in the header reopens it and shows how many required steps remain.

### Step 1: replace the temporary login

![Step 1 of the setup wizard, secure your account](images/wizard-1-account.png)

The generated login is temporary. This step creates the account you will keep, as a real local PAM
account, and removes the plaintext of the temporary one. A deployment that sealed its own
`AIMEE_WEBCHAT_USER` pair before first boot skips this step.

Pick a username that is not already a group on the host. The image ships the usual Unix groups, so
names such as `operator`, `backup`, `staff`, `users`, `news`, `mail`, `proxy`, `adm`, and `aimee`
are unavailable. The wizard names the collision and asks you to choose another username.

### Step 2: choose the primary provider

![Step 2 of the setup wizard, primary provider](images/wizard-2-provider.png)

An API key for Anthropic or OpenAI, or a Claude or Codex subscription seat signed in through the
browser. The key is sealed into the server vault; its plaintext is not written to the data volume.
You can change the primary later on the Agents tab.

### Step 3: choose the knowledge base

![Step 3 of the setup wizard, knowledge base](images/wizard-3-knowledge-base.png)

Deploy one locally, or point at an existing `aimee-kb`. A local knowledge base is the default and
needs nothing else installed.

For a local KB, the remaining steps place its embedder and optional synthesizer, choose the bundled
or external shared store, connect an optional Git host, and add workspaces. The embedder runs inside
the KB container; synthesis runs in its own sidecar or at a remote endpoint. A Git connection can be
skipped because public repositories do not require one. You can continue without a
workspace, but setup remains incomplete until at least one project exists.

After the numbered steps, the summary shows a **Deploy the local stack** panel. Its **Deploy** action
starts one `aimee-kb`, including private PostgreSQL 18, pgvector, and pgvectorscale.

**The embedder runs inside that container. Synthesis does not.** Embedding is served from weights
baked into the KB image, so it needs no second container. If you selected a local synthesis model,
Deploy also starts an `aimee-llm` sidecar beside the KB, and the KB reaches it over mutual TLS on the
Compose network. Choose an external endpoint instead and no sidecar is deployed at all; choose neither
and synthesis is simply off, which is a supported state. See
[Choosing an embedder](#choosing-an-embedder), [Local synthesis](#local-synthesis) and
[KB model backends](KB_LLM_BACKENDS.md).

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

The Deploy action also claims the signed-in browser account as the first remote owner. It displays one
`aimee remote set ...` command that provisions that user's bearer, mTLS certificate, and explicit
`full` write grant. Keep that page open until you finish [Section 3](#3-enroll-the-client).

Complete the account step before exposing the host. A deployment that seals both
`AIMEE_WEBCHAT_USER` and `AIMEE_WEBCHAT_PASSWORD` before the first `up` uses that pair and skips
account replacement.
Supply both or neither: a partial pair is treated as absent, and the server generates and logs a
credential instead.

The browser login is a **local PAM account**, not an aimee credential. First boot provisions the
supplied (or generated) pair as a real system account in the `aimee-webchat` group and authenticates
it through the `aimee` PAM service, the same stack SmoothNAS uses and the same one a KB means when
`/v1/identity/auth-mode` reports `pam`. The plaintext first-boot value is removed once that account
exists. Only accounts in that group are dashboard logins: the container's own system users are never
accepted, and the dashboard cannot see or modify them.

The Vault holds aimee's own secrets: the session key, TLS material, provider credentials. A host
password is not one of those and is never sealed into it.

When the appliance is connected to a KB that reports `oidc`, the identity provider owns accounts and
the wizard's account step disappears; local account creation is refused. Dashboard login itself
remains PAM in this release. Do not configure OIDC-only identity until the browser login flow is
available.

### Choosing an embedder

The wizard's **Deploy topology** step records which embedder the KB uses. Choose one before Deploy.

Which embedder a KB can run is a property of the image you pulled, because the weights are baked in:

| image | embedder | size |
| --- | --- | --- |
| `aimee-kb-a25m` | `bekko-a25m`, 384-dimension | 1.95 GB |
| `aimee-kb-nomic` | `nomic-embed-text-v2-moe`, 768-dimension | 3.34 GB |
| `aimee-kb` | none baked; for an external `EMBEDDER_URL` | 373 MB |

**An embedder is not optional.** Retrieval does not work without one, so the choice is
between a bundled model and an external endpoint, never "neither". `aimee-kb` exists
for the external case: it omits PyTorch and the weights rather than shipping code it
will never run. It is not a way to run without an embedder.

Synthesis is different and genuinely optional: local, external, or off. Off is a
supported state because embedding, search, recall and indexing never call it.

A bundled embedder needs no download and no second container.

**This choice does not survive a change of mind.** DB2 records the vector-column width and refuses to
start when it drifts, so moving between 384 and 768 means re-embedding the whole corpus. Choose before
you ingest anything.

Nothing is selected on a fresh install, and **a KB with no embedder refuses to start**. It says so
and exits:

```text
aimee-kb: no embedder selected, and there is no fallback. Retrieval needs one.
aimee-kb:   pick a bundled model:  aimee config set embedder_model bekko-a25m
aimee-kb:   or point at your own:  EMBEDDER_URL=http://<host>:<port>
aimee-kb: then re-run Deploy. Refusing to start.
```

There used to be a lexical fallback here, so an unconfigured KB came up healthy and answered every
search with keyword matching. A deployment could run for weeks believing it had vector retrieval. It
is gone. If you skipped the step, set it from the server and re-run Deploy:

```bash
aimee config set embedder_model bekko-a25m
```

Once anything has been embedded, changing the embedder is a corpus migration rather than a setting,
and the KB refuses the switch rather than mixing two vector spaces. See
[Change the KB embedder](runbooks/change-embedder.md). Choosing in the wizard, before the first
Deploy, avoids the question entirely.

Confirm the model actually loaded rather than assuming it did:

```bash
docker compose -p aimee logs aimee-kb | grep -i embedder
aimee kb status
```

Address the managed services by project (`-p aimee`), not by the file you started the server with.
`compose.server-managed.yaml` declares only `aimee-server`; the server brings `aimee-kb` and
`aimee-llm` up from its own baked manifest into the same `aimee` project, so
`-f compose.server-managed.yaml logs aimee-kb` fails with `no such service`.

A loaded embedder logs its dimension and serving identity:

```text
aimee-kb: starting bundled embedder (bekko-a25m) on :8760
embedder-server: loaded hotchpotch/bekko-embedding-v1-a25m dim=384 threads=8 quant=fp32
```

Decide before you ingest. The wizard warns when a later choice changes the vector space, but saving
the choice does not perform the migration. A different dimension needs the guarded vector-schema
reset; a same-dimension model, pooling, or prefix change needs a fresh DB2 and source re-ingestion
because the current reset command deliberately no-ops when the dimensions match. Follow
[Change the KB embedder](runbooks/change-embedder.md) before changing an active corpus.

### Local synthesis

Synthesis writes curation and summaries. Unlike the embedder it is not inside the KB container: it is
its own image, `aimee-llm-e2b` or `aimee-llm-e4b`, deployed beside the KB when the wizard selects a
local model. Which model it carries is a property of the tag, because the weights are baked in.

| image | model | weights |
| --- | --- | --- |
| `aimee-llm-e2b` | gemma-4-E2B-it | 2.62 GB (qat-UD-Q4_K_XL) |
| `aimee-llm-e4b` | gemma-4-E4B-it | 7.46 GB (UD-Q6_K_XL) |

E4B is the better model; E2B is roughly half the resident memory and about twice the CPU speed. See
[Choosing a synthesis model](SYNTHESIS_MODELS.md) for the measurements behind that.

Three states are all supported, and `off` is not an error: embedding, search, recall and indexing
never call synthesis.

- **local**: an `aimee-llm-*` sidecar, reached over mutual TLS
- **external**: `SYNTHESIS_ENDPOINT` at any OpenAI-compatible endpoint
- **off**: no synthesis

**Unlike the embedder, this is not a one-way door.** The sidecar holds no data, so switching between
E2B and E4B, adding synthesis to a running deployment, or removing it is a container swap with the KB
left running.

Confirm the sidecar actually came up, rather than assuming Deploy succeeded:

```bash
docker compose -p aimee logs aimee-llm | grep -iE 'synthesis|terminator'
```

A working sidecar logs both halves:

```text
aimee-llm: starting synthesis (gemma-4-E2B-it) on 127.0.0.1:8760
aimee-llm: starting mTLS terminator on :8761 (client certificate required)
```

The mTLS identity is issued by the KB at startup, which is why the KB is deployed first. The sidecar
refuses to start without it rather than serving unauthenticated, so "no identity" fails loudly at
deploy instead of quietly at the first curation call.

### Choosing an image channel

The stack runs the released `:latest` images by default. To run a tested-but-unreleased build, set
`AIMEE_IMAGE_TAG` once. It moves every image in the topology together, including the server, KB,
and browser console:

```bash
AIMEE_IMAGE_TAG=testing docker compose -f compose.server-managed.yaml up -d
```

Set it for the summary's **Deploy** action too, not just the server: the server re-runs Compose for the
managed services, so the tag has to be in its environment or the KB falls back to `:latest` while the
server runs `:testing`. The line above already does this. Mixing versions this way is a real failure
mode, not a theoretical one. A KB and a server from different builds can disagree about the
contract between them and leave the KB permanently unhealthy.

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
