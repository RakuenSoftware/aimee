# aimee Quickstart

This guide takes you from nothing to a working aimee install in four parts:

1. **[Run the server](#part-1--run-the-server-aimee-combined-in-docker)**, stand up the full stack (server + knowledge base + Postgres, with a bundled CPU inference gateway) with the combined Docker image.
2. **[Install the Linux client](#part-2--linux-client)**, install the thin `aimee` binary, point it at your server, and set up workspaces and agents.
3. **[Install the Windows client](#part-3--windows-client)**, same, for Windows.
4. **[Install the macOS client](#part-4--macos-client)**, same, for macOS.

The model is the same on every developer machine: **the services run in Docker (or on a Linux/macOS host); each developer installs only the thin `aimee` client and points it at the server.** The client holds no database, it talks to the server over the `/v1` HTTP API.

---

## Part 1, Run the server (aimee-combined in Docker)

The **combined** image co-locates both aimee binaries in one container: the knowledge base (`aimee-kb`) on loopback `:8741` inside the container, and the server (`aimee-server`) fronting `/v1` over native TLS on `:8743` (self-signed cert; plaintext `:8740` is loopback-only and not published). Postgres (DB2 + pgvector) comes up alongside it as a separate service; the CPU inference gateway (embeddings, reranking, synthesis) is bundled in the image.

### Prerequisites

- Docker Engine + the Docker Compose plugin (`docker compose`, v2).
- ~8 GB free RAM and ~10 GB of disk. The combined image bakes in ~5.5 GB of CPU inference model weights (embed, rerank, synth) on top of Postgres data and build layers. (Build `--build-arg WITH_LLM=0` for a lean server+kb image that points at an external `AIMEE_LLM_URL` instead.)
- No credentials or API keys are required for the default build.

### 1.1 Clone and start the stack

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
docker compose -f compose.combined.yaml up --build -d
```

By default this brings up **two** services. The browser webchat runs *inside* the combined container, not as a separate service, and the combined image bundles a CPU inference gateway (embeddings, reranking, and synthesis), so nothing external is needed to start.

| Service | What it is | Port |
|---------|-----------|------|
| `aimee-server-kb` | Both aimee binaries (server and kb), the browser webchat UI, and a bundled CPU inference gateway (embed, rerank, synth) in one container | `8743` (server `/v1`, native TLS self signed; plaintext `8740` loopback only), `8741` (kb `/v1`), `8443` (webchat HTTPS, self signed) |
| `postgres` | `pgvector/pgvector:pg16`, DB2 (`aimee_shared`) for knowledge and vectors | internal |
| `llm` *(optional)* | Standalone curator LLM sidecar (Gemma 3n E4B via `llama.cpp`). Off by default, since the bundled gateway already serves embed, rerank, and synth. Enable it with `--profile external-llm` for a lean `WITH_LLM=0` image, or to bring your own curator GGUF (point `LLM_ENDPOINT` at it). | internal |

The kb auto-applies its DB2 schema (tables plus the `pg_trgm` and `vector` extensions) on first boot. The first `--build` takes a few minutes to compile the binaries; the CPU inference model weights are pulled in prebuilt (baked into the image), not downloaded at boot, so later starts are fast. To also run the standalone curator LLM sidecar (for a lean `WITH_LLM=0` image or a custom curator GGUF), add the profile: `docker compose -f compose.combined.yaml --profile external-llm up --build -d`.

### 1.2 Verify it's healthy

```bash
docker compose -f compose.combined.yaml ps          # all services should be "healthy"

# Server /v1 over TLS (default bearer is "aimee-local-dev"; -k accepts the self-signed cert):
curl -k -H 'Authorization: Bearer aimee-local-dev' https://localhost:8743/v1/health
curl -k -H 'Authorization: Bearer aimee-local-dev' https://localhost:8743/v1/kb/status

# In-container kb directly (DB + pgvector status):
curl 'http://localhost:8741/v1/health?status=1'
```

If the server endpoints return `200` and `kb/status` reports the DB and vector store ready, the stack is up.

### 1.3 Browser webchat

The browser UI is **on by default**. Open **https://localhost:8443** (accept the self-signed cert) and log in with the default account `aimee` / `aimee-local-dev`. Change `AIMEE_WEBCHAT_USER` / `AIMEE_WEBCHAT_PASSWORD` for anything beyond local dev, or set `AIMEE_WEBCHAT_ENABLED=0` in the compose file to turn it off.

> Webchat is built into the image by default, from both published images and a from-source `docker compose build`, its frontend dependency (`@rakuensoftware/smoothgui`) is vendored in-repo, so the build needs no credentials. Build with `--build-arg WITH_WEBCHAT=0` to ship the server+kb services only.

### 1.4 Before you expose it on a network

- **Override the default bearer.** `aimee-local-dev` is a loopback convenience only. Mount your own `aimee.yaml` at `/var/lib/aimee/aimee.yaml` with a real bearer (see `compose.remote-writes.combined.yaml` for the pattern) and terminate TLS at a reverse proxy.
- **Remote writes are off by default.** Over the network a remote bearer is **read/query only** until you opt in. To let remote clients write memory, run the index, etc., set `aimee.api.remote_writes` in your mounted `aimee.yaml`:
  - `data`, allow data-plane writes (`memory store`, `work …`, `rules …`, `skill …`).
  - `full`, also allow exec/control (`delegate`, `agent`, `provider`, `cron`). **Trusted networks only**, a leaked `full` bearer permits remote code execution.

  (Workspace registration over the network, `workspace add/serve/remove`, is a deliberate, bearer-gated exception and works even with remote writes off.)

### 1.5 Managing the stack

```bash
docker compose -f compose.combined.yaml logs -f                 # follow logs
docker compose -f compose.combined.yaml restart aimee-server-kb # restart just the aimee container
docker compose -f compose.combined.yaml down                    # stop + remove containers (named volumes persist)
docker compose -f compose.combined.yaml down -v                 # also DROP data volumes (DESTROYS DB2 + state)
docker compose -f compose.combined.yaml pull && \
  docker compose -f compose.combined.yaml up -d --build         # update: rebuild and recreate
```

Durable state lives in named volumes (`*-postgres`, `*-home`, `*-workspaces`), so `down` and recreate are safe; only `down -v` erases data. (The CPU inference model weights are baked into the image, not a volume.)

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

**Option B, build the thin client from source** (needs a C compiler, `make`, and `libsqlite3-dev`; add `libssl-dev` to keep `https://` support):

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
aimee remote set https://YOUR_SERVER:8743 aimee-local-dev
aimee remote status     # resolved transport + /v1/health probe
aimee status            # server, DB1, and kb health
```

Use your real bearer token instead of `aimee-local-dev` if you changed it. The server's `/v1` is TLS-only off-loopback; certificate verification is on by default, so set `AIMEE_TLS_INSECURE=1` for the auto-provisioned self-signed cert (or trust/pin it). Alternatives to `aimee remote set`: set `AIMEE_SERVER_URL` / `AIMEE_SERVER_TOKEN`, or pass `--server https://YOUR_SERVER:8743 --server-token=...` per command. Precedence is `--server` flag > env > persisted `remote.conf`.

### 2.3 Configure your AI coding tool

From the cloned checkout, register aimee's hooks and MCP server for every detected tool (Claude Code, Codex CLI, Gemini CLI, GitHub Copilot):

```bash
./configure-hooks.sh
```

This wires SessionStart / PreToolUse / PostToolUse hooks (which call `aimee`) and the `aimee mcp-serve` MCP server into each tool's config.

### 2.4 Set up workspaces

A workspace is a set of repositories aimee indexes and works across as one unit.

```bash
aimee workspace add /path/to/your/repo                     # register an existing checkout and index it
aimee workspace add --repo git@github.com:org/repo.git     # clone, register, and index
aimee workspace list                                       # list roots and the projects under each
```

You can also drop an `aimee.workspace.yaml` manifest in a directory and run `aimee setup` to clone, install dependencies, index, and generate starter rules for a multi-repo workspace in one shot, see [Workspace Management](WORKSPACES.md).

> Indexing and memory writes are server-side mutations, so they need the server's `aimee.api.remote_writes` to be at least `data` (see [1.4](#14-before-you-expose-it-on-a-network)). Workspace registration itself works regardless.

### 2.5 Add agents (delegates)

Delegates are cheaper/local models aimee routes routine work to. Configure them once:

```bash
aimee agent setup <provider>          # OAuth/device-flow setup for subscription providers (e.g. chatgpt, mistral-plan)
aimee agent add <name> --provider openai --model <model> --api-key <key>   # direct API key
aimee agent local local http://YOUR_LLM_HOST:8080 --model MODEL --slots 4  # local OpenAI-compatible runtime (Ollama / llama.cpp)
aimee agent list                      # inspect registered agents + routing data
```

Agent/provider control commands are exec/control operations, so over the network they need the server's `remote_writes` set to `full`. The primary agent (Claude Code, Codex, …) then routes delegateable work automatically; you can also call `aimee delegate <role> "<task>"` directly. See [Setting Up Delegates](DELEGATES.md) for the full agent schema and routing details.

### 2.6 Interactive chat

`aimee chat` and `aimee launch` work against a remote server. When the client is pointed at a remote `/v1` endpoint, it registers your current directory as a **detached workspace** and opens a reverse channel back to it: the agent loop runs on the server, and its file and tool actions reach back into your local working tree over that channel. The client still holds no engine and no database, it renders the session and serves its own tree. Run `aimee chat` from inside the repository you want the agent to work in.

Because `launch`/`chat` are exec/control operations, the server's `aimee.api.remote_writes` must be set to `full` for a remote session (see [1.4](#14-before-you-expose-it-on-a-network)). You can also drive aimee from your AI coding tool (configured in [2.3](#23-configure-your-ai-coding-tool)), or use the browser webchat at `https://YOUR_SERVER:8443`.

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
aimee status            # server, DB1, and kb health
```

Use your real bearer token instead of `aimee-local-dev` if you changed it. The server's `/v1` is TLS-only off-loopback; `https://` works with certificate verification on by default (Schannel against the Windows cert store), so set `AIMEE_TLS_INSECURE=1` for the auto-provisioned self-signed cert (or trust/pin it). Alternatives to `aimee remote set`: set `AIMEE_SERVER_URL` / `AIMEE_SERVER_TOKEN` environment variables, or pass `--server https://YOUR_SERVER:8743 --server-token=...` per command. Precedence is `--server` flag > env > persisted `remote.conf`.

### 3.3 Configure your AI coding tool

From the cloned checkout, register aimee's hooks and MCP server for every detected tool (Claude Code, Codex CLI, Gemini CLI, GitHub Copilot):

```powershell
.\configure-hooks.ps1
```

This wires SessionStart / PreToolUse / PostToolUse hooks (which call `aimee.exe`) and the `aimee mcp-serve` MCP server into each tool's config.

### 3.4 Set up workspaces

A workspace is a set of repositories aimee indexes and works across as one unit.

```powershell
aimee workspace add C:\path\to\your\repo        # register an existing checkout and index it
aimee workspace add --repo https://github.com/org/repo.git   # clone, register, and index
aimee workspace list                            # list roots and the projects under each
```

> Indexing and memory writes are server-side mutations, so they need the server's `aimee.api.remote_writes` to be at least `data` (see [1.4](#14-before-you-expose-it-on-a-network)). Workspace registration itself works regardless.

### 3.5 Add agents (delegates)

Delegates are cheaper/local models aimee routes routine work to. Configure them once:

```powershell
aimee agent setup <provider>          # OAuth/device-flow setup for subscription providers (e.g. chatgpt, mistral-plan)
aimee agent add <name> --provider openai --model <model> --api-key <key>   # direct API key
aimee agent local local http://YOUR_LLM_HOST:8080 --model MODEL --slots 4  # local OpenAI-compatible runtime (Ollama / llama.cpp)
aimee agent list                      # inspect registered agents + routing data
```

Agent/provider control commands are exec/control operations, so over the network they need the server's `remote_writes` set to `full`. The primary agent (Claude Code, Codex, …) then routes delegateable work automatically; you can also call `aimee delegate <role> "<task>"` directly.

### 3.6 Interactive chat

`aimee chat` and `aimee launch` work against a remote server. Pointed at a remote `/v1` endpoint, the client registers your current directory as a **detached workspace** and opens a reverse channel: the agent runs on the server while its file and tool actions reach back into your local working tree (the reverse channel is supported on the Windows client). Run `aimee chat` from inside the repository you want the agent to work in.

Because `launch`/`chat` are exec/control operations, the server's `aimee.api.remote_writes` must be `full` for a remote session (see [1.4](#14-before-you-expose-it-on-a-network)). You can also drive aimee from your AI coding tool (configured in [3.3](#33-configure-your-ai-coding-tool)), or use the browser webchat at `https://YOUR_SERVER:8443`.

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
aimee status            # server, DB1, and kb health
```

Both the prebuilt universal binary and a source build speak `https://` with certificate verification on by default (Secure Transport against the Keychain); set `AIMEE_TLS_INSECURE=1` for a self-signed dev cert. As on the other platforms, you can use `AIMEE_SERVER_URL` / `AIMEE_SERVER_TOKEN` or `--server` instead of `aimee remote set`.

### 4.3 Configure your AI coding tool

```bash
./configure-hooks.sh
```

Registers aimee's hooks + MCP server for every detected tool (Claude Code, Codex CLI, Gemini CLI, GitHub Copilot).

### 4.4 Set up workspaces

```bash
aimee workspace add /path/to/your/repo                     # register an existing checkout and index it
aimee workspace add --repo git@github.com:org/repo.git     # clone, register, and index
aimee workspace list
```

You can also drop an `aimee.workspace.yaml` manifest in a directory and run `aimee setup` to clone, install dependencies, index, and generate starter rules for a multi-repo workspace in one shot, see [Workspace Management](WORKSPACES.md). As on the other platforms, indexing/memory writes require the server's `remote_writes` to be at least `data`.

### 4.5 Add agents (delegates)

```bash
aimee agent setup <provider>          # OAuth/device-flow for subscription providers
aimee agent add <name> --provider openai --model <model> --api-key <key>
aimee agent local local http://YOUR_LLM_HOST:8080 --model MODEL --slots 4   # local Ollama / llama.cpp
aimee agent list
```

Agent/provider control over the network requires the server's `remote_writes` to be `full`. See [Setting Up Delegates](DELEGATES.md) for the full agent schema and routing details.

### 4.6 Interactive chat

As on the other platforms, `aimee chat` and `aimee launch` work against a remote server: the client registers your current directory as a **detached workspace** and opens a reverse channel, so the agent runs server-side while its file and tool actions act on your local tree. Run `aimee chat` from inside the repository you want the agent to work in, with the server's `aimee.api.remote_writes` set to `full` (see [1.4](#14-before-you-expose-it-on-a-network)). You can also drive aimee from your AI coding tool, or use the browser webchat at `https://YOUR_SERVER:8443`.

---

## Where things live

| Path | What |
|------|------|
| `<aimee_home>/remote.conf` | Persisted thin-client remote target (`aimee remote set`). `aimee_home` is `~/.config/aimee` on Linux/macOS; `%LOCALAPPDATA%\aimee` on Windows. |
| `aimee.yaml` | Server config (bearer, `remote_writes`, kb wiring, agents). In Docker this is inside the `*-home` volume at `/var/lib/aimee/aimee.yaml`. |
| Named Docker volumes | `*-postgres` (DB2), `*-home` (server/kb state), `*-workspaces`. (The `--profile external-llm` sidecar adds `aimee-llm-models` for its GGUF cache.) |

## TLS support by build

Every supported client now speaks `https://`, each using its platform's native trust store, no bundled CA bundle. Certificate verification is on by default; set `AIMEE_TLS_INSECURE=1` to skip it for a self-signed dev cert.

| Client | `https://` support | TLS backend / trust store |
|--------|--------------------|---------------------------|
| Linux, prebuilt release binary or `make` build | Yes | OpenSSL, system trust store |
| macOS, prebuilt `aimee-macos-universal` or source build | Yes | Secure Transport, Keychain |
| Windows, prebuilt or `install.ps1` build | Yes | Schannel, Windows certificate store |

For per-client cryptographic identity (not just a shared bearer), the server also supports **mTLS client certificates**, clients present a cert from `<aimee_home>/tls/client.{crt,key}` and the server maps it to a `cert:<CN>` principal. Issue and manage them with the `aimee cert` CLI (`/v1/cert/issue|list|revoke`); see the [Manual](../MANUAL.md) for setup.

## Next steps

- **[Manual](../MANUAL.md)**, the complete command and configuration reference.
- **[Workspace Management](WORKSPACES.md)**, manifests, multi-repo workspaces, session isolation.
- **[Setting Up Delegates](DELEGATES.md)**, configure delegate agents and routing.
- **[Architecture](ARCHITECTURE.md)**, processes, storage boundaries, and deployment topologies.
- **Run on any model**, point Claude Code (`aimee claude-proxy enable`), Codex, or any OpenAI-compatible tool at aimee and run every turn on your chosen primary model; see the [README](../README.md#any-model-behind-your-front-end).
- **Get help**, join the official aimee Discord at <https://discord.gg/FjGjvcgAqz> for questions and discussion.
