# Memory reliability migration history

Historical checkpoint notes. Pending statements and inventory counts apply to
the original checkpoint. See the [delivery tracker](memory-reliability-delivery.md)
for current work and acceptance limits.

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

