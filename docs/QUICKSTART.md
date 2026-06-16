# Quickstart

Zero to working in a few minutes. Run the server in Docker, install the thin
client, point your AI tool at it. Memory, guardrails, and delegation come on
automatically.

For the full picture, see the [Manual](../MANUAL.md) and
[How aimee learns](KNOWLEDGE.md).

## 1. Run the server

One container runs both binaries — the server and the knowledge base. Postgres
and the embedder come up alongside it.

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
docker compose -f compose.combined.yaml up --build -d
```

The server fronts `/v1` on `:8740`; the in-container kb runs on `:8741`. Default
bearer is `aimee-local-dev`. Confirm it's live:

```bash
curl -H 'Authorization: Bearer aimee-local-dev' http://localhost:8740/v1/health
curl -H 'Authorization: Bearer aimee-local-dev' http://localhost:8740/v1/kb/status
```

Set a real bearer for anything past loopback. `aimee-local-dev` is a dev
convenience.

The default embedder is the 0.6B (`pplx-embed-v1-0.6b`, 1024-dim), paired with the
400m reranker — the low-latency tier. For higher fidelity, use the 4B:
`AIMEE_EMBEDDER_IMAGE=ghcr.io/rakuensoftware/aimee-embedder-4b:latest AIMEE_EMBEDDING_DIM=2560 docker compose -f compose.combined.yaml up --build -d`.

## 2. Install the client

The `aimee` CLI is a thin client. It holds no database and talks to a server
over `/v1`. Grab a prebuilt binary from the latest
[release](https://github.com/RakuenSoftware/aimee/releases) (Linux, macOS,
Windows), put it on your `PATH`, done.

Point it at the server:

```bash
aimee remote set http://localhost:8740 aimee-local-dev
aimee remote status   # resolved transport + a health probe
```

Or per-invocation: `aimee --server http://host:8740 --server-token=aimee-local-dev status`.
Or via env: `AIMEE_SERVER_URL` / `AIMEE_SERVER_TOKEN`.

## 3. Wire your AI tool

From a checkout, run the hook installer on the client machine:

```bash
./configure-hooks.sh        # configure-hooks.ps1 on Windows
```

It registers aimee's SessionStart/PreToolUse/PostToolUse hooks and MCP server for
every tool it finds — Claude Code, Codex CLI, Gemini CLI, Copilot. From here,
every session starts knowing what the last one learned, sensitive files are
blocked before a write lands, and you can hand work to cheaper models.

The host AI's own sub-agent tool (Claude's `Task`, Codex's `spawn_agent`) is
blocked on purpose — fan work out through `aimee delegate` instead, so it stays
in aimee's session state, cost accounting, and audit trail. See
[Delegates](DELEGATES.md).

To run your tool's front end on aimee's model instead of its built-in vendor:

```bash
# Claude Code on any model:
ANTHROPIC_BASE_URL=http://localhost:8740 ANTHROPIC_AUTH_TOKEN=aimee-local-dev claude
```

Codex and any OpenAI-compatible client work the same way — see the
[README](../README.md#use-your-front-end-on-any-model).

## 4. Verify

```bash
aimee version
aimee status   # server, DB1, and kb health
```

## 5. Use it

```bash
# Remember something across every future session
aimee memory store db-host "PostgreSQL at 10.0.0.5:5432" --tier L2 --kind fact
aimee memory search "database"

# Hand routine work to a cheaper model
aimee delegate review "Review this PR for security issues"
aimee delegate code --tools "Add tests for the auth module"

# Convene a panel of models and synthesize one answer
aimee delegate roundtable --mode review "Is this migration plan sound?"
```

Delegates and the roundtable panel ship configured out of the box. You don't set
them up. See [Setting Up Delegates](DELEGATES.md) to add your own providers or
change the panel.

## Next

- [Manual](../MANUAL.md) — full command contract and configuration.
- [How aimee learns](KNOWLEDGE.md) — the knowledge base and where this goes at team scale.
- [Workspaces](WORKSPACES.md) — multi-repo and session isolation.
- [Security](SECURITY.md) — trust boundaries and the capability model.
