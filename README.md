# aimee

Your AI coding tool forgets everything between sessions, and you pay full price for the routine
work. aimee is a local server that fixes both. It holds memory and a map of your code across tools,
routes cheap work to cheap models, and enforces guardrails the model cannot write past.

Point any tool at it. Your context follows you between them.

One application image, with one identity established on first boot:

- **aimee-server** assists one human. Sessions, tools, credentials, delegates, workflows.
- **aimee-kb** serves a corpus, team, or company. Durable knowledge, code indexes, retrieval,
  curation.

Server works without a KB. Both identities compose the same Go PostgreSQL and memory modules,
with their own local Vault and PostgreSQL container (LUKS is optional). The role cannot be changed on an
existing instance. The `aimee` CLI is a thin client for Linux, macOS, and Windows; Go composition
modules supervise the standard modules around the existing C resource and event-bus hosts.

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

Follow the [Quickstart](docs/QUICKSTART.md) to generate private database credentials and start
`compose.yaml` with Docker Linux containers. It starts Server, PostgreSQL, and local embedding.
Storage uses an ordinary Docker volume by default; LUKS encryption is an explicit opt-in.
Synthesis is optional; no KB is installed.

Open <https://localhost:8443> and use the generated first-boot login from the application log.
The wizard configures your account, provider, local memory models, Git identity, and workspaces.
Connect an existing shared KB later in Settings. `compose.server-managed.yaml` additionally lets
the wizard manage local model containers through the Docker socket.

## Docs

Start at the [documentation index](docs/README.md).

| Document | Use it for |
|----------|------------|
| [Quickstart](docs/QUICKSTART.md) | Install, enroll, verify. |
| [What's new](docs/WHATS_NEW.md) | Everything 0.4.0 changed, and what it removed. |
| [Upgrading](docs/UPGRADING.md) | Migrate storage and preserve instance identity. |
| [Manual](MANUAL.md) | Day-to-day use and operations. |
| [Architecture](docs/ARCHITECTURE.md) | Processes, storage, trust, request flow. |
| [Deployment](docs/DEPLOYMENT.md) | Standalone Server, optional KB, encrypted storage, backup. |
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
