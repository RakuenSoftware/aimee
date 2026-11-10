# Feature status

This page describes the current tree, the release after published
[1.0.2](https://github.com/RakuenSoftware/aimee/releases/tag/v1.0.2), checked on 2026-11-10.
Published 1.0.2 predates some of these paths. PR status and release qualification are separate from implementation status. `Done` means the path is implemented and covered by
its normal tests. `Gated` means it ships behind configuration or deployment requirements. `Next`
means the contract or branch exists but is not part of the integrated path yet.

Use [Server and KB](SERVER_AND_KB.md) for placement and deployment boundaries. The
[Atlas review](reviews/agent-memory-atlas-2026-09-29.md) records remaining source-confirmed gaps.

Memory moved out of the application image in this release. The public tree keeps the generic
memory SDK, the module transport and the host-side guarantees of a provider-bound request; the
bundled `memory` executable is an explicit unavailable adapter, and records, retrieval and
lifecycle belong to a separately installed implementation ([Memory boundary](MEMORY.md)). Rows
below marked `Installed implementation` describe behaviour that implementation must provide; the
public tree no longer ships it and public CI does not certify it. Existing 1.0.x memories are
carried forward with the procedure in [Upgrading](UPGRADING.md#upgrade-from-102).

## Browser chat integration (2026-10-20)

The browser submits canonical IR turns and replays the Server-owned event log through one reducer.
Primary loading, model switching and compaction use that durable source. Qualified Linux Codex
and Claude schedulers use held native executables, closed environments and the existing workspace
and supervised tool owners. The Server supplies original model context, tool calls and typed results;
SDK conversation observations cannot replace them. Codex npm registrations resolve the matching
native package without running the launcher script.

Actual-SDK integration covers API-key and Vault OAuth forwarding, rotation,
compaction, resume and seeded runs. The current continuation also passes actual
Codex/Claude daemon-crash and interrupted-question recovery, source-bound unknown
outcomes, one-use operator denial and replay without renewed effects or prompts.
Session-selected models persist in owner-checked metadata across restarts.
Private delegate histories now own canonical question/permission scopes; browser
replies target the child session after replay readiness. The web transport rejects
ambiguous reply frames before forwarding validated bytes unchanged.

HTTP and host-owned native executors retain complete owned responses through
selection and repair. Cache claims require durable terminal state and actual tree
quiescence when the optional Linux containment boundary is configured. KB-free
configured-test grading, delegate observations and source-checked next-run reuse
are implemented and have actual-store integration coverage. Legacy import records
exact/normalized fidelity, explicit refusal or a passive text archive, with large
messages and retained sources handled in bounded chunks. Final exact-head CI
qualification remains in progress in #3052.
Production remains KB-free and unpromoted; live subscription and independently
reviewed promotion are separate qualification gates. Existing macOS/Windows
thin-client support does not imply native server execution on those platforms.
See the [current handoff](IR_MIGRATION_HANDOFF.md).

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
| Go workflow control plane | Done | Go WFE owns workflow lifecycle; Go domain and PostgreSQL modules own their state and decisions. C remains the resource host and mechanical enforcement boundary. |
| Versioned `/v1` operations | Done | Named routes replace the generic RPC endpoint. |

## Memory and code

| Feature | State | Boundary |
| --- | --- | --- |
| Memory boundary | Done | Generic memory SDK and module transport; bundled unavailable adapter; implementation selected per role in the module manifest. An instance without one reports `missing_module: memory` and is not ready. |
| Persistent typed memory | Installed implementation | Facts, rules, decisions, episodes, provenance, contradiction, and staleness are the installed implementation's contract. |
| Server/KB placement | Done | Independent stores and immutable roles. Server owns private code and the personal placement; KB owns the shared corpus and the shared placement. Both use the Go PostgreSQL module and select their own memory implementation. |
| PostgreSQL deployment | Done | Separate PostgreSQL service per standard composition; ordinary persistent storage by default, LUKS opt-in. Both roles use the Go PostgreSQL provider; native KB algorithms use session transport. |
| KB-free personal recall | Installed implementation | Ordinary recall defaults to Server; local semantic retrieval needs its embedding service. Missing personal records never fall back to KB. |
| Current-memory eligibility | Installed implementation | Active, unsuppressed, currently valid rows; legacy lifecycle flags do not enable this filter. Historical inspection uses a separate contract. |
| Memory reliability baseline | Installed implementation; host checks Done | Governed revisions, correction review UI, source revalidation, rejection identity, runtime-role refusal protection and erasure replay are implemented. Qualification evidence is scoped to the cited release and Atlas reports; optional adaptive policies remain gated. Revisions, corrections and erasure replay are the installed implementation's; source revalidation at provider dispatch, the byte budget, the send guard and provider receipts remain host checks. |
| Replaceable memory | Done | The implementation is selected per role in the module manifest; the Cognee adapter and the built-in native engine were removed with the extraction. |
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
| Browser workspace | Done | Top session tabs open chat with a session-owned project and history. Chat forwards the authenticated actor and displays tool lifecycle activity, including direct primary models. The left panel opens projects, agents, workflows, graph, logs, settings, and VS Code. |
| Canonical IR migration | In progress | Canonical primary/delegate history, persistent model selection, source-owned Linux native execution, durable interruption recovery and child browser replies are implemented. Legacy-import fidelity and final CI remain in progress; production promotion requires separate review. |
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
