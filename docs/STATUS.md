# Feature status

This page describes the current testing tree. `Done` means the path is implemented and covered by
its normal tests. `Gated` means it ships behind configuration or deployment requirements. `Next`
means the contract or branch exists but is not part of the integrated path yet.

## Runtime

| Feature | State | Boundary |
| --- | --- | --- |
| Shared-memory event bus | Done | One host per daemon; typed routing, private queue pairs, backpressure, arena leases, capture. Linux v0. |
| C and pure-Go bus clients | Done | Shared golden vectors and cross-language conformance; no cgo. |
| Audit and observability on the bus | Done | Actions, memory writes, guardrails, vault, sandbox, MCP, and tool outcomes. |
| External bus clients | Next | Inline cross-process attachment exists on a follow-on branch; not the integrated runtime path. |
| Workflow triggers on the bus | Next | Trigger event contracts and routing exist on a follow-on branch. |
| Module replay | Next | Capture replay is observational; it does not re-execute modules. |
| Source-module boundaries | In progress | Owned headers, descriptors, dependency gates, and attested docs are landing by module. |
| Go workflow control plane | Done | Go owns workflow scheduling; C owns runtime, storage, tools, and policy seams. |
| Versioned `/v1` operations | Done | Named routes replace the generic RPC endpoint. |

## Memory and code

| Feature | State | Boundary |
| --- | --- | --- |
| Persistent typed memory | Done | Facts, rules, decisions, episodes, provenance, contradiction, and staleness. |
| DB1/DB2 ownership | Done | Server owns SQLite; KB owns PostgreSQL and pgvector; thin clients own neither. |
| Embedded KB PostgreSQL | Done | Default container path; external PostgreSQL remains supported. |
| Hybrid retrieval | Done | Lexical, dense, graph, evidence, rerank, synthesis, and abstention stages. |
| Cross-repo code graph | Done | Symbols, calls, imports, dependencies, co-change, callers, and blast radius. |
| Client-side content push | Done | Remote clients upload bytes; server paths never name client files. |
| Structured PDF evidence | Gated | Coordinates are the base; vectors, tables, assets, and OCR have separate gates. |
| Autonomous curation | Done | Extract, dedupe, contradict, decay, reflect, and promote through bounded workers. |

## Agents and workflows

| Feature | State | Boundary |
| --- | --- | --- |
| Role/persona delegate routing | Done | Viable-agent retry; explicit pins fail instead of silently changing model. |
| Isolated delegate worktrees | Done | Created on first write and bounded to the assigned workspace. |
| Networkless delegate sandbox | Done | Default container posture; custom images and mediated packages are supported. |
| Roundtables | Done | Parallel seats, per-seat models, evidence, retry, chair, and cost accounting. |
| Typed workflows | Done | Validation, version hashes, retries, loops, gates, and durable run state. |
| Parallel workflow slices | Done | Agent admission and per-workflow limits prevent oversubscription. |
| Triggers and cron | Done | Start runs in autonomous or interactive mode. Human gates always park. |
| Live forge and PR completion | Done | Branch, implement, verify, review, merge, and PR steps have terminal failure states. |
| Transactional turn rewind | Proposed | Design exists; not a shipped recovery path. |

## Providers and context

| Feature | State | Boundary |
| --- | --- | --- |
| Canonical request/response IR | Done | Provider wire formats end at translation modules. |
| OpenAI, Anthropic, Gemini, Mistral, Bedrock, local endpoints | Done | Availability still depends on credentials and provider capability. |
| Provider catalogs and model registry | Done | Context, output, price, capability, quota, and deprecation metadata. |
| Context economizer | Done | Folding, cache alignment, and tool-output condensation are independently configurable. |
| Local inference service | Done | CPU/GPU tiers serve embedding, rerank, and synthesis outside the KB process. |

## Security and operations

| Feature | State | Boundary |
| --- | --- | --- |
| Sealed credential vault | Done | Server-side source of truth; use TPM, PKCS#11, KMS, or local root-key custody. |
| Thin-client mTLS | Gated | Linux enrollment is automatic; other clients use their native TLS stores and configured certs. |
| Per-user remote writes | Done | Needs deployment posture plus an authenticated user grant. |
| WORM audit chain | Done | Hash chain, checkpoints, verification, seal, and evidence exports. |
| External witness and anchor | Gated | Needed for evidence against a compromised host. |
| Org budgets and rate limits | Done | Catalog, admission, spend, and quota surfaces. |
| Browser workspace | Done | Chat, projects, agents, workflows, graph, logs, settings, and VS Code. |
| Managed container deploy | Done | Browser can launch KB and inference through the mounted Docker socket. |
| Split deploy | Done | Server, KB, and inference can run without Docker-socket delegation. |
| Native thin clients | Done | Linux, macOS, and Windows; no database linkage. |

## Removed

| Surface | Replacement |
| --- | --- |
| `aimee chat` and the built-in TUI | Browser, MCP, ACP, or compatible API client. |
| `aimee work` queue | Workflows, triggers, coordinated jobs, and durable delegate jobs. |
| `aimee migrate v2` | Normal schema migration at daemon startup. |
| Generic `/v1/rpc` | Named, versioned `/v1` routes. |
| Combined appliance image | Managed or split container stack. |
| Client-held agent keys | Server-sealed vault. |
| KB socket autostart | Explicit KB `/v1` service. |

Generated [commands](gen/cli-commands.md), [configuration](gen/configuration.md), and
[routes](gen/api-v1.md) are the exact surface for this checkout.
