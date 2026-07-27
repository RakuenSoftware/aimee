# aimee Quickstart

This guide takes you from nothing to a working aimee install in four parts:

1. **[Run the server](#part-1-run-the-server-in-docker)**, deploy the single `aimee-server` container and let the setup wizard bring up the self-contained knowledge base and LLM for you.
2. **[Install the Linux client](#part-2--linux-client)**, install the thin `aimee` binary, point it at your server, and set up workspaces and agents.
3. **[Install the Windows client](#part-3--windows-client)**, same, for Windows.
4. **[Install the macOS client](#part-4--macos-client)**, same, for macOS.

The model is the same on every developer machine: **the services run in Docker (or on a Linux/macOS host); each developer installs only the thin `aimee` client and points it at the server.** The client holds no database, it talks to the server over the `/v1` HTTP API.

---

## Part 1, Run the server (in Docker)

You start **one container — `aimee-server`** — with the host Docker socket mounted. Everything else is handled in the browser: the setup wizard's **Deploy** step brings up `aimee-kb` (with its bundled PostgreSQL DB2 + pgvector) and `aimee-llm` for you, on CPU or GPU. There is no second compose command or database container.

Advanced operators who prefer to run each service as its own long-lived container (the manual split stack — `deploy/compose/aimee.yaml` and friends) still can; see [1.5](#15-managing-the-stack) and [MANUAL.md](../MANUAL.md). This guide takes the self-deploying path.

### Prerequisites

- Docker Engine + the Docker Compose plugin (`docker compose`, v2).
- `git`, `curl` and `jq` — used by the commands below. A stock server image may have none of them (`apt-get install git curl jq`).
- Host Docker socket access — `compose.server-managed.yaml` mounts it so the server can launch the other containers. It's root-equivalent, so run this only on a host you trust the server on.
- **~25 GB free disk.** Measured on a clean Debian 13 host after a full CPU deploy: 11.7 GB of images + 7 GB of volumes ≈ 19 GB of Docker data, ~20 GB total with the OS and checkout. Most of it is `aimee-llm-cpu`, an 11 GB image with the embed/rerank/synth weights **baked in** — the CPU tier downloads no models at deploy time and mounts no model volume. (The GPU `aimee-llm` image is small and fetches its tier into a volume on first boot instead.)
- **8 GB RAM works; 16 GB is comfortable.** The model weights are mmap'd, so most of the LLM container's footprint is reclaimable page cache — on a 16 GB host the idle stack sits at ~4 GB resident plus ~5 GB cache.
- No credentials or API keys needed for the default build.

### 1.1 Start the server

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
docker compose -f compose.server-managed.yaml up -d
```

Starts `aimee-server` only (webchat built in). `/v1` is on `:8743` (native TLS, self-signed; plaintext `:8740` is loopback-only, not published); webchat is on `:8443`. Nothing else is up yet.

### 1.2 Run the setup wizard

Open **https://localhost:8443** (accept the self-signed cert) and log in as `aimee` / `aimee-local-dev`. That single account gates the whole browser experience — the setup wizard, webchat, and the dashboards.

The wizard covers:

- **Primary provider** — which agent aimee drives.
- **Knowledge base** — a local one (default), or an existing remote `aimee-kb`.
- **Deploy topology** — where the embedder / reranker / synthesizer run (CPU by default, or a GPU).
- **Shared store** — PostgreSQL bundled inside `aimee-kb` (default; nothing to enter), or an existing external database.
- **Connection** — connect a git host so aimee can clone your repos. Pick one auth method and the wizard shows only its fields: **OAuth sign-in** (GitHub, GitLab, Gitea/Forgejo), an **access token** (HTTPS, any host including Bitbucket), or an **SSH key** (private key for `git@host:owner/repo.git`). Public repos clone without any of them. Optional — you can connect a host later.
- **Workspaces & projects** — point at an owner/org, list its repos, and bulk-clone them into a workspace.

The final **Deploy** launches `aimee-kb` + the LLM container and shows their status. On first boot, the KB initializes PostgreSQL inside its own container. On the default CPU topology the LLM container is `aimee-llm-cpu`, whose weights are baked into the image, so the wait is the 11 GB image pull rather than a model download; later boots are fast — state lives in named volumes.

#### Your login account

The browser login is a real PAM account the container creates on startup from **`AIMEE_WEBCHAT_USER`** / **`AIMEE_WEBCHAT_PASSWORD`** (defaults `aimee` / `aimee-local-dev`) — **set your own in the compose file for anything past local dev.** The username must be a valid Linux name: **lowercase letters, digits, `-`, `_`, and not starting with a digit** (so `admin` or `web-user`, not `Admin` or `bob.smith`). For more than one login, set `AIMEE_WEBCHAT_USERS` to a comma-separated `user:password` list. If a login is rejected, check the container logs for `[webchat]` lines — they report exactly which account was created or why it couldn't be.

Each provisioned account is mirrored (username + its `/etc/shadow` hash, never the plaintext) to `AIMEE_HOME/webchat/logins` on the data volume and restored on every start, so browser logins survive a container rebuild or a host reboot even if the runtime stops re-injecting `AIMEE_WEBCHAT_USER`/`AIMEE_WEBCHAT_PASSWORD`.

### 1.3 Verify it's healthy

`aimee-local-dev` is a **one-time bootstrap bearer**: it authorizes exactly one call,
the rotation below, and nothing else. Using it on an ordinary route returns `401`
with `"the one-time bootstrap bearer must be rotated before use"` — that is the
expected response, not a broken server. Rotate it once, then use the token you get
back. (`-k` accepts the self-signed cert.)

```bash
# One-time: exchange the bootstrap bearer for a real one and keep it private.
ROTATED=$(curl -k -fsS -X POST \
  -H 'Authorization: Bearer aimee-local-dev' -H 'Content-Type: application/json' \
  https://localhost:8743/v1/api/rotate_bearer -d '{}' | jq -er '.bearer_token')
(umask 077 && printf '%s\n' "$ROTATED" > .aimee-server-bearer)

# Now /v1 answers. Both of these should print 200:
curl -k -sS -o /dev/null -w '%{http_code}\n' -H "Authorization: Bearer $ROTATED" \
  https://localhost:8743/v1/health
curl -k -sS -o /dev/null -w '%{http_code}\n' -H "Authorization: Bearer $ROTATED" \
  https://localhost:8743/v1/kb/status
```

`aimee remote set https://<host>:8743 <token>` does this rotation for you and stores
the result, so the thin client in [Part 2](#part-2-linux-client) needs no manual step;
`aimee remote enroll` re-runs it on its own.

Until the wizard's **Deploy** step has run, `/v1/health` returns `200` but
`/v1/kb/status` reports `"available": false` — the knowledge base container is not up
yet. That is expected at this point. After Deploy:

```bash
docker ps    # aimee-server, aimee-kb, and the LLM container
```

The LLM container is **`aimee-llm-cpu`** on the default CPU topology and `aimee-llm`
when a role is placed on a GPU; both answer to the network name `aimee-llm`.

### 1.4 Before you expose it on a network

- **Rotate the bootstrap bearer** ([1.3](#13-verify-its-healthy)) before anything reaches the network — until you do, `/v1` is unusable anyway. For a real deployment also mount your own `aimee.yaml` at `/var/lib/aimee/aimee.yaml` with your own bearer, and terminate TLS at a reverse proxy.
- **Remote writes are off by default, and are authorized per user.** Over the network a
  remote bearer is **read/query only**. Holding the bearer no longer grants write
  access to anyone: each subject that may write needs its own **write-tier grant**,
  and the tier comes from the caller's own kb-signed identity token, not from a
  server-wide switch.
  - `data`, data-plane writes (`memory store`, `work …`, `rules …`, `skill …`).
  - `full`, also exec/control (`delegate`, `agent`, `provider`, `cron`). **Trusted
    users only**, a `full` grant permits remote code execution as that subject.

  Grants live in `kb_write_tier_grant` and are administered with
  `kb_write_tier_grant_set` / `kb_write_tier_grant_revoke` (`aimee kb grant
  set|list|revoke --server <id> --team <n>`, local UDS only). A user obtains an
  identity token by logging in to kb (OIDC, or PAM when no OIDC profile is
  configured). See [UPGRADING.md](UPGRADING.md) for granting the first user, the
  exact subject spelling, and how to tell "no grant" apart from "wrong subject".

  **A grant is not enough on its own.** The server can only resolve a caller's tier
  once it knows who it is and which keys to trust, so all three of these must be set
  on `aimee-server` or *every* `/v1` write is denied regardless of grants:

  | Variable | What it is |
  |---|---|
  | `AIMEE_SERVER_TEAM_ID` | the kb team this server is enrolled in |
  | `AIMEE_SERVER_ID` | this server's id — the identity token's audience |
  | `AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE` | path to the root-owned `0600` trust bundle pinning kb's signing keys |

  **`compose.server-managed.yaml` does not set these**, so a stock Docker install
  cannot enable remote writes until you add them — see
  [#2026](https://github.com/RakuenSoftware/aimee/issues/2026). The server says so at
  startup, and that line is the fastest way to confirm the cause:

  ```
  ERROR server.http: AIMEE_SERVER_TEAM_ID is unset or invalid: reads continue, but
  every /v1 write will be denied with no_team_configured until it is set to this
  server's team id
  ```

  (Workspace registration over the network, `workspace add/serve/remove`, is a
  deliberate, bearer-gated exception and works with no grant at all.)

> **`aimee.api.remote_writes` no longer authorizes anything.** It is still parsed,
> so an old config loads, but it grants nothing: the server warns at startup and
> counts the requests it would formerly have allowed in
> `remote_writes.global_ignored` (visible via `aimee api status`). If writes are
> refused after an upgrade, the cause is a missing grant, not this setting.

### 1.5 Managing the stack

```bash
docker compose -f compose.server-managed.yaml logs -f              # server logs
docker compose -f compose.server-managed.yaml restart aimee-server # restart the server
docker compose -f compose.server-managed.yaml down                 # stop the server (managed containers keep running)
docker compose -f compose.server-managed.yaml up -d --pull always  # update the server image
```

The wizard-deployed services (`aimee-kb`, `aimee-llm`) run as their own Docker project — use `docker ps` / `docker logs`, or re-run the wizard's **Deploy** to reconcile them after a config change. KB and embedded-DB2 state live together in the `aimee-managed-kb-home` volume; removing it erases the knowledge base.

Advanced: you can run each service as its own container instead of letting the server orchestrate them (`compose.server.yaml`, `compose.yaml`) — see [MANUAL.md](../MANUAL.md).

---

## Part 2, Linux client

On Linux you can build and run the whole stack from source, but as a **client against a Docker/remote server** you only need the thin `aimee` binary, which talks to your `aimee-server` over `/v1`. The Linux build links OpenSSL, so it supports `https://` servers (verified against the system trust store), as do the prebuilt Windows and macOS clients via their native TLS backends (see [TLS support by build](#tls-support-by-build)).

### 2.1 Install `aimee`

**Option A, prebuilt binary (no build):**

```bash
# Download aimee-linux-x86_64 (or aimee-linux-arm64) from the latest GitHub release, then:
chmod +x aimee-linux-x86_64
mkdir -p ~/.local/bin
mv aimee-linux-x86_64 ~/.local/bin/aimee
# ensure ~/.local/bin is on your PATH (add to ~/.bashrc / ~/.zshrc if needed):
export PATH="$HOME/.local/bin:$PATH"
```

**Option B, build the thin client from source** (needs a C compiler, `make`, `libsqlite3-dev`, and `zlib1g-dev`; add `libssl-dev` to keep `https://` support):

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee/src
make -j"$(nproc)" ../aimee     # produces ./aimee at the repo root
cp ../aimee ~/.local/bin/aimee # or anywhere on your PATH
```

(Want the full server + kb running locally on this host instead of in Docker? Run `./install-deps.sh` then `./install.sh` from the checkout, that builds everything, bootstraps Postgres, and installs systemd user units. For the client-against-Docker setup in this guide, the thin client above is all you need.)

Confirm the install:

```bash
aimee version
```

### 2.2 Point the client at your server

```bash
# If you have NOT rotated the bootstrap bearer yet, pass it and the client rotates
# it for you (and pins the cert + enrols an mTLS client certificate):
aimee remote set https://YOUR_SERVER:8743 aimee-local-dev

# If you DID follow 1.3, that token is already spent — pass the rotated one instead:
aimee remote set https://YOUR_SERVER:8743 "$(cat .aimee-server-bearer)"

aimee remote status     # resolved transport + /v1/health probe
aimee status            # server, DB1, and knowledge-base health
```

`aimee-local-dev` only works **once per server**, so passing it after a previous
client (or [1.3](#13-verify-its-healthy)) already enrolled fails — `aimee remote set`
exits non-zero and tells you where the real bearer is. `aimee remote status` likewise
distinguishes "reachable but not authorized" (a bad or spent token) from unreachable,
and exits non-zero for both, so it is safe to use as a setup check in a script.

On a new default install, `remote set` pins the self-signed certificate, prints its
SHA-256 fingerprint for out-of-band verification, enrolls a client mTLS identity,
and rotates the public bootstrap bearer. `aimee-local-dev` then stops working. Use
your provisioned token instead if the operator disabled bootstrap enrollment.
Alternatives to `aimee remote set`: set `AIMEE_SERVER_URL` / `AIMEE_SERVER_TOKEN`,
or pass `--server https://YOUR_SERVER:8743 --server-token=...` per command.
Precedence is `--server` flag > env > persisted `remote.conf`.

### 2.3 Configure your AI coding tool

aimee wires itself into every detected tool (Claude Code, Codex CLI, Gemini CLI, GitHub Copilot) automatically — the first `aimee` command you run registers the SessionStart / PreToolUse / PostToolUse hooks (which call `aimee`) and the `aimee mcp-serve` MCP server into each tool's user config. To do it explicitly, or from the cloned checkout at install time, run:

```bash
./configure-hooks.sh
```

Opt out of the global registration with `AIMEE_NO_CLIENT_INTEGRATIONS=1` (or set `client_integrations_enabled: false` in `aimee.yaml`); per-project wiring via `aimee setup` still works.

### 2.4 Set up workspaces

A workspace is a set of repositories aimee indexes and works across as one unit.

```bash
aimee workspace add /path/to/your/repo   # register this host's checkout (see the note below on indexing)
aimee workspace list                     # list roots and the projects under each
```

You can also drop an `aimee.workspace.yaml` manifest in a directory and run `aimee setup` to clone, install dependencies, index, and generate starter rules for a multi-repo workspace in one shot, see [Workspace Management](WORKSPACES.md).

> Indexing and memory writes are server-side mutations, so over the network they need a **write-tier grant of at least `data` for your own subject** (see [1.4](#14-before-you-expose-it-on-a-network)); `aimee.api.remote_writes` no longer authorizes them.
>
> Registering a workspace is exempt and lands with no grant — but the **indexing it
> then kicks off is not**. Without a grant `aimee workspace add` reports
> `workspace registered`, then `index ingest batch failed … capabilities beyond the
> presented token's scope` and `ingested 0 file(s)`, and exits non-zero. The
> workspace is registered; nothing is indexed until a grant exists.
>
> The `agent` family (§2.5) is exec/control rather than a data write, so it needs a
> `full` grant — that includes reads like `aimee agent list`.
>
> **Cloning a repo onto the server** is a browser-side operation, not a CLI one:
> the setup wizard's *Workspaces & projects* step clones into the server's own
> workspace tree. `aimee workspace add --repo <url>` exists only in a local
> (same-host) install — against a remote server it refuses and points you here,
> because the clone route requires a browser login rather than a bearer.
>
> Without a grant these return `403 … requires capabilities beyond the presented
> token's scope`, so on a brand-new install your first `aimee memory store` or
> `aimee kb ingest` **will fail** until one is issued.
>
> The server's local Unix socket is exempt (same-user trusted peer), so on a
> single-machine install the quickest way to write is to run the command there:
>
> ```bash
> docker compose -f compose.server-managed.yaml exec -u aimee aimee-server \
>   aimee memory store my-key "some fact"
> ```
>
> For writes **from another machine**, that client's subject needs a write-tier
> grant — `data` for memory/index writes, `full` to also allow exec/control.
> Grants are keyed by `(server_id, team_id, subject)` and issued on the server:
>
> ```bash
> aimee kb grant set --subject <subject> --server <server-id> --team <team-id> --tier data
> aimee kb grant list --server <server-id> --team <team-id>      # confirm it landed
> ```
>
> [UPGRADING.md](UPGRADING.md) is the reference for the two parts that are easy to
> get wrong: how a subject is spelled for each login mode (a grant for the wrong
> spelling is silently a grant for nobody), and why the operator installs its own
> context with team `0`.

### 2.5 Add agents (delegates)

Delegates are cheaper/local models aimee routes routine work to. Configure them once:

```bash
aimee agent setup <provider>          # OAuth/device-flow setup for subscription providers (e.g. chatgpt, mistral-plan)
aimee agent add <name> --provider openai --model <model> --api-key <key>   # direct API key
aimee agent local local http://YOUR_LLM_HOST:8080 --model MODEL --slots 4  # local OpenAI-compatible runtime (Ollama / llama.cpp)
aimee agent list                      # inspect registered agents + routing data
```

Agent/provider control commands are exec/control operations, so over the network they need a **write-tier grant of `full` for your subject** (see [1.4](#14-before-you-expose-it-on-a-network)). The primary agent (Claude Code, Codex, …) then routes delegateable work automatically; you can also call `aimee delegate <role> "<task>"` directly. See [Setting Up Delegates](DELEGATES.md) for the full agent schema and routing details.

### 2.6 Interactive chat

`aimee acp-serve` and the web chat work against a remote server. When the client is pointed at a remote `/v1` endpoint, it registers your current directory as a **detached workspace** and opens a reverse channel back to it: the agent loop runs on the server, and its file and tool actions reach back into your local working tree over that channel. The client still holds no engine and no database, it renders the session and serves its own tree. Run `aimee chat` from inside the repository you want the agent to work in.

Because `launch`/`chat` are exec/control operations, a remote session needs a **write-tier grant of `full` for your subject** (see [1.4](#14-before-you-expose-it-on-a-network)). You can also drive aimee from your AI coding tool (configured in [2.3](#23-configure-your-ai-coding-tool)), or use the browser webchat at `https://YOUR_SERVER:8443`.

---

## Part 3, Windows client

On Windows aimee runs as the **thin client only**: a single `aimee.exe` that talks to your remote `aimee-server` over `/v1`. The server and kb run in Docker (Part 1) or on a Linux/macOS host, never on Windows.

> **TLS note:** the Windows client now speaks `https://` natively via **Schannel**, verifying the chain and hostname against the **Windows certificate store**, no OpenSSL and no bundled CA bundle, just the single self-contained `aimee.exe`. Both the prebuilt release binary and the `install.ps1` build enable it. Set `AIMEE_TLS_INSECURE=1` for a self-signed dev cert.

### 3.1 Install `aimee.exe`

**Option A, prebuilt binary (no build):**

1. Download **`aimee-windows-x86_64.exe`** from the [latest GitHub release](https://github.com/RakuenSoftware/aimee/releases).
2. Rename it to `aimee.exe` and put it in a folder on your `PATH` (e.g. `%LOCALAPPDATA%\aimee\bin`, then add that folder to your user PATH).

**Option B, build from source** (needs Git, CMake, and a C compiler, Visual Studio Build Tools `cl.exe` or MinGW `gcc`):

```powershell
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
.\install.ps1
```

`install.ps1` builds the thin client and installs it to `%LOCALAPPDATA%\aimee\bin`, adding that directory to your user `PATH`.

**Open a new PowerShell or Command Prompt** so the PATH change takes effect, then confirm:

```powershell
aimee version
```

### 3.2 Point the client at your server

```powershell
aimee remote set https://YOUR_SERVER:8743 aimee-local-dev
aimee remote status     # shows the resolved transport + a /v1/health probe
aimee status            # server, DB1, and knowledge-base health
```

Use your real bearer token instead of `aimee-local-dev` if you changed it. The server's `/v1` is TLS-only off-loopback; `https://` works with certificate verification on by default (Schannel against the Windows cert store), so set `AIMEE_TLS_INSECURE=1` for the auto-provisioned self-signed cert (or trust/pin it). Alternatives to `aimee remote set`: set `AIMEE_SERVER_URL` / `AIMEE_SERVER_TOKEN` environment variables, or pass `--server https://YOUR_SERVER:8743 --server-token=...` per command. Precedence is `--server` flag > env > persisted `remote.conf`.

### 3.3 Configure your AI coding tool

aimee wires itself into every detected tool (Claude Code, Codex CLI, Gemini CLI, GitHub Copilot) automatically — the first `aimee.exe` command registers the SessionStart / PreToolUse / PostToolUse hooks (which call `aimee.exe`) and the `aimee mcp-serve` MCP server into each tool's user config. To do it explicitly, run:

```powershell
.\configure-hooks.ps1
```

Opt out of the global registration with `AIMEE_NO_CLIENT_INTEGRATIONS=1` (or set `client_integrations_enabled: false` in `aimee.yaml`); per-project wiring via `aimee setup` still works.

### 3.4 Set up workspaces

A workspace is a set of repositories aimee indexes and works across as one unit.

```powershell
aimee workspace add C:\path\to\your\repo   # register this host's checkout (see the note below on indexing)
aimee workspace list                            # list roots and the projects under each
```

> Indexing and memory writes are server-side mutations, so over the network they need a **write-tier grant of at least `data` for your own subject** (see [1.4](#14-before-you-expose-it-on-a-network)); `aimee.api.remote_writes` no longer authorizes them.
>
> Registering a workspace is exempt and lands with no grant — but the **indexing it
> then kicks off is not**. Without a grant `aimee workspace add` reports
> `workspace registered`, then `index ingest batch failed … capabilities beyond the
> presented token's scope` and `ingested 0 file(s)`, and exits non-zero. The
> workspace is registered; nothing is indexed until a grant exists.
>
> The `agent` family (§2.5) is exec/control rather than a data write, so it needs a
> `full` grant — that includes reads like `aimee agent list`.
>
> **Cloning a repo onto the server** is a browser-side operation, not a CLI one:
> the setup wizard's *Workspaces & projects* step clones into the server's own
> workspace tree. `aimee workspace add --repo <url>` exists only in a local
> (same-host) install — against a remote server it refuses and points you here,
> because the clone route requires a browser login rather than a bearer.
>
> Without a grant these return `403 … requires capabilities beyond the presented
> token's scope`, so on a brand-new install your first `aimee memory store` or
> `aimee kb ingest` **will fail** until one is issued.
>
> The server's local Unix socket is exempt (same-user trusted peer), so on a
> single-machine install the quickest way to write is to run the command there:
>
> ```bash
> docker compose -f compose.server-managed.yaml exec -u aimee aimee-server \
>   aimee memory store my-key "some fact"
> ```
>
> For writes **from another machine**, that client's subject needs a write-tier
> grant — `data` for memory/index writes, `full` to also allow exec/control.
> Grants are keyed by `(server_id, team_id, subject)` and issued on the server:
>
> ```bash
> aimee kb grant set --subject <subject> --server <server-id> --team <team-id> --tier data
> aimee kb grant list --server <server-id> --team <team-id>      # confirm it landed
> ```
>
> [UPGRADING.md](UPGRADING.md) is the reference for the two parts that are easy to
> get wrong: how a subject is spelled for each login mode (a grant for the wrong
> spelling is silently a grant for nobody), and why the operator installs its own
> context with team `0`.

### 3.5 Add agents (delegates)

Delegates are cheaper/local models aimee routes routine work to. Configure them once:

```powershell
aimee agent setup <provider>          # OAuth/device-flow setup for subscription providers (e.g. chatgpt, mistral-plan)
aimee agent add <name> --provider openai --model <model> --api-key <key>   # direct API key
aimee agent local local http://YOUR_LLM_HOST:8080 --model MODEL --slots 4  # local OpenAI-compatible runtime (Ollama / llama.cpp)
aimee agent list                      # inspect registered agents + routing data
```

Agent/provider control commands are exec/control operations, so over the network they need a **write-tier grant of `full` for your subject** (see [1.4](#14-before-you-expose-it-on-a-network)). The primary agent (Claude Code, Codex, …) then routes delegateable work automatically; you can also call `aimee delegate <role> "<task>"` directly.

### 3.6 Interactive chat

`aimee acp-serve` and the web chat work against a remote server. Pointed at a remote `/v1` endpoint, the client registers your current directory as a **detached workspace** and opens a reverse channel: the agent runs on the server while its file and tool actions reach back into your local working tree (the reverse channel is supported on the Windows client). Run `aimee chat` from inside the repository you want the agent to work in.

Because `launch`/`chat` are exec/control operations, a remote session needs a **write-tier grant of `full` for your subject** (see [1.4](#14-before-you-expose-it-on-a-network)). You can also drive aimee from your AI coding tool (configured in [3.3](#33-configure-your-ai-coding-tool)), or use the browser webchat at `https://YOUR_SERVER:8443`.

---

## Part 4, macOS client

macOS uses the same **thin client** model as Linux and Windows: install `aimee`, point it at your Docker/Linux server, and set up workspaces and agents.

### 4.1 Install `aimee`

**Option A, prebuilt binary (no build):**

```bash
# Download aimee-macos-universal from the latest GitHub release, then:
chmod +x aimee-macos-universal
xattr -d com.apple.quarantine aimee-macos-universal 2>/dev/null || true   # clear Gatekeeper quarantine
mkdir -p ~/.local/bin
mv aimee-macos-universal ~/.local/bin/aimee
# ensure ~/.local/bin is on your PATH (add to ~/.zshrc if needed):
export PATH="$HOME/.local/bin:$PATH"
```

The prebuilt macOS binary is a universal (arm64 + x86_64) build and speaks `https://` natively via **Secure Transport**, evaluating trust against the **Keychain**, no OpenSSL and no bundled CA bundle. Set `AIMEE_TLS_INSECURE=1` for a self-signed dev cert.

**Option B, build the thin client from source** (needs Xcode Command Line Tools + CMake; TLS uses Secure Transport/Keychain, so no OpenSSL is required):

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
cmake -B build -DAIMEE_THIN_CLIENT=ON -DAIMEE_LEAN=ON \
      -DWITH_PAM=OFF -DWITH_LIBSECRET=OFF -DWITH_UI=OFF -DWITH_TLS=ON \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"   # drop for a host-arch-only build
cmake --build build --target aimee
cp build/aimee ~/.local/bin/aimee      # or anywhere on your PATH
```

(Want the full server + kb running locally on the Mac instead of in Docker? Run `./install-deps.sh` then `./install.sh`, that builds everything and registers launchd agents. For the client-against-Docker setup in this guide, the thin client above is all you need.)

Confirm the install:

```bash
aimee version
```

### 4.2 Point the client at your server

```bash
aimee remote set https://YOUR_SERVER:8743 aimee-local-dev
aimee remote status     # resolved transport + /v1/health probe
aimee status            # server, DB1, and knowledge-base health
```

Both the prebuilt universal binary and a source build speak `https://` with certificate verification on by default (Secure Transport against the Keychain); set `AIMEE_TLS_INSECURE=1` for a self-signed dev cert. As on the other platforms, you can use `AIMEE_SERVER_URL` / `AIMEE_SERVER_TOKEN` or `--server` instead of `aimee remote set`.

### 4.3 Configure your AI coding tool

aimee registers its hooks + MCP server into every detected tool (Claude Code, Codex CLI, Gemini CLI, GitHub Copilot) automatically on the first `aimee` command. To do it explicitly, run:

```bash
./configure-hooks.sh
```

Opt out of the global registration with `AIMEE_NO_CLIENT_INTEGRATIONS=1` (or `client_integrations_enabled: false` in `aimee.yaml`); per-project wiring via `aimee setup` still works.

### 4.4 Set up workspaces

```bash
aimee workspace add /path/to/your/repo   # register this host's checkout (see the note below on indexing)
aimee workspace list
```

You can also drop an `aimee.workspace.yaml` manifest in a directory and run `aimee setup` to clone, install dependencies, index, and generate starter rules for a multi-repo workspace in one shot, see [Workspace Management](WORKSPACES.md). As on the other platforms, indexing/memory writes require a **write-tier grant of at least `data`** for your subject.

### 4.5 Add agents (delegates)

```bash
aimee agent setup <provider>          # OAuth/device-flow for subscription providers
aimee agent add <name> --provider openai --model <model> --api-key <key>
aimee agent local local http://YOUR_LLM_HOST:8080 --model MODEL --slots 4   # local Ollama / llama.cpp
aimee agent list
```

Agent/provider control over the network requires a **write-tier grant of `full`** for your subject. See [Setting Up Delegates](DELEGATES.md) for the full agent schema and routing details.

### 4.6 Interactive chat

As on the other platforms, `aimee acp-serve` and the web chat work against a remote server: the client registers your current directory as a **detached workspace** and opens a reverse channel, so the agent runs server-side while its file and tool actions act on your local tree. Run `aimee chat` from inside the repository you want the agent to work in, with a **write-tier grant of `full`** for your subject (see [1.4](#14-before-you-expose-it-on-a-network)). You can also drive aimee from your AI coding tool, or use the browser webchat at `https://YOUR_SERVER:8443`.

To verify that the client sends local document bytes to the remote KB—the server
does not need access to the client's filesystem—stage a small document twice and
confirm the second upload is deduplicated:

```bash
aimee kb docs push --scope onboarding README.md
aimee kb docs push --scope onboarding README.md # reports the document skipped
aimee memory store onboarding.check "remote memory is working" --tier L2 --kind fact
aimee memory search "remote memory"
```

`kb docs push` stages corpus documents for review/release processing. Its upload
counter and second-run skip counter validate transport and content deduplication;
staging does not make a document immediately searchable.

---

## Where things live

| Path | What |
|------|------|
| `<aimee_home>/remote.conf` | Persisted thin-client remote target (`aimee remote set`). `aimee_home` is `~/.config/aimee` on Linux/macOS; `%LOCALAPPDATA%\aimee` on Windows. |
| `aimee.yaml` | Server config (bearer, `remote_writes`, kb wiring, agents). In Docker this is inside the `*-home` volume at `/var/lib/aimee/aimee.yaml`. |
| Named Docker volumes | `*-kb-home` (KB + embedded DB2), `*-server-home`, and `*-workspaces`. A downloadable LLM service also uses `*-llm-models` for its GGUF cache. |

## TLS support by build

Every supported client now speaks `https://`, each using its platform's native trust store, no bundled CA bundle. Certificate verification is on by default; set `AIMEE_TLS_INSECURE=1` to skip it for a self-signed dev cert.

| Client | `https://` support | TLS backend / trust store |
|--------|--------------------|---------------------------|
| Linux, prebuilt release binary or `make` build | Yes | OpenSSL, system trust store |
| macOS, prebuilt `aimee-macos-universal` or source build | Yes | Secure Transport, Keychain |
| Windows, prebuilt or `install.ps1` build | Yes | Schannel, Windows certificate store |

For per-client cryptographic identity (beyond a shared bearer), the server also supports **mTLS client certificates**, clients present a cert from `<aimee_home>/tls/client.{crt,key}` and the server maps it to a `cert:<CN>` principal. Issue and manage them with the `aimee cert` CLI (`/v1/cert/issue|list|revoke`); see the [Manual](../MANUAL.md) for setup.

## Next steps

- **[Manual](../MANUAL.md)**, the complete command and configuration reference.
- **[Workspace Management](WORKSPACES.md)**, manifests, multi-repo workspaces, session isolation.
- **[Setting Up Delegates](DELEGATES.md)**, configure delegate agents and routing.
- **[Architecture](ARCHITECTURE.md)**, processes, storage boundaries, and deployment topologies.
- **Run on any model**, point Claude Code (`aimee claude-proxy enable`), Codex, or any OpenAI-compatible tool at aimee and run every turn on your chosen primary model; see the [README](../README.md#any-model-behind-your-front-end).
- **Get help**, join the official aimee Discord at <https://discord.gg/FjGjvcgAqz> for questions and discussion.
