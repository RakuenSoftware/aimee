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

Make the memory module Golang-only: its implementation, tests and executable belong to `server-go/modules/memory` and the supervised `aimee-module-memory` process. Its bus producer and consumer, including module-side request/reply framing, serialization and scope propagation, must also be Go. The existing bus in `src/core/event_bus` remains C; `server-go/bus` supplies Go bindings to that bus, not a replacement bus. External Server, KB and CLI hosts may retain C transport callers. Memory policy, persistence orchestration, retrieval, ranking and fallback behavior must execute in the shared Go owner, not in those callers. No C source or header may remain in the memory module or provide a relocated memory implementation elsewhere in the repository. A clean module directory alone does not satisfy this requirement. Strengthen the shared Go implementation across Server/private and KB/shared placements. Keep placement validation, scoped record identities and the PostgreSQL bus boundary; do not merge personal/shared stores or introduce a second memory service. The [module descriptor](../../../src/modules/memory/module.yaml) and [process contracts](../../../src/modules/process-contracts.json) remain the source of the shipped inventory and wire surface.

| Responsibility | Implementation boundary |
|---|---|
| Memory-side bus producer and consumer | Go clients and handlers use the existing C bus protocol through Go bindings, without cgo or a native memory implementation. The bus remains C. External C hosts may marshal requests and forward results; they do not implement memory policy or a fallback engine. |
| Eligibility, temporal rules, mutation admission and utility horizons | Go memory domain functions used by all data operations; PostgreSQL migrations, constraints and transactional storage enforce durable invariants through the existing storage owner. |
| Candidate collection/fusion, memory requirements, evidence lineage, memory projection and named views | Go memory implementation over placement-specific adapters. Migrate matching memory policy from the typed C backend into this owner as the relevant slice lands. |
| Memory indexing and collection/dependency freshness | Go memory owns admission, jobs and serving decisions for its records; coordinate storage/index generation operations with existing DB2/PostgreSQL owners over declared contracts. |
| Task state, exploration limits and action admission | Existing task/execution-policy owners consume versioned Go memory evidence and freshness decisions. Memory cannot grant tool permission or own external effects. |
| Procedure review and verified outcomes | Existing learning owner remains authoritative; memory serves governed projections and references, without acquiring a second promotion/reward engine. |
| Final provider serialization, full-request caps and dispatch receipts | Existing host/provider, economizer and audit owners consume the Go memory projection. Return final retained IDs/spans for Go memory coverage evaluation; provider-specific wrappers and credentials stay at the host boundary. |

Extend the existing memory-data contract with bounded, versioned request/result types for eligibility, mutations, projections, lineage and views. Factor domain functions out of transport dispatch and storage adapters; do not grow `handleData` into a second implementation of those rules. Adapters validate wire shape and preserve domain reason codes, effective limits, source versions and capability status. Govern new calls with the existing authenticated invocation, deadline, cancellation and payload limits. Unknown schema versions or unsupported placement capabilities return explicit errors.

Memory does not gain direct database connections or in-process imports of other feature modules to implement this series. Use existing bus/storage contracts and host-supplied, authenticated inputs at ownership boundaries; declare any necessary contract extension. Carry learning/task evidence with its original authority and dependencies. Preserve the memory module's dependency direction.

Use the existing Go bindings to the C bus as the starting point: [bus attachment](../../../server-go/bus/client_connect.go), [concurrent module calls](../../../server-go/bus/concurrent_module_caller.go), [module serving](../../../server-go/bus/module_runtime.go), the [memory handler](../../../server-go/modules/memory/memory.go) and its [live bus probe](../../../server-go/modules/memory/cmd/aimee-memory-bus-probe/main.go). Reuse their authenticated invocation, correlation, deadlines, cancellation, bounded payloads and attach/close lifecycle. The probe demonstrates the Go caller path; its test-only principal is not a production grant. Pin the reference revision and contract fixtures before porting memory-side producers and consumers. Do not create a second bus, convert the C bus to Go or wrap C memory clients with cgo.

Treat `src/kb/db2_adapters/kb_service_backend_context.c`, CLI/MCP/HTTP handlers and legacy memory ABIs as migration entry points. Move memory behavior into Go and remove duplicate native implementations. The memory process must produce and consume bus messages entirely in Go. Host-side C transport and protocol declarations may remain with their existing owners when they only call the Go owner and forward its results. Any extraction of host-only code from the memory tree needs an explicit ownership disposition and transport conformance evidence; moving memory behavior into a host directory is not a migration. Preserve supported commands, authenticated identity, exact IDs and complete responses. A missing Go owner returns an explicit unavailable result; C callers must not substitute local memory behavior.

Each slice adds Go domain and communication tests and exercises the actual memory process in both placements, including restart, cancellation and unavailable-module behavior. Port the relevant C framing fixtures into Go before removing their old registrations. Update the descriptor, process schema and maintained memory guide with the implementation. Replace [check_memory_c_boundary.py](../../../scripts/check_memory_c_boundary.py)'s C allowlist with a zero-C module gate and continue scanning retired implementations and build registrations across the repository. Audit external C callers for transport-only ownership rather than requiring every caller to change language. MR-18 records these ownership gates alongside behavioral parity.

### G0: Replace the remaining C with Go

The pinned source inventory contains 10 `.c` files and 14 `.h` files under `src/modules/memory`, including its public include tree. This is remaining migration work even where the descriptor currently says `ownership_complete`. G0 is a foundation slice within this program, not an additional feature proposal. Assign every file, exported symbol and production caller a disposition: Go memory implementation, deletion, or an explicitly justified external host/storage protocol contract. Record the owner, reference contract, parity fixture and cutover dependency. Memory behavior and module-side communication must move to Go; moving either to another C directory is not a disposition. The C bus and unrelated C modules are outside this language migration.

| Existing files under `src/modules/memory` | Required disposition |
|---|---|
| `gw_stage_memory.c`, `gw_stage_memory.h` | Port memory invocation, recall decisions and memory-specific gateway integration to Go. Keep unrelated host behavior and transport calls under their existing C owner. Switch production entry points and delete the memory C stage and declarations. |
| `memory_data_bus.c`, `memory_domain_bus.c`, `memory_domain_runtime_bus.c`, `memory_bus_context.h` | Put all module-side framing, scope handling and request/reply processing in Go using bindings to the existing C bus. Remove native memory implementations and declarations. Classify remaining host-only transport separately and preserve its framing/failure fixtures; it cannot retain memory policy. |
| `memory_content_gate_bus.c`, `memory_embed_bus.c`, `memory_extract_patterns.c`, `memory_fact_gate.c`, `memory_pii_gate.c`, `memory_extract_patterns.h`, `memory_fact_gate.h`, `memory_pii_gate.h` | Use Go communication for gates, extraction and embedding, whose domain operations already have Go implementations. Replace C callback registration and wire clients, preserve fail-closed behavior and delete their native declarations. |
| `memory_scope_connection.c` | Replace memory-specific connection/context binding with authenticated Go invocation and the existing Go storage-bus client. Preserve transaction-local scope and pooled-connection isolation; do not move memory binding into another C owner. |
| `include/aimee/memory/module_api.h`, `include/aimee/memory/pii_provider.h` | Make Go memory request/result types and versioned protocol fixtures authoritative. Remove native headers from the memory module. External C hosts may retain only the protocol identifiers/declarations needed to call it, under host ownership and conformance tests; no native memory implementation or module-side adapter may hide there. |
| `memory_activation.h`, `memory_assemble_util.h`, `memory_core_internal.h`, `memory_graph_fusion.h`, `memory_ontology.h`, `memory_platform.h`, `memory_profile_pack.h` | Inventory live declarations and inline implementations. Port required behavior, caller contracts and tests to Go; delete obsolete helpers and native declarations. |

Update affected production/test callers, Make/CMake registration, installed header packaging and module-bus inventories together. Remove native sources, headers and C tests from the memory descriptor; schema/descriptor resources may remain. Do not keep symlinks, forwarding headers, copied memory implementations or cgo wrappers to satisfy old paths. Any remaining C memory behavior or module-side producer/consumer prevents ownership closeout. External C bus callers alone do not.

Scope clarification (2026-09-19): this boundary follows the explicit requirement to keep the C bus and unrelated C modules in C. Earlier progress entries below used a broader all-native-callers criterion. The immutable `check_memory_go_only.py --report` inventory remains unchanged and still reports that broader criterion; its total is not a count of C memory implementations. Classify its findings against this boundary before closing G0. The final native-file cutover now leaves zero native files in both memory trees and zero native entries in the memory descriptor. The [module guide](../../modules/memory.md#compatibility) records each disposition: benchmark-host transport stays C outside memory, host wire identifiers and DB2 persisted graph codes have their external owners, and the obsolete internal header is deleted. No C bus implementation changed. `check_memory_c_boundary.py` and `check_memory_go_only.py --module-only` enforce the language boundary; the broad default audit remains separate and unchanged. CGO-disabled builds and live C-bus process tests cover both placements. Go lane metrics replace the orphan native fixture. The native performance harness reports its removed memory cases unavailable instead of certifying missing measurements. Full historical behavior and performance parity, including legacy PageRank timing and unit/temporal semantic weighting, remains distinct unfinished work; this checkpoint does not claim every G0 parity or numbered proposal gate has passed.

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

The native prospective-memory client source has been deleted. CLI, MCP, agent
and review-reminder consumers now use generic authenticated commands for reminder
CRUD/matching, scope ranking/tagging and provenance. Reminder creation still
screens locally through the Go content gate before transmission. Agent reminders
and CLI/MCP provenance consume full JSON strings rather than fixed native buffers.
Native consumer tests cover refusal, redaction, gate outages and long responses;
Go tests retain matching, expiry and once/repeat lifecycle coverage.

Shared graph recall now uses a Go breadth-first traversal with a total 192-node
budget, bounded neighbor reads and two-hop depth. It restores direct shared-entity
bridges, relation/structural/observation weighting, provenance-class weighting and
UTC utility decay. Code-shaped queries gate code traversal; inactive projections
and unpromoted semantic facts are excluded. Parent visibility applies to seeds
and returned memories, and scope priority precedes result limits. Equal graph
scores preserve text ordering. Legacy graph feedback/diagnostic integration is
still part of the remaining migration.


The native repair/rebuild/reindex, maintenance and lint clients are retired.
CLI and MCP consumers use generic authenticated commands; reindex retains its
five-minute budget and vector repair/rebuild retain ten minutes. Transport tests
verify bearer authentication, explicit scope, operation budgets and refusal bodies.
CLI vector failure messages no longer read from a freed response.


Versioned re-embedding now runs in Go. Draft vectors retain their exact parent/unit
input hashes and provider configuration; failed attempts and resumed runs preserve
completed drafts and the live index. Cutover/rollback checks every current input
and pending metadata job under the rebuild lock, then switches vectors and the
active provider in one transaction. Unrelated semantic vectors survive. HTTP
provider identity is checked before and after embedding; legacy command versions
bind the configured command and dimension. Ordinary indexing uses the active
provider and maintains its retained vectors. Rollback requires a retained version
that covers current inputs; untracked legacy vectors cannot be relabeled as a
historical version. The DB2 schema version is 7.

The native embed/re-embed handlers, version-query helpers and typed clients are
retired. Host-only text embedding is `memory.embed_text`; the public
`memory.embed` command uses scoped Go storage. Normal CLI embed/repair operations
let the Go owner select the active provider. CLI progress preserves 64-bit IDs and
reports readiness, incomplete work and pending metadata without claiming a failed
batch is complete. Vector mutation routes require index-admin capabilities at Server.

The graph/entity/episode typed KB clients are retired. CLI as-of graph search
and the actual MCP tools use generic authenticated commands and retain full JSON
strings. MCP rendering preserves long results and dependency/authorization errors;
episode reads now carry the same caller scope as graph/entity reads. PostgreSQL
regression coverage rejects cross-project episode access through the public route.

The KB HTTP entity routes and typed-context episode/summary channels now call
the shared Go owner directly; the native graph/profile/episode domain adapters
are retired. Graph and episode reads require active, unsuppressed parents, and
local scope ranks ahead of global scope before result limits. Entity profiles
count distinct memory mentions and current semantic assertions, exclude hidden
memory-backed evidence, and return an explicit miss for unknown entities. Runtime
context queries preserve exact scope, full text and host-only access. HTTP reads
retain the verified request context, preserve long JSON strings and distinguish
missing entities from unavailable services. Failed context channels report degraded
status instead of an empty success.

The separate native JSONL/duplicate-check client source is deleted. Actual data
CLI calls use the authenticated generic command transport. Export errors stop the
command; an unavailable or malformed duplicate check cannot turn skip into an
overwrite, and refused writes cannot be counted as imported. Go key-existence
queries now honor caller scope. Native CLI tests cover these failure paths, while
PostgreSQL checks cover scoped duplicate visibility.

Cognification now runs in the shared Go owner. The CLI and benchmark drain
callers use generic commands; the native result and queue API is retired. Model
execution is bounded, source ancestry is checked, and kind/relations/claims/actor
provenance commit atomically. Claims inherit canonical source scope and model
authority; private preferences cannot leak through the global rule channel.
Shared queue attempts lock the parent and generation, roll back partial writes,
retry at most three times and execute synchronously even when enqueue mode is
on. Queue reads respect source scope. Direct Go canonical writes also enforce
the shared content gate, closing the native-client bypass. PostgreSQL replay and
actual CLI tests cover these contracts, redaction, tombstone rollback and full
text. The old Go DB1 queue remains a migration concern for pre-existing jobs;
new work is owned by the shared KB memory queue. The unused native DB1 queue
bindings and their fixed-width client header are also deleted; queue behavior
is exercised through the Go owner and PostgreSQL.

Console memory review now uses generic Go commands with the authenticated
operator context separate from action JSON. Reject creates the canonical scoped
refusal and suppresses the memory, rather than merely lowering confidence; the
old reject/restore adapters are deleted. Restore retains the verified actor.
Restricted-role replay covers scope isolation, blocked re-extraction, review
visibility and restore, while native HTTP tests distinguish missing/forbidden
responses from unavailable or malformed replies and reject fractional IDs.

Graph traversal, code-vector seed resolution and path feedback now share the
Go owner. Seeds require current project generations; hidden memory evidence
cannot provide an intermediate graph bridge. Diagnostics retain raw graph and
code-proximity scores, and host-owned feedback conserves credit, clamps utility
and excludes authority-controlled semantic facts. The obsolete graph/fusion C
header and ported C fixtures are removed; DB2 provenance coverage remains.
Schema version 8 adds only code identity/generation read grants for the runtime
role, without exposing vector payloads or project writes.

Typed-fact extraction completion now commits through the Go memory owner.
Canonical entity aliases, ontology admission, normalized identities, authority
quarantine, functional corrections, evidence replay and WORM seals share one
transaction. Restricted-role tests exercise deferred evidence guards and prove
that a denied audit seal rolls back facts and queue acknowledgement. Legacy
identity matching is read-only until an assertion is actually mutated, and
rejections survive alternate Unicode spellings.

Extraction leases now carry generation, nonce and source hash. Edited memories
requeue completed jobs; stale completion cannot acknowledge a newer lease or
attach model output to changed/suppressed sources. Pattern and model facts use
valid world-fact kinds, retain distinct authority, and retry errors omit provider
content. The native worker no longer commits candidates; it still supplies the
existing curator connection. Other native fact-ingest/review paths remain G0
work. Schema version 9 adds the bounded runtime writer's evidence/ontology
grants while WORM chain writes remain unavailable to it.

Typed-fact operator review and candidate listing now execute in Go through
verified host commands. Approve/reject/undo preserve functional incumbents,
authority, tombstones, review history and atomic WORM seals. Undo refuses to
clobber subsequent independent changes, including changes to an incumbent it
would restore; failures roll back partial transitions. Candidate reads and
reviews filter memory-backed evidence by source visibility and lifecycle.
The native candidate-list API is deleted, and HTTP tests cover verified actor
transport, malformed responses and distinct missing/conflict/outage results.
Schema version 10 grants the Go owner access to the review journal. The old
native review implementation remains only with legacy lifecycle fixtures until
the dependent rollback/invalidation coverage is ported.

The public context-block and typed-fact recall routes now execute entirely in
Go. Query retraction remains structurally model-authority, including inside an
authenticated user session, and reports annotate-only/operator-only refusals.
Allowed corrections commit history, tombstones and WORM seals atomically; an
oversized matching set fails without a partial retraction. Fact recall now
filters hidden or inactive memory-backed evidence, closing a private-source
leak. The C endpoint composition, pattern-ingest/recall implementations, host
providers and their obsolete ABI fixtures are removed. Go tests retain pattern
seed-kind correction and sensitive recall coverage; restricted-role replay
covers public context authority, scope, historical evidence, bounds and rollback.
The KB now declares 97 commands; the Server still declares four. Native fact
rollback, other mutation consumers and gateway integration remain G0 work.

The native context-block and typed-fact KB clients are also retired. MCP and
prompt injection use the generic authenticated command transport with the
existing request scope. MCP preserves full context text and explicit refusal
bodies; automatic injection rejects malformed or failed responses and reports
unavailable recall. Actual MCP, ingress, gateway and transport tests preserve
scope forwarding, long responses and the rendered envelope contract.

Explicit `facts.retract` is now a Go-owned command. Requested authority can
only lower verified caller authority; anonymous and remote-owner requests stay
model-authority even when their JSON asks for user privileges. Verified users
retain the existing right to retract immutable birthplace and parent/child facts,
while model-generated context queries cannot. Selectors retain target filtering
and relation normalization, corrections retain history/tombstones/WORM seals,
and scoped requests cannot mutate hidden memory-backed facts. The native KB
retraction handler, duplicate authority resolver and DB2 JSON adapter are deleted.
The KB now declares 98 commands. Legacy native mutation implementations remain
with dependent rollback fixtures and are still part of the unfinished G0 work.

The obsolete C fact-invalidation implementation and lifecycle retraction API
are now deleted. Go restricted-role replay covers the remaining native cases:
nonfunctional sibling values, model/user alias corrections and all three
immutable relations. Native lifecycle/graph fixtures retain their independent
review, traversal and rollback checks and pass after the retirement.

The Server's typed fact-retraction client is retired. Its actual consumer
forwards through the generic authenticated transport, preserves the verified
account authority cap and content-free audit notification, and retains the Go
refusal envelope. Missing/malformed replies are unavailable rather than not-found;
invalid counts cannot become success. Runtime-web maps explicit conflicts to
HTTP 409. Native consumer tests cover authority, valid zero-count responses,
refusals, malformed counts and HTTP classification; Go tests cover the status map.

Confidence display thresholds now run through the shared host-only Go command.
The native adapter validates the response and exposes the existing label. All
unused fixed-wire extraction, scanning and confidence codecs are deleted from
the native module header; only remaining stage identifiers and a relation bound
remain. Go boundary tests, both live placements and all 77 lint checks pass.

Server private-memory commands, dashboards and session briefings now resolve
through named host command discovery instead of fixed memory stage IDs. Go
rejects the wrong placement and non-host peers. The generic Server envelope
adapter retains HTTP classification and overrides untrusted operation fields
without mutating caller arguments. Native consumer tests preserve long briefing
text and reject malformed/error responses; PostgreSQL tests exercise the named
briefing/dashboard routes. Both live placements and the private scope tests pass.

Gateway session-start detection, exact shell-tool selection, enable-token parsing
and recall-gate telemetry now live in Go. Host-only plans preserve compaction and
second-turn behavior. Native IR code applies a validated descending index patch;
malformed patches cannot partially remove tools. Recall audit payloads use query
fingerprints rather than raw user text, and audit failure cannot change the gate.
The native session-start, persona-wrapper and recall-counter APIs are retired.
Generic named calls now accept an explicit invocation budget: fast gateway and
confidence decisions use 500 ms, and migrated private commands/views retain
60 seconds. Tests cover authority-separated rendering, complete responses,
fail-open recall, concurrent Go counters, invalid patches and protocol/IR seams.
The gateway transport and remaining pre-injection policy are still G0 work.

The native gateway memory stage and header are deleted. Structured ingress and
legacy text handlers now execute Go context-injection plans through the generic
IR module-plan executor. Go selects query precedence/bounds, gate/audit behavior,
guidance/evidence ordering, typed authority and tool removals. The executor only
validates/applies IR edits and invokes explicitly supplied host connections;
existing authenticated KB retrieval and audit transport remain in those bindings.
Malformed plans cannot partially change tools or perform earlier effects, and
retrieval-origin text cannot acquire instruction authority. Native tests retain
full text, 64-bit epochs, cache metadata and protocol parity. A real trusted-host
bus fixture verifies Go plans in both placements. An unavailable plan produces
no injection; recall-gate off/observe/enforce behavior stays in Go.
Three C sources and five headers remain in the memory tree. The strict immutable
G0 audit still reports 254 violations across the remaining files, callers and
build registrations; pre-injection policy and legacy clients remain migration work.

Task-conditioned ingress now uses Go for its bounded 64-session history,
atomic first-task claims, low-overlap task changes and recovery after unavailable
KB retrieval. The same Go owner validates the complete task packet before
rendering its bounded prefix, so a large earlier row cannot conceal later
foreign/stale evidence. Packet identities and spans reject fractional/overflow
values; long project identities and UTF-8 truncation remain intact. Native
retrieval continues on the authenticated, scoped KB connection. Native fixtures
invoke the real Go handler, and live trusted-host tests cover claims, recovery
and packet rendering in both placements. An unavailable Go task owner suppresses
the task packet. Other pre-injection assembly/rendering and native communication
remain G0 work; this change does not complete the migration.

The shared Go owner now assembles ingress envelopes: code compression, escaped
memory previews, grouping, byte budgets, omissions, fact/temporal/audit sections
and confidence all run there. Native renderer types/helpers and the separate
confidence provider are deleted. Go ports retain the renderer goldens, and the
actual native builder reproduces its prior full-envelope fixture through the
real Go handler. Decimal transport preserves full int64 memory IDs. The native
integrity gate examines the final envelope, unavailable assembly yields no
injection, and failure to mint an audit event now releases request scope.
Live tests exercise assembly in both placements. Retrieval selection, audit
policy and transitional native transport still prevent G0 completion.

Go ingress plans now select scoped retrieval channels, strict/observe/off mode,
request opt-outs, compression overrides and usable budgets. Task completion in
Go enforces the two-second latency limit, observe-mode invisibility and retry
rearming; unavailable-versus-empty recall metrics are atomic Go state. Native
callers only capture request/config context and invoke the planned connections.
The native task-format/reset/counter APIs are retired. Budgets too small to
inject now avoid retrieval and leave the first-task claim available for a later
usable request. Mode/scope matrices, concurrent telemetry, native byte parity
and live authenticated-host checks pass in both placements. Audit/placement
policy and legacy native clients still remain G0 work.

Go canonical mutation observations now follow their owning transaction, including
background cognification. Commit releases the bounded ACTION batch; transaction
rollback and SQL savepoint rollback discard observations for reverted writes.
The KB memory audit bridge and native memory hook API are removed. A Go producer
is tested through the authenticated daemon bus into the real audit ledger, and
restricted-role replay covers failed work whose retry bookkeeping still commits.
Publication acknowledges enqueue only; transactional SQL WORM remains the durable
mutation record. Server pre-dispatch refusal hooks and other native memory
clients still require migration. The current inventory is three C sources
(353 lines) and five headers in the memory tree, with 169 strict repository-wide
G0 findings; the immutable baseline is unchanged and G0 remains incomplete.

Workflow observation policy now lives in the shared Go memory owner: literal
shell invocation recognition, configured-workspace matching, signal precedence,
rule generation, privacy screening and write-receipt validation. Observed shell
commands and explicit MCP stores use the same owner; the host captures local
context and transports the Go-generated request through the authenticated KB
connection. Only a successful canonical receipt enables the existing learning
notification. Exact decimal int64 IDs survive receipt rendering, and malformed
or refused writes cannot report success. The C parser and native workflow
upsert client are deleted; their tests are ported to Go, with actual native host
connections tested against the real Go handler and canonical writes replayed as
the restricted PostgreSQL role. Strict G0 remains incomplete, with its immutable baseline unchanged. Three C sources (353 lines) and five headers
remain in the memory tree, along with other native consumers outside it.
Wiki selection/rendering is now Go-owned, with full content, scope before
500-row page caps, explicit output-size/refusal handling and no DB1 dependency.
The native host writes the resulting file bundle and reports I/O failures.
Its new scope-context transport fixture brings the strict count to 169; that
remaining native boundary is still migration work.


Scoped Go retrieval now includes canonical `_shared` workspace rows alongside
public global rows in visible search, graph evidence, negation recall and hybrid
code context. Project and active-workspace priority stays ahead of result caps;
exact-scope requests still exclude shared rows. Restricted-role replay covers
shared graph parents, shared code evidence, missing context and private-row
exclusion. Scope-tag mutation/projection/rollback/cascade checks live in Go;
unused native scope tagging and scoped-search adapters are removed. Remaining
unbuilt native context fixtures are explicitly still migration debt. The memory
tree is down to three C sources (287 lines) and five headers. Strict G0 still
reports 169 repository-wide findings because these adapter retirements do not
remove the remaining native files and consumers; its baseline is unchanged.


Trace retry/recovery/common-sequence detection now belongs to the shared Go
owner through a host-only runtime operation. It preserves the legacy indicators,
thresholds, order and wording, counts distinct supporting plans, and avoids the
old 128-pair scratch limit. Bounded input/output and write screening refuse a
batch without partial findings. Native trace code is reduced from 329 to 110
lines of collection/persistence transport; its real-Go fixture checks malformed
responses, deduplication and reported write/cursor failures. The trace path is
still an unshipped monolithic connection: native DB1 fixed buffers, plan-sorted
pagination, the global cursor and non-atomic persistence remain migration debt.
A source-specific, authenticated Server-to-KB handoff must replace them before
this can be exposed through shipped surfaces. The new native connection fixture
raises strict G0 to 170 findings; the immutable baseline remains unchanged.


Go now commits legacy trace findings, canonical actor/extraction metadata and
cursor advancement in one transaction. A transaction lock and expected-cursor
comparison refuse stale concurrent batches. Packaged-schema replay under the
restricted role forces a final cursor failure and verifies all findings roll
back; separate PostgreSQL transactions verify serialization. DB1 trace pages
now select by ID before LIMIT, with Go restoring plan/turn order for analysis.
The native DB2 cursor implementation and memory_domain_bus.c are deleted. The
old generated DB2 key_exists wire contract still retains its catalog declaration
and remains migration debt, with no native implementation/production caller.
Schema 16 grants the runtime only SELECT/INSERT on the cursor log and sequence
access. Memory-tree inventory is two C sources (224 lines), five headers, and
166 strict G0 findings. The native collector, global single-source cursor,
source/epoch identity, cross-page continuity and authenticated Server-to-KB
handoff remain unfinished; no public trace endpoint is introduced.


Benchmark QA context assembly now runs in the Go owner through a host-only
runtime operation. It uses the same scoped search path as the former native
adapter, preserves numbered snippets and the byte/4 token estimate, and counts
prefixes, ellipses and newlines against both budgets. Full UTF-8 content survives
when it fits; small budgets truncate only at rune boundaries, and unavailable
retrieval cannot become an empty success. Restricted-role replay covers local
priority, missing/exact context and failures. A native connection fixture calls
the real Go assembler. Native benchmark scoring, loaders, scratch-store isolation
and other consumers remain migration debt. Its explicit scope-context transport
and fixture raise strict G0 to 169 findings; the baseline is unchanged and the
memory tree still has two C sources (224 lines) and five headers.

G0 completion requires no native files in either memory implementation tree, no C entries in the memory descriptor, Go producers and consumers on the memory side of the bus, and no relocated C memory behavior elsewhere. The bus and external transport-only callers remain C. Build the memory executable and Go caller tooling with `CGO_ENABLED=0`; inspect their dependency closure as well as the source inventory. Exercise supported CLI/MCP/HTTP/bus operations against the actual C bus in both placements, then audit source ownership, descriptors and build registrations. Prove unavailable-module, malformed-response, unsupported-version, cancellation, deadline, restart and concurrent-call behavior. A successful pure-Go build alone does not certify these integration guarantees. Later feature slices must preserve this boundary.

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
| G0 extraction and Go memory integration in every slice | MR-18/S1 file/symbol dispositions first. Replace all C memory behavior and module-side bus communication with Go bindings to the existing C bus; pass zero-C module, repository ownership and `CGO_ENABLED=0` gates before ownership closeout. Preserve the C bus and transport-only external callers. Version contracts and test both placements in each later slice. |
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
The native caller inventory requires ownership review: remaining memory behavior blocks G0; external transport-only C callers do not.
