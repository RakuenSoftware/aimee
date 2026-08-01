# aimee

A local server that gives any AI coding tool a persistent memory, a map of your code, cheap
delegate models, and guardrails it can't write past. A shared-memory event bus gives the whole
runtime one ordered place to observe and audit what it does. Point your tool at it, or run it
alongside. Your context follows you between tools.

Two parts:

- **aimee-server** is an assistant to one human. It owns sessions, tools, credentials, delegates,
  and workflows.
- **aimee-kb** is a knowledge base for a corpus, team, or company. It owns durable knowledge, code
  indexes, retrieval, and curation.

The `aimee` CLI is a thin client. Linux, macOS, and Windows builds talk to the same server.

## What it does

- **One bus for the runtime.** Each daemon owns a bounded shared-memory event bus. Governed
  actions, memory mutations, guardrail decisions, vault reads, sandbox degradation, MCP calls,
  and tool outcomes all cross the same sequenced tap. The tap feeds the WORM audit ledger and an
  exact capture stream without putting storage on the request path. See [Event bus](docs/EVENT_BUS.md).
- **Memory across sessions and tools.** The curator extracts facts, joins related evidence,
  catches contradictions, and lets stale detail decay. One KB can hold product knowledge, code,
  documents, decisions, and conversation history. See [Knowledge](docs/KNOWLEDGE.md).
- **Your code as a graph.** Symbols, callers, imports, git co-changes, and cross-repo dependencies
  feed search and blast-radius checks. See [Code intelligence](docs/CODE_INTELLIGENCE.md).
- **Delegates that cut the bill.** Route review, diagnosis, drafting, and routine implementation to
  the cheapest model fit for the role. Use an API, a local model, Codex OAuth, or an installed CLI.
  See [Delegates](docs/DELEGATES.md).
- **Roundtables.** Give a draft or diff to several models, require repository evidence, then let a
  chair remove weak findings and return one result. See [Ensembles](docs/ENSEMBLE.md).
- **Workflows that finish the job.** Typed blocks take a proposal through planning, implementation,
  verification, review, and a PR. Triggers and cron jobs can start runs; human gates always stop
  for a human. See [Workflows](docs/WORKFLOWS.md).
- **Guardrails.** Secret paths, unsafe writes, worktree escapes, untrusted MCP packages, and direct
  credential use are checked before execution. Delegate sandboxes can run with no network and no
  credentials. See [Security](docs/SECURITY.md).
- **Any model, any provider.** OpenAI Chat Completions, OpenAI Responses, Anthropic Messages,
  Gemini, Mistral, local OpenAI-compatible servers, and AWS Bedrock all pass through one internal
  request format. Switch the primary model without switching tools.
- **Local inference.** One `aimee-llm` container serves embeddings, reranking, and synthesis on CPU
  or GPU. The KB runs no model itself. See [Inference](docs/KB_LLM_BACKENDS.md).
- **Document ingestion.** Push source trees, Markdown, text, and PDFs. Structured PDF mode keeps
  coordinates, tables, page crops, OCR text, and citations. See [Structured PDF](docs/STRUCTURED_PDF.md).
- **A browser workspace.** Chat, projects, agents, workflows, the code graph, logs, settings, and
  an in-browser VS Code editor live in one UI. See [Browser UI](docs/DASHBOARD.md).
- **An API, not a lock-in.** Use aimee through MCP, ACP, the CLI, the browser, or the versioned
  `/v1` APIs. OpenAI- and Anthropic-compatible ingress lets existing front ends use aimee as their
  model endpoint.

Core services are C. The workflow control plane and browser are Go. Hot paths stay small. Nothing
phones home.

## Get started

Start one container. The browser wizard brings up the KB and inference services.

```bash
git clone https://github.com/RakuenSoftware/aimee.git
cd aimee
docker compose -f compose.server-managed.yaml up -d
```

Open <https://localhost:8443> and sign in as `aimee` / `aimee-local-dev`. The wizard picks the
primary agent, inference tier, git hosts, and workspaces. Its last step starts:

- `aimee-kb`, with private PostgreSQL 18, pgvector, and pgvectorscale inside the container;
- `aimee-llm`, on CPU or GPU.

Change `AIMEE_WEBCHAT_USER` and `AIMEE_WEBCHAT_PASSWORD` before putting it on a network. The
managed compose file mounts the Docker socket; that gives aimee-server control of the host Docker
daemon. Use the regular split stack if you do not want that.

Install the `aimee` release binary on each development machine, then enroll it:

```bash
aimee remote set https://host:8743 aimee-local-dev
aimee remote status
aimee status
```

`remote set` pins a private server certificate, prints its fingerprint, rotates the one-use
bootstrap bearer, and, on Linux, enrolls an individual mTLS certificate. Confirm the fingerprint
out of band. The bootstrap bearer stops working after the first enrollment.

The client registers hooks and MCP with supported coding tools on first setup. Set
`AIMEE_NO_CLIENT_INTEGRATIONS=1` to keep global tool configuration untouched.

```bash
aimee memory store myhost "PVE at 10.0.0.1"
aimee memory search "proxmox"
aimee index find kb_client
aimee delegate review --persona reviewer "Review the current diff"
aimee status
```

The [Quickstart](docs/QUICKSTART.md) covers release binaries, the split stack, source builds,
client setup on all three platforms, and first-run checks.

## Since v0.2.192

The current tree is a large step past the last public release. The short version:

- every daemon gained a bounded shared-memory event bus, C and pure-Go clients, typed routing,
  backpressure, large-payload leases, capture/replay, and a single audit seam;
- action audit, memory writes, semantic guardrails, vault access, sandbox degradation, MCP calls,
  and tool outcomes moved onto that bus; accepted records drain to durable sinks on shutdown;
- the bus made cross-language modules, external attachment, uniform policy, workflow events, and
  full-stream telemetry possible without another private side channel; those consumers land one at
  a time behind their own contracts and tests;
- the workflow control plane moved to Go and now schedules parallel slices, live forge work,
  triggers, retries, review loops, and human gates;
- the combined appliance image is gone; managed and split deployments use separate server, KB,
  and inference containers;
- the KB container now owns its PostgreSQL database and can export it to an external server;
- delegates gained role/persona routing, stronger admission, isolated worktrees, networkless
  containers, package mediation, custom images, and learned toolchains;
- all provider wires now pass through one canonical request/response IR;
- roundtables gained evidence requirements, per-seat model selection, retries, and an optional
  reasoning chair;
- the server gained mTLS client enrollment, per-request revocation checks, a sealed credential
  vault, org model catalogs, budgets, rate limits, spend reporting, Bedrock, and WORM audit paths;
- interactive `aimee chat`, the old work queue, `aimee migrate v2`, and the combined image were
  removed.

See [What's new](docs/WHATS_NEW.md) for the full grouped change list and upgrade notes.

## Docs

Start at the [documentation index](docs/README.md).

| Document | Use it for |
|----------|------------|
| [Quickstart](docs/QUICKSTART.md) | Install, enroll, verify. |
| [Manual](MANUAL.md) | Day-to-day use and operations. |
| [Architecture](docs/ARCHITECTURE.md) | Processes, storage, trust, request flow. |
| [Event bus](docs/EVENT_BUS.md) | Routing, ordering, audit, capture, and extension contracts. |
| [Deployment](docs/DEPLOYMENT.md) | Managed, split, external DB2, backup, and hardening. |
| [Upgrading](docs/UPGRADING.md) | Move from v0.2.192 without losing data or identity. |
| [Command reference](docs/gen/cli-commands.md) | Every CLI command. Generated from source. |
| [Configuration reference](docs/gen/configuration.md) | Every config key and environment variable. Generated from source. |
| [Server API](docs/PUBLIC_API.md) | `/v1` transport, auth, compatibility, generated specs. |
| [Feature status](docs/STATUS.md) | What works, what is gated, what was removed. |
| [Troubleshooting](docs/TROUBLESHOOTING.md) | Diagnose the first broken boundary. |
| [Technical reference](src/README.md) | Modules, build, tests, and code ownership. |

## Community

Questions and discussion: <https://discord.gg/FjGjvcgAqz>.

## License

Copyright (C) 2026 The aimee authors. Licensed under the **GNU AGPL v3.0**. See
[LICENSE](LICENSE) and [NOTICE](NOTICE).

If the AGPL does not suit you, other terms can be discussed. Contact <jbailes@gmail.com>.
Bundled components and generated SDKs may use different licenses; [NOTICE](NOTICE) lists them.
