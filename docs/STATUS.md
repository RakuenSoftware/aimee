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
| DB1/DB2 ownership | Done | The store module owns DB1's PostgreSQL; KB owns DB2's PostgreSQL and pgvector; the server and thin clients own neither. |
| Embedded KB PostgreSQL | Done | Default container path; external PostgreSQL remains supported. |
| Hybrid retrieval | Done | Lexical, dense, graph, evidence, synthesis, and abstention stages. |
| Cross-repo code graph | Done | Symbols, calls, imports, dependencies, co-change, callers, and blast radius. |
| Client-side content push | Done | Remote clients upload bytes; server paths never name client files. |
| Structured PDF evidence | Gated | Coordinates are the base; vectors, tables, assets, and OCR have separate gates. |
| Autonomous curation | Done | Extract, dedupe, contradict, decay, reflect, and promote through bounded workers. |
| Temporal assertion recall | Done | Independent world-time and belief-time axes; current-only by default, historical recall opt-in. |
| Evidence-backed observations | Done | Two independent sessions before a recurrence becomes an observation; every claim carries an exact span and hash. |
| Reviewed procedural learning | Done | Observations raise proposals into the existing review gate with applicability, expiry, evidence, and rollback. |
| Typed context assembly | Done | Per-channel budgets, packing traces, watermarks, and trust boundaries. Master and per-channel opt-outs remain. |
| Index detach, purge, and GC | Proposed | `workspace remove` unregisters only. No shipped command deletes indexed data; the audited lifecycle is designed. |
| Multi-KB fleet routing | Next | The design selects a KB by corpus, authority, and capabilities; current managed and split profiles configure one KB URL. |
| Per-KB local or remote model roles | In progress | Embedding placement and model-specific local synthesis sidecars work; profile support is still converging. There is no generic inference gateway. |

## Agents and workflows

| Feature | State | Boundary |
| --- | --- | --- |
| Role/persona delegate routing | Done | Viable-agent retry; explicit pins fail instead of silently changing model. |
| Isolated delegate worktrees | Done | Created on first write and bounded to the assigned workspace. |
| Per-session branch and worktree | Done | CLI and MCP sessions; cut from the default branch, keyed per session id so two sessions never share one. |
| Networkless delegate sandbox | Done | Default container posture; custom images and mediated packages are supported. |
| Roundtables | Done | Parallel seats, per-seat models, evidence, retry, chair, and cost accounting. |
| Typed workflows | Done | Validation, version hashes, retries, loops, gates, and durable run state. |
| Parallel workflow slices | Done | Agent admission and per-workflow limits prevent oversubscription. |
| Watched-proposal triggers | Done | Go scans `watch-dir` and `proposals`. Mode is recorded, but current scheduling is identical; human gates always park. |
| Generic cron jobs | Done | The C job scheduler runs configured commands. Cron is not a Go WFE trigger source. |
| Live forge and PR completion | Done | Branch, implement, verify, review, merge, and PR steps have terminal failure states. |
| Transactional turn rewind | Proposed | Design exists; not a shipped recovery path. |

## Providers and context

| Feature | State | Boundary |
| --- | --- | --- |
| Canonical request/response IR | Done | Provider wire formats end at translation modules. |
| OpenAI, Anthropic, Gemini, Mistral, Bedrock, local endpoints | Done | Availability still depends on credentials and provider capability. |
| Provider catalogs and model registry | Done | Context, output, price, capability, quota, and deprecation metadata. |
| Context economizer | Done | Folding, cache alignment, and tool-output condensation are independently configurable. |

## Security and operations

| Feature | State | Boundary |
| --- | --- | --- |
| Sealed credential vault | Done | Server-side source of truth; use TPM, PKCS#11, KMS, or local root-key custody. |
| Thin-client mTLS | Gated | Linux enrollment is automatic; other clients use their native TLS stores and configured certs. |
| Per-user remote writes | Done | KB-signed identity, server/team/JWKS trust, and an exact subject grant. |
| WORM audit chain | Done | Hash chain, checkpoints, verification, seal, and evidence exports. |
| External witness and anchor | Gated | Needed for evidence against a compromised host. |
| Org budgets and rate limits | Done | Catalog, admission, spend, and quota surfaces. |
| Browser workspace | Done | Chat, projects, agents, workflows, graph, logs, settings, and VS Code. |
| Managed container deploy | Done | Browser can launch the current one-KB profile through the mounted Docker socket. |
| Split deploy | Done | Server and KB can run without Docker-socket delegation. |
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
| Generic `aimee-llm` inference gateway | Embedding stays with the KB; local synthesis uses a model-specific `aimee-llm-e2b` or `aimee-llm-e4b` sidecar, and remote synthesis uses its configured endpoint. |
| KB socket autostart | Explicit KB `/v1` service. |

Generated [commands](gen/cli-commands.md), [configuration](gen/configuration.md), and
[routes](gen/api-v1.md) are the exact surface for this checkout.
