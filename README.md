# aimee

Your AI coding tool forgets everything between sessions, and you pay full price for the routine
work. aimee is a local server that fixes both. It holds memory and a map of your code across tools,
routes cheap work to cheap models, and enforces guardrails the model cannot write past.

Point any tool at it. Your context follows you between them.

Two services:

- **aimee-server** assists one human. Sessions, tools, credentials, delegates, workflows.
- **aimee-kb** serves a corpus, team, or company. Durable knowledge, code indexes, retrieval,
  curation.

The `aimee` CLI is a thin client for Linux, macOS, and Windows. Core services are C, the workflow
control plane and browser are Go, and nothing phones home.

## What you get

- **Memory that survives the session.** The curator extracts facts, joins evidence, catches
  contradictions, and lets stale detail decay. See [Knowledge](docs/KNOWLEDGE.md).
- **Your code as a graph.** Symbols, callers, imports, and cross-repo dependencies feed search and
  blast-radius checks. See [Code intelligence](docs/CODE_INTELLIGENCE.md).
- **Delegates that cut the bill.** Send review, diagnosis, and routine implementation to the
  cheapest model that fits the role. See [Delegates](docs/DELEGATES.md).
- **Guardrails before execution.** Secret paths, unsafe writes, worktree escapes, and untrusted MCP
  packages are checked first, and delegate sandboxes run with no network and no credentials. See
  [Security](docs/SECURITY.md).
- **One bus, one audit trail.** Every governed action, memory write, guardrail decision and vault
  read crosses one sequenced tap into a WORM ledger. See [Event bus](docs/EVENT_BUS.md).
- **Any provider.** OpenAI, Anthropic, Gemini, Mistral, Bedrock, and local OpenAI-compatible servers
  pass through one internal request format. Switch models without switching tools.

## Start

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
docker compose -f compose.server-managed.yaml up -d
docker compose -f compose.server-managed.yaml logs aimee-server
```

The log prints a generated, one-time dashboard login. Open <https://localhost:8443>, sign in, and
the setup wizard covers the account, provider, knowledge base, deployment, and workspaces. After the
numbered steps, the summary can start `aimee-kb` with PostgreSQL 18, pgvector, and pgvectorscale
inside the container.

The managed compose file mounts the Docker socket, which gives aimee-server control of the host
Docker daemon. Use the split stack if you do not want that.

The [Quickstart](docs/QUICKSTART.md) has the rest: choosing your own login, the split stack, thin
client enrollment, and what to check when it does not work.

## Docs

Start at the [documentation index](docs/README.md).

| Document | Use it for |
|----------|------------|
| [Quickstart](docs/QUICKSTART.md) | Install, enroll, verify. |
| [What's new](docs/WHATS_NEW.md) | Everything 0.4.0 changed, and what it removed. |
| [Upgrading](docs/UPGRADING.md) | Move from v0.2.192. One-way, so read it first. |
| [Manual](MANUAL.md) | Day-to-day use and operations. |
| [Architecture](docs/ARCHITECTURE.md) | Processes, storage, trust, request flow. |
| [Deployment](docs/DEPLOYMENT.md) | Managed, split, external DB2, backup, hardening. |
| [Command reference](docs/gen/cli-commands.md) | Every CLI command. Generated from source. |
| [Configuration reference](docs/gen/configuration.md) | Every config key and variable. Generated from source. |
| [Server API](docs/PUBLIC_API.md) | `/v1` transport, auth, compatibility. |
| [Feature status](docs/STATUS.md) | What works, what is gated, what was removed. |
| [Troubleshooting](docs/TROUBLESHOOTING.md) | Diagnose the first broken boundary. |

## Community

Questions and discussion: <https://discord.gg/FjGjvcgAqz>.

## License

Copyright (C) 2026 The aimee authors. Licensed under the **GNU AGPL v3.0**. See
[LICENSE](LICENSE) and [NOTICE](NOTICE).

If the AGPL does not suit you, other terms can be discussed. Contact <jbailes@gmail.com>.
Bundled components and generated SDKs may use different licenses; [NOTICE](NOTICE) lists them.
