# Thin Client: Workspaces, Agents & Client-Held Credentials

This document describes how the `aimee` thin client works against a **remote**
`aimee-server` (e.g. a shared server reached over `tcp:`), covering three things
that are deliberately designed so the *client machine*, not the server, holds
the working tree and the secrets:

1. **Workspaces** are ingested *from the client* (the server never reads the
   client's filesystem).
2. **Agent/delegate API keys live on the client** and are pushed to a
   **RAM-only, per-session** keyring on the server that is **never persisted to
   disk**.
3. The **attention guard** is inert by default.

It complements [WORKSPACES.md](WORKSPACES.md), [DELEGATES.md](DELEGATES.md), and
[SECURITY.md](SECURITY.md).

## What runs where

| Concern | Thin client (your machine) | aimee-server (remote) |
| --- | --- | --- |
| Your working tree / files | yes | no (never reads client fs) |
| Agent API keys / Codex OAuth | stored here | cached in RAM per session, never on disk |
| Engine, agent loop, DB1/DB2, KB |, | yes |
| Code index, memory, chat/delegate execution |, | yes |

Point the client at the server once:

```sh
aimee remote set http://SERVER:8740 <bearer-token>
# or per-invocation: --server / AIMEE_SERVER_URL (+ --server-token / AIMEE_SERVER_TOKEN)
```

A remote endpoint is "tcp" (`http(s)://host:port`) vs. a co-located unix socket.
The behaviors below activate only for a **remote tcp** endpoint; co-located use
is unchanged.

### Server write posture

Mutating `/v1` calls require the server to allow remote writes. Set it in the
server's `aimee.yaml` (`aimee.api.remote_writes: off|data|full`) **or** via the
`AIMEE_API_REMOTE_WRITES` environment variable (deploy truth, applied even when
the config file is read-only or absent, e.g. a containerized server). `full`
grants the LAN bearer the capabilities needed for workspace add, ingest, and
session-credential push; use it only on trusted networks.

## Workspaces (client-push ingest)

On a thin client the workspace root lives on *your* machine, which the server
cannot read. `aimee workspace add <path>` therefore:

1. resolves `<path>` locally,
2. registers it on the server as a **`detached`** workspace (the server stores
   the path verbatim and does not try to scan its own filesystem),
3. collects the source files locally and pushes them to `POST /v1/index/ingest`,
   which relays them to aimee-kb's code index.

```sh
aimee workspace add ~/dev          # register + ingest from this machine
aimee workspace list               # shows dev as [detached] + indexed projects
aimee index scan [path]            # re-push one path, or every detached workspace
aimee index find <symbol>          # query the code index
```

Notes:
- The push is **chunked** to stay under aimee-kb's 1 MB request-body cap; large
  trees are split across several batches.
- A single `workspace add` collects up to `CODE_COLLECT_MAX_FILES` (4096) files
  and logs when a tree is truncated; re-run `index scan` from the client to
  re-push after changes.
- VCS/build/hidden directories (`.git`, `node_modules`, `target`, `dist`, …) and
  binary/oversized files are skipped.

## Agents & client-held credentials

API keys for delegates and the primary provider are **held on the client**, not
stored on the server. The model:

- `agent add` stores the **definition** (name, endpoint, model, roles, provider)
  on the server; the **key** stays local.
- Once per session the client pushes its keys to the server's in-memory keyring;
  the server uses them for that session's turns and **never writes them to
  disk** (they evaporate on session end / restart). See [SECURITY.md](SECURITY.md).

### Adding an agent

```sh
aimee agent add minimax https://api.minimax.io/v1/chat/completions MiniMax-M3 \
      --key "$MINIMAX_KEY" --provider openai --default \
      --roles "code,review,explain,refactor,draft,execute,summarize,format,diagnose,validate"
```

Against a **remote tcp** server, `--key K`:
- is written to the local keyring `~/.config/aimee/agent-keys.json` (mode 0600),
  keyed by the agent name, and
- is **stripped** from the request before the definition is forwarded, the key
  never reaches the server.

Set the primary chat provider to any configured agent:

```sh
aimee config set provider minimax
```

### How keys reach the server (per session)

- A stable per-client **credential-session id** is generated once into
  `~/.config/aimee/cred-session.id`.
- The first chat/delegate turn of a session pushes the local keyring (and Codex
  creds, below) to `POST /v1/session/credentials`, deduplicated (re-pushes at
  most every ~2 minutes, which also re-syncs after a server restart). Each turn
  carries a `cred_session_id` so the server resolves the right keyring entry,
  decoupled from the functional chat session id.
- Server auth resolution prefers a client-pushed session key (by session + agent
  name) over any server-stored `api_key` / provider env var.

You set up agents **per machine** you use, that is the intended trade-off for
not centralizing keys on the server.

### Codex (ChatGPT OAuth)

The Codex CLI keeps a refreshed OAuth token in `~/.codex/auth.json` on your
machine. Add a Codex agent with no key, the client reads that file and pushes
the token (and ChatGPT account id) with the session credentials:

```sh
aimee agent add codex https://chatgpt.com/backend-api/codex gpt-5.5 \
      --provider codex --roles "code,review,explain,refactor,draft,execute,summarize,format,diagnose,validate"
aimee config set provider codex     # optional: use Codex as the primary
```

`--provider codex` is a convenience alias for the Codex adapter (provider
`chatgpt`, `auth_type` `codex-oauth`, the responses-wire delegate driver). The
token is sourced live per session, so it stays fresh as the Codex CLI refreshes
it; it is never stored on the server.

## Attention guard

The PreToolUse attention guard (`aimee attention-guard`, used by the Claude Code
integration) is **inert by default**: recursive raw scans (`grep -r`, `Grep`,
`Glob`, …) are allowed unless you set a positive `ingress_max_raw_scans` cap in
`aimee.yaml`, after which a session may run that many raw scans before further
ones are redirected toward the indexed tools. The destructive-file guard (it
blocks a hard-destructive command on a file the session has actively edited) is
always on. `AIMEE_GUARD=0` disables the guard entirely.

The client's Claude Code integration (MCP server + hooks) is registered at the
**installed** binary path and self-heals: running the installed client rewrites
any stale hook/MCP command (e.g. one left pointing at an old build directory) to
the resolved binary.

## New `/v1` endpoints

| Method | Endpoint | Purpose |
| --- | --- | --- |
| POST | `/v1/index/ingest` | Index client-pushed `{rel_path, content}` files for a workspace the server cannot see (relays to aimee-kb). |
| POST | `/v1/session/credentials` | Receive the once-per-session push of `{session_id, agents{}, codex_oauth_token, codex_account_id}` into the RAM-only keyring. |

Both `/v1/index/scan` and `/v1/index/ingest` run as synchronous handlers under
the async op-run worker (poll `GET /v1/runs/{id}`).
