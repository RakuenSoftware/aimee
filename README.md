# aimee

A local server that gives any AI coding tool a persistent memory, a map of your code, cheap
delegate models, and guardrails it can't write past. Point your tool at it, or run it
alongside. Your context follows you between tools.

Two parts:

- **aimee-server** is an assistant to one human. Learns how you work and what you expect.
  General purpose, but coding is what it does best, especially across large or multi-repo
  work.
- **aimee-kb** is a knowledge base for a whole corpus: a subject, a company, a team.

## What it does

- **Memory across sessions and tools.** A curator distills each session into a typed
  knowledge base, dedupes facts, flags contradictions, and lets stale detail decay. Point a
  team at one kb and everyone works from the same memory. See [KNOWLEDGE.md](docs/KNOWLEDGE.md).
- **Your code as a graph.** aimee indexes your code into a symbol and call graph, spanning
  repos. The AI finds callers and traces an edit's blast radius before it writes.
  See [CODE_INTELLIGENCE.md](docs/CODE_INTELLIGENCE.md).
- **Delegates that cut the bill.** Route review, summarization, and boilerplate to the
  cheapest model that can do it. A local GPU model or a subscription plan costs nothing
  extra. The primary gets a compact result back. See [DELEGATES.md](docs/DELEGATES.md).
- **Guardrails.** `.env`, keys, and prod configs are blocked before the AI touches them.
  Planning mode freezes writes. Every session is fully isolated by aimee.
  See [SECURITY.md](docs/SECURITY.md).
- **Any model, any provider.** Point a tool's OpenAI or Anthropic compatible API at aimee
  and it runs the turn on any model: Claude, GPT, Gemini, a model on your own GPU. Or run
  aimee beside the tool over MCP/ACP and it keeps its own model. Switch tools whenever.
- **Workflows.** Compose a job from typed steps and aimee runs it to a PR, with review
  panels and human sign-off gates where you want them. See [WORKFLOWS.md](docs/WORKFLOWS.md).
- **Run your own inference.** Embeddings, reranking, and synthesis in one CPU or GPU
  container. The kb curates locally, no outside API calls. See [KB_LLM_BACKENDS.md](docs/KB_LLM_BACKENDS.md).
- **A browser workspace.** `aimee-webchat`: chat, a live code graph, a git project manager,
  an in-app VS Code editor, and dashboards. No terminal required. See [DASHBOARD.md](docs/DASHBOARD.md).

Core services are C. Hot paths run in single-digit milliseconds. Nothing phones home.

## Get started

One container to start. The browser wizard brings up the rest.

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee

# Start aimee-server. It mounts the host Docker socket so the wizard can launch
# aimee-kb, the LLM, and Postgres for you — CPU or GPU. No second compose command.
docker compose -f compose.server-managed.yaml up -d
```

Open <https://localhost:8443>, log in as `aimee` / `aimee-local-dev`, and run the setup
wizard: pick a primary agent, choose CPU or GPU, connect a git host, pick workspaces. Its
**Deploy** step brings up aimee-kb, the LLM, and Postgres. Change the login (`AIMEE_WEBCHAT_USER`
/ `AIMEE_WEBCHAT_PASSWORD`) before you put it on a network.

Then install the `aimee` CLI on each dev machine and point it at the server:

```bash
aimee remote set https://host:8743 <token>   # AIMEE_TLS_INSECURE=1 for the self-signed cert
aimee status                                  # server, DB1, and kb health
```

aimee wires its hooks and MCP server into your AI coding tools on first run — opt out with
`AIMEE_NO_CLIENT_INTEGRATIONS=1`.

[QUICKSTART.md](docs/QUICKSTART.md) walks the full setup. Every deployment topology and the
from-source install live in [Deployment and operations](src/README.md#deployment-and-operations).
Questions? Join the [Discord](https://discord.gg/FjGjvcgAqz).

```bash
aimee memory store myhost "PVE at 10.0.0.1"   # a fact the AI keeps across sessions
aimee memory search "proxmox"
aimee delegate review "Review this PR"         # route to a cheaper model
aimee status
```

## Docs

| Document | What's in it |
|----------|--------------|
| [Manual](MANUAL.md) | Full reference: install, config, commands, feature guides. |
| [Architecture](docs/ARCHITECTURE.md) | Processes, storage, trust boundaries, request lifecycles. |
| [Technical Reference](src/README.md) | Code internals, RPC/HTTP surfaces, build, deployment. |
| [Commands](docs/COMMANDS.md) | Client command contract, flags, options. |
| [How aimee learns](docs/KNOWLEDGE.md) | The kb, the self-learning pipeline, delegation economics. |
| [Code intelligence](docs/CODE_INTELLIGENCE.md) | Symbol/call graph, cross-repo deps, blast radius. |
| [Workflows](docs/WORKFLOWS.md) | The dev-lifecycle workflow engine and authoring. |
| [Security Model](docs/SECURITY.md) | Threat model, trust boundaries, capability system. |
| [Delegate Sandbox](docs/DELEGATE_SANDBOX.md) | Running delegates in a network-none container and customizing its image. |
| [Compatibility](docs/COMPATIBILITY.md) | Supported OS, shell, and provider matrix. |
| [Feature Status](docs/STATUS.md) | Implementation status of all features. |

More references live under [docs/](docs/).

## Community

Questions and discussion on the aimee Discord: <https://discord.gg/FjGjvcgAqz>.

## License

Copyright (C) 2026 The aimee authors. Licensed under the **GNU AGPL v3.0** (AGPL-3.0). See
[LICENSE](LICENSE) and [NOTICE](NOTICE).

If the AGPL doesn't suit you, other terms can be discussed. Contact <jbailes@gmail.com>.
Bundled third-party components (cJSON, the generated SDKs under `api/sdks/`) are licensed
separately; see [NOTICE](NOTICE).
