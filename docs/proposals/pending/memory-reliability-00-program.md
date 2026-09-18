# Aimee Memory Reliability: Proposal Series

**State:** Proposed · **Revision:** 3 · **Date:** 17 September 2026

## Decision requested

Adopt the following 18 proposals as a dependency-ordered improvement program for memory correctness, evidence quality, context efficiency and operational reliability. Review and implement each proposal in its defined slices. Approval of the program is not approval to enable every optional policy in production.

The program reuses Aimee's personal/shared memory ownership, PostgreSQL schema, typed context, event bus, reviewed learning, execution policy, audit/WORM and economizer. It establishes consistent contracts across those owners and adds derived views and diagnostics where needed.

All command names, schema additions, defaults and release thresholds described as proposed are design work. The acceptance gates are requirements to implement and run; this package does not represent them as completed changes or passing tests.

## Proposal index

| ID | Proposal | Priority | Primary outcome |
|---|---|---|---|
| [MR-01](memory-reliability-01-unified-eligibility-and-validity.md) | Unified retrieval eligibility and validity | P0 | Every serving surface applies the same authorized lifecycle/time rules |
| [MR-02](memory-reliability-02-authority-preserving-mutations.md) | Authority-preserving memory mutations | P0 | All write verbs preserve author authority, provenance and history |
| [MR-03](memory-reliability-03-final-payload-context-budgets.md) | Final-payload budgets and protected projections | P0 | Actual provider-bound context obeys byte/token caps |
| [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md) | Evidence lineage, independent corroboration and retraction | P1 | Copies cannot self-corroborate; corrections/erasure reach derivatives |
| [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) | Requirement-based sufficiency and bounded recovery | P0/P1 | Completeness reflects delivered evidence and declared task needs |
| [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) | Actual traces, context receipts and evidence states | P0/P1 | Explain ranking and distinguish assembled, dispatched and acknowledged context |
| [MR-07](memory-reliability-07-task-exploration-contracts.md) | Task-bound exploration contracts | P1 | Reduce redundant discovery without trapping tasks or widening permission |
| [MR-08](memory-reliability-08-retrieval-health-telemetry.md) | Retrieval health telemetry | P1; baseline starts immediately | Detect concentration, lifecycle leakage and low-trust spread with defined metrics |
| [MR-09](memory-reliability-09-fair-hybrid-ranking-and-exposure.md) | Fair hybrid candidates, bounded priors and exposure selection | P1/P2 | All retrieval arms compete; priors/diversity cannot dominate requirements |
| [MR-10](memory-reliability-10-deterministic-utility-horizons.md) | Deterministic utility horizons | P1 | Expire ordinary usefulness of transient state without deleting valid history |
| [MR-11](memory-reliability-11-embedding-generations-and-index-freshness.md) | Embedding generations and bounded index freshness | P1 | Prevent vector-space mixing and retrieval stalls during rebuilds |
| [MR-12](memory-reliability-12-served-memory-views-and-claim-cards.md) | Named served-memory views and claim cards | P1 | Deliver task-oriented briefings and inspectable evidence-backed claims |
| [MR-13](memory-reliability-13-disposable-task-projections.md) | Disposable task projections | P2 | Keep working hypotheses useful without promoting them to canonical truth |
| [MR-14](memory-reliability-14-proposal-only-memory-hygiene.md) | Proposal-only hygiene | P2 | Detect memory problems while keeping canonical changes reviewable |
| [MR-15](memory-reliability-15-procedure-outcomes-and-task-cost.md) | Procedure experience, delayed outcomes and task cost | P1/P2 | Learn from verified application outcomes and account for complete task cost |
| [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md) | Evidence-bound actions and composition policy | P1 | Reauthorize exact effects and prevent sequence/idempotency bypasses |
| [MR-17](memory-reliability-17-clean-retry-context.md) | Clean retry context with preserved real-world state | P2 | Isolate failed reasoning without hiding or replaying actual effects |
| [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) | Frozen evaluation, migration parity and release gates | P0; continuous | Measure equivalent behavior and block invariant regressions |

P0 denotes correctness foundations and required evaluation. P1 denotes the next integration wave. P2 denotes measured extensions. A P2 feature may be designed earlier; it must not be enforced before its dependencies and evaluation gates are ready.

## Common contracts and ownership

These definitions apply to every proposal. Individual files restate their critical invariants so that they can be reviewed separately.

| Concept | Shared definition |
|---|---|
| Authenticated scope | Host-derived principal, audience, purpose and current permission; a caller's scope string is not a grant |
| Canonical memory | Versioned durable assertion/experience/instruction admitted by the existing owner |
| Derived projection | Rebuildable state carrying declared input versions and inherited access; no independent authority |
| Valid time / belief time | Separate temporal coordinates, with explicit support or rejection by each endpoint |
| Eligibility | Hard policy decision before ranking and again at release; similarity cannot override it |
| Index admission | Permission to process and retain a version in an index; query-time eligibility still controls serving |
| Collection dependency | Scoped query input population, including absent matches and time-driven applicability changes |
| Utility horizon | Ordinary-serving policy for usefulness, separate from truth, validity, retention and erasure |
| Evidence family | Established common origin/dependency group; distinct identifiers alone do not prove independence |
| Sufficiency | Coverage of declared task requirements by the final retained evidence; separate from answer correctness |
| Context plan | Versioned selection/requirement/projection result bound to the request/task and its inputs |
| Context receipt | Durable commitment and staged evidence about assembly/preparation/dispatch; its state must match what happened |
| Exploration contract | Host-issued adaptive work limit that can tighten, never widen, operator permission or ceilings |
| Outcome | Host-verifiable result or explicitly attributed feedback; automatic exposure is not successful use |

Keep policy artifacts versioned and atomically switchable. Record the effective versions used by a request. Distinguish absent limits, disabled adaptive policies and literal zero allowances; do not overload integer zero across incompatible meanings.

A budget's identity includes its unit and reset scope. Token, byte, candidate, file, raw-scan, graph-work, time and monetary budgets are separate. Concurrent operations reserve against the relevant task/session ceiling atomically. Refund only under a defined outcome that proves the charged work was not admitted/executed; network uncertainty is not such proof.

Trace metadata and model context are separate projections. IDs, hashes and family links remain potentially sensitive. Keep authorized audience/purpose and retention on diagnostics, caches and audit records. Required action/context receipts cannot be replaced by sampled health events.

## Go memory module integration

Make the memory module Golang-only: its implementation, tests and executable belong to `server-go/modules/memory` and the supervised `aimee-module-memory` process. This includes memory communication: client calls, request/reply framing, wire types, serialization, scope propagation and compatibility translation belong in Go. No C source or header may remain in the memory module or provide a relocated memory implementation or communication path elsewhere in the repository. A clean module directory alone does not satisfy this requirement. Strengthen the shared Go implementation across Server/private and KB/shared placements. Keep placement validation, scoped record identities and the PostgreSQL bus boundary; do not merge personal/shared stores or introduce a second memory service. The [module descriptor](../../../src/modules/memory/module.yaml) and [process contracts](../../../src/modules/process-contracts.json) remain the source of the shipped inventory and wire surface.

| Responsibility | Implementation boundary |
|---|---|
| Memory communication, wire contracts and caller integration | Go clients and handlers over the existing Go bus implementation; no memory-specific C adapter, decoder, callback registry or ABI shim. |
| Eligibility, temporal rules, mutation admission and utility horizons | Go memory domain functions used by all data operations; PostgreSQL migrations, constraints and transactional storage enforce durable invariants through the existing storage owner. |
| Candidate collection/fusion, memory requirements, evidence lineage, memory projection and named views | Go memory implementation over placement-specific adapters. Migrate matching memory policy from the typed C backend into this owner as the relevant slice lands. |
| Memory indexing and collection/dependency freshness | Go memory owns admission, jobs and serving decisions for its records; coordinate storage/index generation operations with existing DB2/PostgreSQL owners over declared contracts. |
| Task state, exploration limits and action admission | Existing task/execution-policy owners consume versioned Go memory evidence and freshness decisions. Memory cannot grant tool permission or own external effects. |
| Procedure review and verified outcomes | Existing learning owner remains authoritative; memory serves governed projections and references, without acquiring a second promotion/reward engine. |
| Final provider serialization, full-request caps and dispatch receipts | Existing host/provider, economizer and audit owners consume the Go memory projection. Return final retained IDs/spans for Go memory coverage evaluation; provider-specific wrappers and credentials stay at the host boundary. |

Extend the existing memory-data contract with bounded, versioned request/result types for eligibility, mutations, projections, lineage and views. Factor domain functions out of transport dispatch and storage adapters; do not grow `handleData` into a second implementation of those rules. Adapters validate wire shape and preserve domain reason codes, effective limits, source versions and capability status. Govern new calls with the existing authenticated invocation, deadline, cancellation and payload limits. Unknown schema versions or unsupported placement capabilities return explicit errors.

Memory does not gain direct database connections or in-process imports of other feature modules to implement this series. Use existing bus/storage contracts and host-supplied, authenticated inputs at ownership boundaries; declare any necessary contract extension. Carry learning/task evidence with its original authority and dependencies. Preserve the memory module's dependency direction.

Use the existing Go communication implementation as the starting point: [bus attachment](../../../server-go/bus/client_connect.go), [concurrent module calls](../../../server-go/bus/concurrent_module_caller.go), [module serving](../../../server-go/bus/module_runtime.go), the [memory handler](../../../server-go/modules/memory/memory.go) and its [live bus probe](../../../server-go/modules/memory/cmd/aimee-memory-bus-probe/main.go). Reuse their authenticated invocation, correlation, deadlines, cancellation, bounded payloads and attach/close lifecycle. The probe demonstrates the Go caller path; its test-only principal is not a production grant. Pin the reference revision and contract fixtures before porting consumers. Do not create a second bus or wrap the C clients in Go.

Treat `src/kb/db2_adapters/kb_service_backend_context.c`, CLI/MCP/HTTP handlers and legacy memory ABIs as migration entry points. Migrate memory-specific caller integration and communication into Go together with the corresponding behavior, then delete the C implementation and declarations. Moving memory framing or compatibility translation into a host directory is not a completed migration. Existing non-memory host/provider integration and the generic bus substrate retain their owners; neither may hide memory-specific C framing, decoding, scope interpretation or fallback policy. Preserve supported user-facing commands and responses through Go entry points, not through a retained memory C ABI. A missing Go owner returns an explicit unavailable result.

Each slice adds Go domain and communication tests and exercises the actual memory process in both placements, including restart, cancellation and unavailable-module behavior. Port the relevant C framing fixtures into Go before removing their old registrations. Update the descriptor, process schema and maintained memory guide with the implementation. Replace [check_memory_c_boundary.py](../../../scripts/check_memory_c_boundary.py)'s C allowlist with a zero-C gate and scan retired symbols, native callers and build registrations across the repository. MR-18 records these ownership gates alongside behavioral parity.

### G0: Replace the remaining C with Go

The pinned source inventory contains 10 `.c` files and 14 `.h` files under `src/modules/memory`, including its public include tree. This is remaining migration work even where the descriptor currently says `ownership_complete`. G0 is a foundation slice within this program, not an additional feature proposal. Assign every file, exported symbol and production caller a Go replacement or deletion disposition. Record the target Go owner, reference contract, parity fixture and cutover dependency; relocation to another C directory is not a disposition.

| Existing files under `src/modules/memory` | Required disposition |
|---|---|
| `gw_stage_memory.c`, `gw_stage_memory.h` | Port memory invocation, recall decisions and memory-specific gateway integration to Go. Separate any unrelated host behavior under its existing owner without preserving C memory calls. Switch production entry points and delete the memory C stage and declarations. |
| `memory_data_bus.c`, `memory_domain_bus.c`, `memory_domain_runtime_bus.c`, `memory_bus_context.h` | Replace request/reply framing, scoped context propagation and legacy translation with Go clients using the existing Go bus. Convert every caller, port framing/failure fixtures and delete the C adapters and declarations. |
| `memory_content_gate_bus.c`, `memory_embed_bus.c`, `memory_extract_patterns.c`, `memory_fact_gate.c`, `memory_pii_gate.c`, `memory_extract_patterns.h`, `memory_fact_gate.h`, `memory_pii_gate.h` | Use Go communication for gates, extraction and embedding, whose domain operations already have Go implementations. Replace C callback registration and wire clients, preserve fail-closed behavior and delete their native declarations. |
| `memory_scope_connection.c` | Replace memory-specific connection/context binding with authenticated Go invocation and the existing Go storage-bus client. Preserve transaction-local scope and pooled-connection isolation; do not move memory binding into another C owner. |
| `include/aimee/memory/module_api.h`, `include/aimee/memory/pii_provider.h` | Make Go request/result types and versioned protocol fixtures authoritative. Convert consumers and delete the C wire/client headers; do not relocate or generate replacement memory C headers. |
| `memory_activation.h`, `memory_assemble_util.h`, `memory_core_internal.h`, `memory_graph_fusion.h`, `memory_ontology.h`, `memory_platform.h`, `memory_profile_pack.h` | Inventory live declarations and inline implementations. Port required behavior, caller contracts and tests to Go; delete obsolete helpers and native declarations. |

Update all production/test callers, Make/CMake registration, installed header packaging and module-bus inventories together. Remove native sources, headers and C tests from the memory descriptor; schema/descriptor resources may remain. Do not keep symlinks, forwarding headers, copied implementations, cgo wrappers or externally hosted C memory clients to satisfy old paths. Any temporarily unconverted caller remains explicit unfinished G0 work and prevents ownership closeout.

Implementation progress: PR #2984's Go client is integrated into this branch. The
next removal slice deletes the unused native extraction and fact-gate callbacks,
their Server registrations, and the obsolete inline context-assembly helpers.
The Go client retains gate/extraction conformance against historical fixtures.
The next live cutover also removes the PII callback layer: Go now supplies a
combined fact-write decision and classifies queries inside typed-fact recall.
Gate/PII binary framing and live decision smoke coverage now have Go callers.
Private-memory public commands now validate arguments and shape replies in Go
through the shared command dispatcher. Explicit shared-KB routing and its native
client remain pending; the command registry has not replaced that surface.
KB recall now decodes activation snapshots and applies cooldown, delay, sticky
relevance, suppression, and graph backfill in Go. PostgreSQL regressions exercise
both the selector and the public command. The native activation contract and
its discarded-snapshot wrapper are removed; DB1 still owns persisted turn state.
Prospective-memory, directive, lint, and maintenance command handling is now Go,
including reminder/directive dashboards and session-start rendering. The native
CRUD adapters and duplicate directive SQL are removed. Their PostgreSQL tests
cover lifecycle, deduplication, list caps, matching, and dry-run scheduling.
Unused background-embedding C hooks and their suppression state are retired.
Content screening is shared by both Go placements; the native content-gate
source, header and platform build entries are removed.
Scoped and visible fact-search commands now run through the Go command owner;
their native KB handlers and response builders are removed. Packaged-schema
tests exercise these commands under the restricted runtime role.
Model edits preserve history, scope tags and lineage without inheriting user
provenance. Public edits capture extraction authority in the same transaction;
audit triggers receive the verified initiator and the effective content authority.
The public KB `memory.ask` handler now runs in Go, including session-cluster
selection, citations, event/temporal extraction, abstention and curated exemptions.
Restricted-role replay verifies project isolation and the configured gate. Answer
counters are owned by the Go process and included in recall metrics. Confidence
is display metadata and no longer changes the Go text-ranking score or ordering.
Native ask and diagnostic decoding adapters and their backend handlers are removed;
the benchmark reads diagnostic JSON through the generic command dispatcher.
Host dashboard formatting, fusion-state reads and recall metrics now run through
the shared Go runtime command. Saved maintenance summaries, counters and config
are rendered by Go; the native formatter and state/metrics adapters are removed.
The internal command is absent from public discovery and rejects non-host callers.
Vector rebuild/reindex command handling is now Go. Rebuild validates the deployed
dimension and ANN index, uses a transaction advisory lock, and atomically clears
derived vectors, queues replacements and records the version. Runtime callers
perform no DDL or TRUNCATE. Restricted-role replay covers competing rebuilds,
dimension mismatch and rollback after a late failure; native rebuild adapters
are removed. Historical generation cutover remains later MR-11 work.
CLI and MCP answer consumers now read the Go result through the existing generic
authenticated KB transport. The native answer decoder, structs and enum formatters
are removed; MCP preserves long answers and new trace fields without truncation.
A native presentation test covers scoped requests, abstention, malformed replies
and unavailable transport without duplicating the Go answer policy.
Vector repair now runs in Go, including single/all/failed-only modes, bounded
retry configuration and stuck memory/code-job reset. Per-record transactions
preserve progress; vector-write savepoints retain the prior vector on SQL failure
while recording retry diagnostics. Successful jobs use the existing `ok` state.
The native repair handler, wrappers and backend enumeration helpers are removed.
Episode cards now honor the configured cognifier and enable flag, enforce source
and subprocess output bounds, and retain parent/unit lineage and summary links.
CLI and session-close callers consume the Go command directly; card listing stays
inside the request's visibility scope. The runtime C adapter is deleted. Vector
search runs through the host-only Go command and applies explicit scope filters;
PostgreSQL coverage includes both 1024- and 2560-dimensional columns and rejects
mismatched writes without losing the previous vector.
Vector verification is Go-owned too. It reads the same advisory lock and schema
metadata as rebuild, reports actual catalog indexes, preserves unmeasured lag,
and bounds configured-embedder timing probes. Scoped failed-job details exclude
other projects. CLI JSON preserves the complete Go response; native verification
handlers, collectors, schema-version helper and snapshot source/header are retired.
Directive promotion and curiosity routing now use the host-only Go owner;
the duplicate DB2 directive store and header are deleted. CLI and MCP use the
generic authenticated transport while retaining the Go-backed screen before
transmission. The native directive decoder and formatter are removed, fixing
CLI rejection of valid public responses and preserving additional JSON fields.
Native consumer tests cover promotion audit, screening, unavailable screening,
and response preservation; public caller evidence remains ignored.

Scene listing and member lookup now use the public Go owner and generic CLI
transport. Visible active parents are filtered before limits, full member keys
are preserved, and member lookup is capped at 512. The native scene handlers,
DB2 JSON builders, typed clients and obsolete scene API header are removed.

Session folding now runs entirely in Go, including learning-evidence capture
and embedding/synthesis queue writes. Sources retain their scope; mixed scopes,
non-active sources and sessions above 64 rows are refused. Checkpoint, lineage,
evidence and source removal share one SQL statement. Restricted-role replay
covers complete digests, refusal and rollback when a queue write fails.

Accepted learning proposals now invoke the host-only Go memory owner for
feedback, replacements and workflows. Negative feedback records a corrected-by
relation; workflow reinforcement uses canonical scoped writes, model provenance,
a 0.8 confidence ceiling and rejection checks. Workflow identity updates are
serialized per scope/key. Go packaged-schema replay replaces the old mutation
fixtures; the native proposal-consumer test checks routing and failure handling.

Native record readers now consume Go records through generic host dispatch.
Demotion, learning targets, benchmark diagnostics and retrieval provenance no
longer use the fixed memory ABI. Reference capture retains the stored version
and provenance retains source_session; both fields were being discarded by
the legacy adapter. The CLI cognifier reads full content through the authenticated
KB transport. Session pruning also invokes the shared Go maintenance command.

Default fact search and adaptive limits now run in Go. Explicit limits bypass
sampling; automatic retrieval honors promoted limits, bounded sidecar output and
the exploration budget. Policy telemetry uses savepoints so failures cannot abort
retrieval. Reward closure and posterior updates are atomic and idempotent.

Derived text indexing now rebuilds aliases, entities, temporal references,
headlines, event frames and overlapping chunks in Go. Reindex preserves authored
summaries/events, applies request scope, skips retired parents and rolls back all
index replacement when any write fails. Chunks advance across UTF-8 boundaries;
relative dates use UTC and month abbreviations map to their calendar month.
Graph-unit, relation, coreference and unit-vector rebuild parity remains open.

Derived graph units and edges are now Go-owned. Reindex reuses unchanged unit
IDs, preserves episode cards and lineage, removes stale unit points/jobs and
limits session/supersession links to matching parent scopes. Unit vectors use
the existing scoped Go repair path, including retained vectors on failed writes
and retry limits. Collection rebuild queues parents and units; a partial scope
cannot reset the whole collection. Runtime grants expose only identity/version
columns needed by dependency freshness checks, not file or outcome content.
Episode-relation and coreference rebuild parity remains open.

Episode and event/link relation indexing now runs in Go. Matching legacy C
episodes and their generated relations are adopted once through lineage; later
rebuilds replace only generated rows. Authored/cognified relations and custom
episodes survive. Episode and summary identities remain stable across rebuilds,
obsolete generated facts are removed, and inaccessible linked content cannot be
copied into a visible parent. Summary replacement also retains its dependency
registry identity. Coreference and negation refresh remain open migration work.

Coreference and negation refresh now run in Go. Heuristic and configured-model
coreference use bounded, same-session, same-scope active history; disabled or
failed resolution clears obsolete bindings, and audit writes commit atomically.
The native coreference API and C negation tokenizer are retired. Negation recall
uses the existing PostgreSQL index plus bounded polarity-overlap ranking while
retaining scope priority; positive-query ranking is unchanged. Runtime replay
covers opposite-polarity twins, additional negative candidates and scope/state
exclusions. The former derived-metadata C entry point is removed.

The shared Go owner now runs the KB metadata/vector indexer alongside the
placement-specific personal index. Canonical writes durably enqueue a new job
generation in the same transaction, including edits, scope/lifecycle changes and
deletions. Metadata replacement is atomic; failed attempts preserve the previous
index and use bounded retries with backoff. Both parent and unit vectors are
processed under the parent lock, preventing concurrent edits from being overtaken
by an old embedding. Retired/deleted parents lose their queued and stored vectors.
Restricted-role replay covers automatic indexing and retry rollback; two-connection
tests cover competing owners, crash rollback and the embedding/edit lock order.

The current inventory is four C sources and seven headers; the table above
records the original pinned inventory, and G0 remains incomplete.

G0 completion requires no native files in either memory implementation tree, no C entries in the memory descriptor and no memory-specific C communication or implementation elsewhere. Build the memory executable and Go caller tooling with `CGO_ENABLED=0`; inspect their dependency closure as well as the source inventory. Exercise supported CLI/MCP/HTTP/bus operations through Go communication in both placements, then run repository-wide source, descriptor and build-registration checks. Prove unavailable-module, malformed-response, unsupported-version, cancellation, deadline, restart and concurrent-call behavior. A successful pure-Go module build does not certify unconverted C callers. Later feature slices must preserve this boundary.

## Serving sequence

1. Authenticate identity, scope and purpose; choose the explicit temporal mode.
2. Retrieve bounded authorized arm pools with declared index state.
3. Apply eligibility, fuse/deduplicate and derive task requirements.
4. Select coherent evidence, apply bounded optional priors/diversity and produce the model projection.
5. Serialize, enforce hard budgets and evaluate requirement coverage over the exact retained evidence.
6. Perform only bounded, authorized recovery when necessary; revise the plan explicitly.
7. Reauthorize release, bind final provider-shaped bytes and durably append the governed preparation receipt.
8. Dispatch and record acknowledgement or uncertainty; use the context contract only within operator policy.
9. Attribute actual applications/outcomes and update scoped health/experience projections.
10. Maintain derived state and generate reviewable hygiene proposals without autonomous canonical rewrites.

This is a logical order across existing owners. It does not require a new process, database or synchronous external audit service at every stage. Where durability is required, the existing audit/WORM pipeline must acknowledge its specified durable boundary before dispatch.

## Initial serving-surface inventory

This source inventory is pinned to PR revision `b6c2cfa58c8f301b3a422cb8bf1592ea575c8473`. It identifies existing entry points and review obligations; it is not a passing conformance report. The [memory module guide](../../modules/memory.md) defines personal Server and shared KB placement ownership. MR-18 slice 1 expands these rows into operation/placement cases and records unsupported or unverified capabilities explicitly.

| Surface | Existing entry point / owner | Baseline behavior and required parity work |
|---|---|---|
| CLI | [cmd_memory_core.c](../../../src/cmd_memory_core.c), `mem_recall` and memory command handlers | Recall consumes a client envelope and renders sections. Pin store selection, scope/activation parameters, temporal modes and retained IDs per command. |
| MCP | [server_mcp_call_table.c](../../../src/server/server_mcp_call_table.c), `mcph_memory_recall`; [server_mcp.c](../../../src/server/server_mcp.c), search and briefing tools | Recall selects personal memory by default or shared memory explicitly. Test each tool's authenticated scope and declared capabilities; transport parity does not imply provider dispatch. |
| HTTP | [server_api.c](../../../src/server/server_api.c), `memory_recall_handler` | Personal recall is the default; `store=kb` selects shared recall. Pin effective token limits, scope derivation, invalid parameters and unavailable-store responses. |
| Memory event bus | [memory_data_bus.c](../../../src/modules/memory/memory_data_bus.c) and [data.go](../../../server-go/modules/memory/data.go) | The memory-data stage dispatches to the Go placement owner. Cover get/search/visible-search/bundles/facts and mutations separately under actual runtime credentials. |
| Shared KB RPC and compatibility APIs | [kb_service_memory.c](../../../src/kb/kb_service_memory.c) and [kb_service_backend_context.c](../../../src/kb/db2_adapters/kb_service_backend_context.c) | Shared recall, graph/as-of and assertion valid/belief-time requests have distinct handlers. Pin supported time modes, lexical/dense/graph arms and parameter behavior for each. |
| Typed context and ingress | [kb_service_backend_context.c](../../../src/kb/db2_adapters/kb_service_backend_context.c), typed assembly; [ingress_preinject.c](../../../src/server/ingress_preinject.c), `ingress_render_block` | Typed channels use summary estimates; outer ingress applies an envelope. MR-03/MR-05/MR-06 must agree on actual retained evidence after final packing. |
| Provider-bound context | [server_provider.c](../../../src/server_provider.c) and [model_provider.c](../../../src/server/model_provider.c), with economizer/provider adapters | Inventory every enabled dispatch route and retry transformation before assigning a final-byte guarantee. Capture exact payloads and durable preparation/admission/observed-dispatch stages per route. |
| Maintenance and legacy runtime adapter | [maintenance.go](../../../server-go/modules/memory/maintenance.go) and [memory_domain_runtime_bus.c](../../../src/modules/memory/memory_domain_runtime_bus.c), `memory_maintenance_run` | Existing maintenance can promote, retire and otherwise change records. Apply MR-02 admission/invalidation parity; MR-14's new proposal-only path does not certify existing modes as read-only. |

Each conformance row records owner/store namespace, authenticated identity source, query modes, arm readiness, requested/effective parameters, budget units and receipt stage. CLI/MCP/HTTP retrieval responses may establish assembly but cannot claim a model received the evidence. Provider dispatch is verified at its own boundary. Unknown current guarantees remain unknown until the authenticated integration fixture runs.

## Delivery waves and dependencies

| Wave | Work | Exit condition |
|---|---|---|
| 0: Baseline and contracts | Start [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md); expand the initial serving inventory into per-operation parameter cases; prepare [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md)/[MR-08](memory-reliability-08-retrieval-health-telemetry.md) event schemas | Fixed fixtures, honest baseline and identified owners/defaults |
| 1: Boundary correctness | G0 C extraction; [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-02](memory-reliability-02-authority-preserving-mutations.md), [MR-03](memory-reliability-03-final-payload-context-budgets.md); deterministic [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) requirements and [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) final-payload receipts | Memory is Golang-only; eligibility/history/caps are enforced; incomplete context and unsent attempts are labeled correctly |
| 2: Evidence and retrieval | [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md), [MR-08](memory-reliability-08-retrieval-health-telemetry.md), candidate/prior slices of [MR-09](memory-reliability-09-fair-hybrid-ranking-and-exposure.md), [MR-10](memory-reliability-10-deterministic-utility-horizons.md), [MR-11](memory-reliability-11-embedding-generations-and-index-freshness.md), [MR-12](memory-reliability-12-served-memory-views-and-claim-cards.md); complete [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md)/[MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) integration | Independent support, lineage, fair candidates, readiness and views are inspectable |
| 3: Governed efficiency and effects | [MR-07](memory-reliability-07-task-exploration-contracts.md) observe then canary; reward-correction/attribution slices of [MR-15](memory-reliability-15-procedure-outcomes-and-task-cost.md); [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md) | Fewer redundant reads without starvation; current evidence governs exact effects |
| 4: Derived continuity and maintenance | [MR-13](memory-reliability-13-disposable-task-projections.md), [MR-14](memory-reliability-14-proposal-only-memory-hygiene.md), [MR-17](memory-reliability-17-clean-retry-context.md); measured experience/exposure/routing extensions | Temporary state stays non-authoritative; hygiene/retries preserve canonical and external history |

[MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) runs in every wave. [MR-16](memory-reliability-16-evidence-bound-actions-and-composition.md) moves earlier for any automated external-write workflow that already relies on remembered evidence. Health collection can start with existing final-selection events; any unavailable dimensions must be labeled missing until their producing proposal lands.

Implementation work may proceed in parallel once interfaces are fixed, but deployment dependencies remain explicit. In particular, restrictive [MR-07](memory-reliability-07-task-exploration-contracts.md) enforcement waits for truthful [MR-05](memory-reliability-05-context-sufficiency-and-bounded-recovery.md) coverage and [MR-03](memory-reliability-03-final-payload-context-budgets.md)/[MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md) final context evidence. Independent-support requirements wait for [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md); simpler deterministic requirements can ship earlier.

### Slice-level deployment gates

`MR-NN/Sn` names the numbered implementation slice in that proposal. These gates refine the proposal-level dependencies: interfaces can be designed earlier, but a consuming slice cannot advertise a guarantee until its producer and applicable MR-18 tests pass. References to later consumers do not make those consumers prerequisites for the shared foundation.

| Deployable slice or group | Required predecessor / exit evidence |
|---|---|
| MR-18/S1; MR-06/S1 and MR-08/S1 schema work | No feature prerequisite for fixtures or schema design. Record baseline omissions explicitly; do not synthesize missing lineage or dispatch evidence. |
| G0 extraction and Go memory integration in every slice | MR-18/S1 file/symbol dispositions first. Replace all memory C implementation, communication and callers with Go, using the existing Go bus reference; pass repository-wide zero-C-memory and `CGO_ENABLED=0` gates before ownership closeout. Version contracts and test both placements in each later slice. |
| MR-01/S1–S3; MR-02/S1–S3 | MR-18/S1 fixtures first; MR-02 consumes MR-01 authorization vocabulary. MR-02/S3 delivers durable invalidation and scoped collection-change tracking before dependent caches/indexes are enabled. |
| MR-03/S1–S3 | MR-01 eligible-candidate contract; final-request capture from MR-18/S2 before endpoint cap enforcement. |
| MR-05/S1–S2 deterministic coverage | MR-01 decisions and MR-03 final retained IDs/spans. Independence requirements remain unavailable until MR-04/S1–S2. |
| MR-06/S2–S4 final receipts | MR-03 final bytes and MR-05 coverage; MR-18/S2 crash/handoff capture. Full family/index diagnostics join as MR-04/MR-11 land, with unavailable dimensions labeled meanwhile. |
| MR-04/S1–S4 | MR-01 release checks and MR-02 durable mutations/invalidation. Parent lineage and collection dependencies precede cross-owner erasure claims. |
| MR-05/S3 bounded recovery | MR-05/S1–S2 coverage and host task/access budgets; MR-04 support projections before recovery promises independent corroboration. Record final plan revisions for MR-06. |
| MR-08/S1–S3 collection and reports | MR-06 observed selection/dispatch; MR-04 for family metrics. Unknown-dispatch attempts remain a separate population. |
| MR-09/S1–S3; MR-10/S1–S2; MR-11/S1–S4 | Their declared foundation contracts and MR-18 fixtures. MR-11 temporal coverage and durable replay precede generation cutover; MR-10 enforcement must preserve historical recall. |
| MR-12/S1–S3 | MR-01/MR-03/MR-05/MR-06 plus MR-04 evidence projections. Cache activation additionally requires MR-02 collection generations and current-owner checks. |
| MR-07/S1–S3, then S4 | Observe/expansion follows final coverage and receipts; enforcement waits for MR-18/S3 completion, efficiency and starvation evidence. |
| MR-15/S1–S3; MR-16/S1–S4 | Declared evidence/receipt dependencies. Reward proxy correction can precede fitting; external-write admission and uncertain-effect recovery precede automated effects that rely on those guarantees. |
| MR-13/S1–S3; MR-14/S1–S3; MR-17/S1–S3 | MR-13 follows MR-12; MR-14 follows MR-13 and uses MR-11 admission before enabling rebuild queuing; MR-17 follows MR-13 and MR-16 reconciliation. |
| MR-09/S4, MR-10/S3, MR-15/S4 and other fitted policies | MR-18/S3 held-out quality/cost evidence plus their earlier slices; promote each optional policy independently. |
| MR-18/S4 | Applicable invariant tests become required with each feature rollout; do not wait for all 18 proposals to finish. |

## Proposed configuration and compatibility

Use existing configuration ownership and validation. Add small versioned policy sections only where needed; these are proposed controls, not claims of current configuration keys.

| Control | Initial behavior | Promotion rule |
|---|---|---|
| Eligibility and mutation fixes | Correctness enforcement after migrations/conformance tests | No tuning flag may bypass the corrected invariants |
| Final payload cap and receipt stages | Enforced per converted endpoint | Legacy endpoints explicitly identify unsupported guarantees |
| Exploration contracts | Observe, with independent operator opt-in for enforcement | Paired completion/efficiency and starvation tests pass |
| Retrieval health | Bounded collection with declared sampling/retention | Expand after overhead/privacy and population-metric checks |
| Utility horizons | Shadow; enforce only explicitly configured transient kinds | Historical preservation and domain usefulness gates pass |
| Ranking/exposure/routing | Versioned baseline; optional additions independently disabled | Held-out paired ablation and hard invariant gates pass |
| Task projections | Explicit reads first; automatic preload later | Scope, expiry, non-authority and invalidation gates pass |
| Hygiene | Manual dry run / proposal creation; scheduler opt-in | No canonical mutation without existing review/admission |
| Outcome learning | Observe attributable outcomes before fitting policies | Sufficient validated feedback and fixed evaluation |

Retain advertised legacy memory semantics through Go adapters. A compatibility parameter must work, be explicitly deprecated or be rejected; it must not be silently ignored. Legacy `ingress_max_raw_scans <= 0` remains disabled behavior, while a new typed contract can express a literal zero allowance without ambiguity.

## Review checklist

For every implementation slice, reviewers should be able to identify the problem, owner, contract version, state transitions, exact failure behavior, migration strategy, changed defaults, acceptance tests, observability and rollback. Keep a small PR when a slice changes a durable boundary. Do not group unrelated optional ranking experiments with authorization or history fixes.

No proposed threshold is evidence of an achieved improvement. Freeze the [MR-18](memory-reliability-18-evaluation-parity-and-release-gates.md) evaluation policy before observing experimental outcomes. Distinguish verified effects from claims, unknown data from success, and retrospective explanation from execution-time evidence.

## Deferred experiments

Cross-encoder reranking, vector quantization, alternative ANN backends, community summaries, learned utility horizons and broader autonomous consolidation remain separate experiments. They require equal-budget quality/cost evidence and complete provenance/invalidations before production adoption. Their absence does not block the correctness program.

## Series contents

The 18 numbered proposal files are independently reviewable. This program index defines common contracts and delivery order. The [requirements coverage map](memory-reliability-requirements-coverage.md) assigns each requirement to a proposal and its acceptance gates.

All documents remain proposed. Each implementation slice requires its own review, validation and rollout decision.

The shared Go command owner also handles graph/entity queries, episodes,
provenance, links, conflicts, health, and dashboard statistics. Native domain
wrappers for these operations are retired. Derived reads are checked against
parent memory visibility, with non-owner PostgreSQL scope regression coverage.
The native caller inventory remains an explicit G0 blocker.
