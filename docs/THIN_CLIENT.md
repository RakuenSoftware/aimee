# Thin Client: Workspaces, Agents & Credentials

This document describes how the `aimee` thin client works against a **remote**
`aimee-server` (e.g. a shared server reached over `tcp:`), covering three things
specific to remote operation:

1. **Workspaces** are ingested *from the client* (the server never reads the
   client's filesystem), the client still owns the working tree.
2. **Agent/delegate credentials live in the server's sealed vault**, encrypted
   at rest and decryptable by the server autonomously. They are **not** held on
   the client and there is **no** RAM session keyring (the legacy client-held
   keyring + per-session push were retired; the vault is the single store).
3. The **attention guard** is inert by default.

It complements [WORKSPACES.md](WORKSPACES.md), [DELEGATES.md](DELEGATES.md), and
[SECURITY.md](SECURITY.md).

## What runs where

| Concern | Thin client (your machine) | aimee-server (remote) |
| --- | --- | --- |
| Your working tree / files | yes | no (never reads client fs) |
| Agent API keys / Codex OAuth | not stored here | **sealed vault**, encrypted at rest |
| Interactive CLI logins (Claude CLI, Codex CLI) | login lives here | runs against the client's login (see below) |
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
grants the LAN bearer the capabilities needed for workspace add and ingest; use
it only on trusted networks.

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

## Agents & credentials (server vault)

Credentials for delegates and the primary provider, API keys and Codex/OAuth
tokens, live in the server's **sealed vault**: encrypted at rest, keyed by
agent, and decryptable by the server autonomously (a dual-access wrap lets the
server unseal them without an interactive unlock). They are **not** held on the
client, and there is **no** per-session RAM keyring or credential push. The
vault is the single, permanent store. See [SECURITY.md](SECURITY.md).

- `agent add` stores the **definition** (name, endpoint, model, roles, provider)
  and, with `--key`, seals the **key into the vault** under the server principal.
  Plaintext key storage is refused, the key only ever lands encrypted.
- Every chat/delegate turn resolves the agent's credential from the vault (the
  in-flight turn's attested principal, falling back to the server principal). No
  credential is pushed per session and none is cached on the client.

### Adding an agent

```sh
aimee agent add minimax https://api.minimax.io/v1/chat/completions MiniMax-M3 \
      --key "$MINIMAX_KEY" --provider openai --default \
      --roles "code,review,explain,refactor,draft,execute,summarize,format,diagnose,validate"
```

`--key K` sends `K` to the server once, where it is sealed into the vault; it is
never stored on the client and never written to disk in plaintext. Set the
primary chat provider to any configured agent:

```sh
aimee config set provider minimax
```

You configure agents **once on the server**, the vault is shared across every
client that reaches it, so there is no per-machine key setup.

### Migrating legacy client-held keys

If an older client left keys in `~/.config/aimee/agent-keys.json`, move them into
the vault:

```sh
aimee agent key import            # vault each leftover key (preview with --dry-run)
aimee agent key import --scrub     # …and delete each from agent-keys.json once vaulted
```

This is the **only** remaining use of `agent-keys.json`; new keys go straight to
the vault. Run it on the server host over the Unix socket, or from a connection
holding the `vault:write:server` capability.

### Codex (ChatGPT OAuth)

Codex/OAuth tokens are stored in the vault too, so a Codex agent authenticates
server-side with no per-session push from the client. Use the server-hosted
OAuth setup, which installs the CLI and seals the token into the vault:

```sh
aimee agent setup codex-oauth       # server-hosted OAuth → token sealed in the vault
aimee config set provider codex      # optional: use Codex as the primary
```

`provider codex` is the Codex adapter (provider `chatgpt`, `auth_type`
`codex-oauth`, the responses-wire delegate driver). A legacy plaintext token
(from an older db1/secrets store) is migrated into the vault and scrubbed on
first use.

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

Both `/v1/index/scan` and `/v1/index/ingest` run as synchronous handlers under
the async op-run worker (poll `GET /v1/runs/{id}`).
