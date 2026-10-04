# aimee

Aimee is a persistent runtime for AI tools. It keeps your memory, code index, sessions and
workflows outside any one model or coding client. Change the tool or provider and the state stays.
The server also governs what an agent may read, execute, send and change.

Memory is the hard part. A stored answer can become wrong, a correction can race an old revision,
and a retrieved record can be erased before the model uses it. Aimee carries source identity and
revision through retrieval, review and provider dispatch. Models can propose corrections;
authenticated users decide whether protected content changes.

![Aimee overview: enrolled tools and browser use a personal Server; shared knowledge and Cognee retrieval are optional](docs/images/architecture/overview.svg)

## Your runtime, with shared knowledge when you need it

A standalone **Server** owns one person's sessions, durable personal memory, private code index,
credentials, tools and delegates. Its Go workflow engine owns scheduling, retries, gates and durable
workflow runs. It works without a knowledge server.

An optional **KB** owns a shared corpus: memories, documents, facts, code graphs, evidence and
curation. Connecting one adds an explicitly selected shared store. Personal records stay on Server;
`--store kb` selects shared knowledge. A project name or failed private lookup cannot switch stores.

Both roles ship in one application image. Each instance retains its own immutable role, identity,
Vault, PostgreSQL store and event bus. The thin CLI runs on Linux, macOS and Windows and opens no
database. A unified node with parent connections remains separate design work; the current runtime
still has these two roles. See [Server and KB](docs/SERVER_AND_KB.md).

## What you can do

- **Keep and correct memory.** Personal and shared records retain versions, scope, provenance and
  lifecycle. The Memory Center supports correction proposals and attributed review. Current recall
  excludes hidden, expired and retired material. See [Knowledge](docs/KNOWLEDGE.md).
- **Ask about the code you are working on.** Published indexes supply symbols, callers, imports,
  text, vectors and blast radius. Private indexing stays on Server; shared code belongs to KB.
  Detached source spans describe a published snapshot, so rescan when the files change.
  See [Code intelligence](docs/CODE_INTELLIGENCE.md).
- **Delegate bounded work.** Route review and implementation to eligible agents, isolate their
  worktrees and containers, and apply capability, spend and execution constraints. A cheaper route
  is useful only when it fits the task; Aimee does not promise a universal cost reduction.
  See [Delegates](docs/DELEGATES.md).
- **Run repeatable workflows.** Definitions resolve to versioned execution snapshots. Runs preserve
  transitions, artifacts, human gates and terminal failures. See [Workflows](docs/WORKFLOWS.md).
- **Switch providers.** OpenAI, Anthropic, Gemini, Mistral, Bedrock and local endpoints feed a common
  request/response representation. Model capability and endpoint readiness still govern what works.
  See [Compatibility](docs/COMPATIBILITY.md).
- **Inspect the action trail.** The event bus orders governed actions and observations. WORM evidence,
  transactional mutation intents and delivery receipts expose distinct durability milestones.
  External witnesses are needed for evidence against a compromised host.
  See [Security](docs/SECURITY.md) and [WORM worker](docs/WORM_WORKER.md).

## Memory engines can change without changing ownership

The replaceable-memory implementation in [PR #3005](https://github.com/RakuenSoftware/aimee/pull/3005)
extracts the current memory API into a generic contract. Native Aimee memory remains the default;
Cognee 1.6.2 is the first alternative retrieval engine. Aimee retains canonical records,
authorization, identity, audit and lifecycle. A replacement uses the existing module infrastructure.

Both backends passed deployed private/shared API and lifecycle checks in disposable containers.
Cognee also passed provider-outage retry and derived-state cleanup. Its first adapter bounds a
retrieval scope to 256 eligible records and refuses larger scopes explicitly. This implementation
is part of the 1.0.0 release work and is absent from the older 0.4.6 image. The
[contract and authoring guide](docs/modules/memory.md#memory-backend-contract) describes integration,
configuration and limits.

The separate [native-memory vLLM plugin](docs/NATIVE_MEMORY_PLUGIN.md) lets supported local models
consume selected Aimee records as native attention memory. That model-side delivery mechanism and
a replaceable retrieval engine solve different parts of the memory path. The plugin's staged
preview requires a server with the newer `/v1/native/primitive` route.

## Start with one Server

Follow [Quickstart](docs/QUICKSTART.md) to generate private database credentials and start
`compose.yaml` with Docker Linux containers. It starts Server, a PostgreSQL service and local
embedding. Persistent storage is an ordinary Docker volume; LUKS is an explicit option.
A KB and synthesis model are optional.

Open <https://localhost:8443> and use the generated first-boot login from the application log.
The wizard configures your account, provider, local memory models, Git identity and workspaces.
Conversations open from the top session tabs. Connect a separately deployed KB in Settings when
shared knowledge is needed.

`compose.server-managed.yaml` lets the browser manage model containers through the host Docker
socket. Use the standard composition when you want to manage those containers yourself.
Back up each instance's home, Vault, database and audit evidence together before upgrading.

## 1.0.0 is the release target

This tree prepares **1.0.0**. The declared application series is `1.0`; release approval and
artifact publication remain separate from merging code. The memory contract, Cognee integration
and native-memory delivery described here belong to that release work.

The previous published application release is **0.4.6**, dated 2026-09-27. It does not contain
all of these changes. **0.3.0 was an intended release**, published on 2026-08-04. The later 0.4
series changed deployment and runtime boundaries; it did not invalidate that release.

Use [What's new](docs/WHATS_NEW.md) for the 1.0.0 scope and release history,
[Feature status](docs/STATUS.md) for implementation and qualification, and
[Upgrading](docs/UPGRADING.md) before reusing an older store. Current database startup refreshes
credentials but does not repair an obsolete database/role layout. The separate native-memory
plugin keeps its own version and publication status.

## Read further

The [documentation index](docs/README.md) maps the full set of guides.

| Task | Guide |
| --- | --- |
| Install, enroll and verify | [Quickstart](docs/QUICKSTART.md) |
| Operate CLI, browser and memory | [Manual](MANUAL.md) |
| Understand processes and trust | [Architecture](docs/ARCHITECTURE.md) |
| Deploy, back up or restore | [Deployment](docs/DEPLOYMENT.md) |
| Implement a memory backend | [Memory contract](docs/modules/memory.md#memory-backend-contract) |
| Connect an enrolled local vLLM model | [Native memory plugin](docs/NATIVE_MEMORY_PLUGIN.md) |
| Call a named API or configure a field | [Public API](docs/PUBLIC_API.md), [commands](docs/gen/cli-commands.md), [configuration](docs/gen/configuration.md) |
| Diagnose a failure | [Troubleshooting](docs/TROUBLESHOOTING.md) |
| Contribute code or review ownership | [Contributing](CONTRIBUTING.md), [owners](OWNERS.md), [technical reference](src/README.md) |

## Community and license

Questions and discussion: <https://discord.gg/FjGjvcgAqz>.

Copyright (C) 2026 The aimee authors. Licensed under the **GNU AGPL v3.0**. See
[LICENSE](LICENSE) and [NOTICE](NOTICE). Other terms can be discussed at <jbailes@gmail.com>.
Bundled components and generated SDKs may use different licenses; NOTICE lists them.
