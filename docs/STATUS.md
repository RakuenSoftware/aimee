# Feature status

This page describes the **1.0.0 release target**, inspected integration code, checked on
2026-10-05. Published [0.4.6](https://github.com/RakuenSoftware/aimee/releases/tag/v0.4.6) predates
some of these paths. PR status and release qualification are separate from implementation status. `Done` means the path is implemented and covered by
its normal tests. `Gated` means it ships behind configuration or deployment requirements. `Next`
means the contract or branch exists but is not part of the integrated path yet.

Use [Server and KB](SERVER_AND_KB.md) for placement and deployment boundaries. The
[Atlas review](reviews/agent-memory-atlas-2026-09-29.md) records remaining source-confirmed gaps.

The published 0.4.6 release adds governed memory revisions, source checks at provider dispatch,
reviewed corrections, erasure protection, and durable async run ownership. See the
[release preparation](validation/release-0.4.6-preparation-2026-09-27.md) for completed checks
and its point-in-time preparation gates. Optional adaptive policies are not promoted by this release;
MR-07 remains observe-only, selection and utility-horizon policies remain disabled, and clean
retry remains opt-in. Existing detached source spans still require a published snapshot, and
workspace registration alone does not start a client runner.

## Runtime

| Feature | State | Boundary |
| --- | --- | --- |
| Shared-memory event bus | Done | One host per daemon; typed routing, private queue pairs, backpressure, arena leases, capture. Linux v0. |
| C and pure-Go bus clients | Done | Shared golden vectors and cross-language conformance; no cgo. |
| Audit and observability on the bus | Done | Actions, memory writes, guardrails, vault, sandbox, MCP, and tool outcomes. |
| External bus clients | Next | Executable-bound module clients attach cross-process today. A general external/untrusted client API is not the supported public runtime path. |
| Workflow triggers on the bus | Next | Go trigger scanning and HTTP fire exist; general bus trigger delivery is not integrated. |
| Module replay | Next | Capture replay is observational; it does not re-execute modules. |
| Source-module boundaries | Done for the canonical catalog | 34 module descriptors and 26 Go process identities are checked for ownership, placement and exported builds. Deeper C resource/domain migration remains work. |
| Go workflow control plane | Done | Go WFE owns workflow lifecycle; Go domain/memory/PostgreSQL modules own their state and decisions. C remains the resource host and mechanical enforcement boundary. |
| Versioned `/v1` operations | Done | Named routes replace the generic RPC endpoint. |

## Memory and code

| Feature | State | Boundary |
| --- | --- | --- |
| Persistent typed memory | Done | Facts, rules, decisions, episodes, provenance, contradiction, and staleness. |
| Server/KB placement | Done | Independent stores and immutable roles. Server owns personal memory and private code; KB owns the shared corpus. Both use Go memory and PostgreSQL modules. |
| PostgreSQL deployment | Done | Separate PostgreSQL service per standard composition; ordinary persistent storage by default, LUKS opt-in. Both roles use the Go PostgreSQL provider; native KB algorithms use session transport. |
| KB-free personal recall | Done | Ordinary recall defaults to Server; local semantic retrieval needs its embedding service. Missing personal records never fall back to KB. |
| Current-memory eligibility | Done | Active, unsuppressed, currently valid rows; legacy lifecycle flags do not enable this filter. Historical inspection uses a separate contract. |
| Memory reliability baseline | Done, qualified per recorded gates | Governed revisions, correction review UI, source revalidation, rejection identity, runtime-role refusal protection and erasure replay are implemented. Qualification evidence is scoped to the cited release and Atlas reports; optional adaptive policies remain gated. |
| Replaceable memory / Cognee | Merged; deployed-tested | Generic Store contract, native default, Cognee 1.6.2 alternative; canonical guarantees stay in Aimee. Required real-provider CI is added. Not included in 0.4.6; 256-record scope bound and restart interruption apply. |
| Native-memory model delivery | Integrated source; five-model 0.3.3 candidate prepared separately | Enrolled `/v1/native/primitive` selects personal source records. Separate vLLM plugin prepares model-native banks locally. Five dedicated adapters share one runtime and passed 7900 XTX native-memory smokes. The final candidate is unsigned; current testing passed 2,314 installed-runtime checks; signing and publication remain separate. See the [plugin guide](NATIVE_MEMORY_PLUGIN.md). |
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
| Browser workspace | Done | Top session tabs open chat with a session-owned project and history. The left panel opens projects, agents, workflows, graph, logs, settings, and VS Code. |
| Managed container deploy | Done | Browser manages Server model containers through the mounted Docker socket. A KB is deployed and enrolled separately. |
| Split deploy | Done | Server and KB can run without Docker-socket delegation. |
| Native thin clients | Done | Linux, macOS, and Windows; no database linkage. |

## Removed

| Surface | Replacement |
| --- | --- |
| `aimee chat` and the built-in TUI | Browser, MCP, ACP, or compatible API client. |
| `aimee work` queue | Workflows, triggers, coordinated jobs, and durable delegate jobs. |
| `aimee migrate v2` | Normal schema migration at daemon startup. |
| Generic `/v1/rpc` | Named, versioned `/v1` routes. |
| Combined Server-plus-KB appliance | One application image selects one immutable role per instance; PostgreSQL and models are separate services. |
| Client-held agent keys | Server-sealed vault. |
| Generic `aimee-llm` inference gateway | Each instance owns embedding; local synthesis uses a model-specific `aimee-llm-e2b` or `aimee-llm-e4b` sidecar, and remote synthesis uses its configured endpoint. |
| KB socket autostart | Explicit KB `/v1` service. |

Generated [commands](gen/cli-commands.md), [configuration](gen/configuration.md), and
[routes](gen/api-v1.md) are the exact surface for this checkout.
