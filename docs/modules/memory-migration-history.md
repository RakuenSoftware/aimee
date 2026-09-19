# Memory migration history

Historical checkpoints in implementation order. These are not current status or
release certification; see [the maintained module guide](memory.md).

KB record reads, history, session-priority queries, scope ranks and tagging also
execute at stage 8. Public record responses preserve full content, headlines and
stored metadata inside the request's scoped transaction. Tag destinations are
separate from the source visibility context. PostgreSQL tests exercise these
commands as a non-owner role, including historical validity, hidden records,
empty results and bounded lists. The retired native list/get adapters and
full-content ABI helpers are removed.

Briefing, alerts, context assembly, graph-edge queries, conversation compaction,
and task-drift responses are also Go-owned public commands. Briefing episodes
must have a visible parent memory, and context assembly searches the visible
project/workspace/global set rather than silently dropping shared context.
Failures remain distinct from successful empty results.

KB delete, update, touch, reject, restore and workflow-upsert commands execute
in Go. Destructive user edits require both an explicit request and verified
host authority. Restore records the verified actor, never an actor supplied in
arguments. The generic CMPQ v2 request adds a context length at byte 16 and sends
verb, arguments and verifier context separately; CMPS responses remain v1.
Memory accepts verifier context only from the host's reserved bus principal.
External grants cannot claim that principal and the bus stamps sender identity.
Plugins retain their v1 invocation and receive no verifier context.

KB store also runs in the Go owner. The memory row, fact-extraction actor capture,
and extraction job commit together; capture or enqueue failures roll back the
write. Verified host context is required for user provenance, and replacement
of a key with model-authored text resets captured authority. Learning promotion
and import consume the generic command route. Import applies its workspace
before writing and reports module failures instead of counting them as imported.

Supersede closes the previous validity interval and opens its replacement at the
same instant, preserving source scope, metadata, secondary scope tags and a
supersedes link. New model text cannot inherit user provenance. Episode and
experience corrections require annotation; instructions and policies require
revocation. Public replacement and extraction enqueueing share one transaction.

Episode-card creation, conversation search and JSONL exports also use the Go
command owner. Cards inherit the source scope, reject incompatible private
scopes, exclude previous generated cards and commit their lineage with the parent.
Exports retain full content and scope metadata, page through records, and publish
a private output file only after every read and write succeeds. The native export
allocation API, export header and conversation-search adapter are retired.

Content screening is a stateless Go command shared by Server and KB. KB client
transmission and trajectory export consume its allow/redact/reject result through
generic module routing; missing or malformed responses fail closed. Screening
redacts all credential spans, rejects PEM private-key bodies and refuses truncated
redactions. The native gate function, source file and private header are removed.

The Go module also declares `relations.schema_list`. Publication and graph
validation share the original 18-rule graph schema, including commit/fixes/bug
and wildcard discussion edges. This repairs an earlier port that incorrectly
used the identity-fact vocabulary and published only supersedes. Response order
is deterministic. The native schema and validation adapters and unused native
authority-edit, workspace-tag and keyword-search wrappers are removed.

Maintenance learning, scoped diagnostics, and profile packs now run in Go.
Diagnostics keep trace features local to each request and persist traces after
successful retrieval, using verified caller scope and identity. A failed trace
write cannot discard the retrieval result. Runtime-role tests cover trace
persistence, feature reconstruction, scope fallback, and write failure.
Profile packs share one Go implementation across server and KB, including bounded
validation, directory selection, and atomic activation. Native pack policy and
its header are removed; the CLI renders the shared command's response.

Embedding consumers now invoke the shared Go owner through generic host-internal
command dispatch. Zero-surface fixed-module declarations stay out of public
CLI/RPC/MCP/ACP registries; the embedding owner rejects non-host principals.
The native embedding adapter is deleted. Go owns single-text and batch requests,
configured-program dimension probes, breaker state, and per-response authorization
status. Batches issue one governed HTTP request and reject malformed rows without
publishing partial vectors. Batch HTTP timeouts default to 120 seconds and accept
`AIMEE_EMBED_HTTP_TIMEOUT_MS` overrides from 1 through 120000 milliseconds.

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

A partial context-injection cutover adds Go-owned execution plans for context,
legacy text and tool filtering. Plans preserve pristine-query precedence,
bounded UTF-8 fallback queries, separate guidance/evidence authority, conditional
revision epochs and hashed recall audit payloads. Preview/tool plans do not
count extra recalls. The generic native plan executor is not wired yet; existing
production consumers still use the previous adapter. This checkpoint is not G0
completion. Validation: memory/module race tests with PostgreSQL, a CGO-disabled
module build, module inventory and the transitional C boundary check.

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

The local KB fusion probe now invokes the host-only Go runtime view. Go performs
retrieval and renders the configured fusion state and ranked IDs/keys. The C
host transports the output without fixed memory records or floating-point ID
conversion. The probe retains the 20-row cap and no-context visibility; supplied
include-all fields cannot widen it. Missing retrieval/configuration remains an
error. Tests cover full UTF-8 keys, IDs above 2^53, both fusion settings, public
principal/Server refusal and restricted-role visibility.

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
the restricted PostgreSQL role. Strict G0 now reports 168 findings, with its
immutable baseline unchanged. Three C sources (353 lines) and five headers
remain in the memory tree, along with other native consumers outside it.

Memory wiki selection and Markdown rendering now belong to the Go owner through
`memory.list` with `format=wiki`. It retains the five page categories (including
the all-kind facts page), independent 500-row caps, provenance display, UTC
index timestamp and seven-file output. Scope is applied before the cap; full
UTF-8 content reaches the client without native memory buffers. The complete
serialized export has a 4 MiB limit and returns a capacity error rather than
truncating a page. Retrieval/metadata failure returns no files. The native host
validates basenames and the complete bundle before opening local output, checks
write/close failures, and preserves existing log content. It no longer requires
DB1 to export shared KB memory. Filesystem publication is per file, not an
atomic directory transaction; an I/O failure is reported even after earlier
files have been written. The remaining scope-context transport is still counted
by G0; this does not certify completion of the native migration.

This checkpoint has 169 strict G0 findings: the new wiki transport test adds a
counted scope-context reference while the production list adapter is retired.
The inventory remains three C sources (353 lines) and five memory-tree headers.

Benchmark context, hard-negative artifacts and miss diagnostics now use the
shared Go owner. Context assembly budgets complete numbered UTF-8 snippets.
Hard-negative selection retains the top-five cap and explicit retrieval-error
field; Go serializes the complete JSON line so native output cannot round int64
IDs or truncate content. The legacy salience field remains zero because search
does not provide that signal. Miss reports send decimal expected IDs, classify
failures and render full expected/top-result text in Go. Missing and hidden
expected rows are omitted; failed reads remain errors. Explicit scope constrains
expected rows even when the host has include-all authority. Reports above 1 MiB
fail without partial output. Native hosts retain file I/O and progress counters;
these transports remain migration work. Restricted-role PostgreSQL replay and
the actual native-to-Go connection cover visibility, exact IDs, full content,
malformed replies and failed writes.

The latest inventory is two C sources (224 lines) and five headers in the memory
tree, with 169 strict repository-wide G0 findings. The baseline is unchanged.
Benchmark scoring, seed/load operations, scratch-store isolation and other native
memory clients remain pending; this checkpoint does not satisfy G0.

Benchmark case scoring now retrieves through the Go owner and shares its MRR,
NDCG and recall formulas with the Go benchmarks process. The generic formulas
live in `server-go/internal/retrievalmetrics`, without a dependency cycle between
memory and benchmarks. Top-20 result IDs and up to 128 expected IDs cross the
remaining native transport as decimal strings. The owner retains legacy
cutoff/duplicate semantics; native receipt validation completes before exposing
scores. The remaining progress writer also preserves exact numeric ID tokens
in JSONL, without conversion to double. Host latency measures the complete owner call, including transport and
scoring. Restricted-role replay covers visibility, explicit scopes, archived
rows and required-query failures. All benchmark calls to `memory_find_facts` are
removed; its remaining legacy fixtures still need retirement.

Isolated memory and benchmarks exports include the shared metric helper and
build with CGO disabled. Exported manifests now use the canonical Go language
version and include memory's Unicode dependency and PostgreSQL test dependencies.
The isolated scoring tests pass; this is not a claim that the exported memory
runtime has complete production bootstrap or that its full replay fixtures are
packaged. The full benchmark runners still have retired query-plan references,
native loaders, scratch-store isolation and native transports to migrate.
Strict G0 is now 168 findings; the immutable baseline and memory-tree inventory
(two C sources, 224 lines, five headers) are unchanged.

The unused `memory_find_facts` C adapter and public declaration are now removed.
The transitional boundary check rejects restoration. Its remaining memory
regressions run through the Go owner: archived rows stay out of retrieval, and
canonical directive creation/repeat receipts leave the returned IDs and scores
unchanged. Restricted-role replay requires a successful nonempty search before
and after actual writes; two failed reads cannot satisfy the invariant. The
native curiosity tests retain their queue selection/state-transition assertions;
this change does not claim that the broader legacy native suite is migrated.
Two C sources (204 lines) and five headers remain in the memory tree. Strict G0
still reports 168 findings; insertion adapters, scope transport and other native
clients remain unfinished.

Memory process initialization now belongs to the shared Go owner. Both the
multicall executable and standalone export call `NewProcessHandler`, which
requires explicit Server/KB placement, attaches the existing storage/config/audit
identity, configures governed egress and starts the applicable index workers.
The standalone export previously served a default handler without a datastore.
It now includes the concurrent bus caller and configuration client. Missing
placement or storage attachment fails startup instead of serving that handler.
Configuration precedence and failure propagation have focused coverage; both
binaries pass live bus/process tests in both placements, and the memory/module
race suites pass with restricted-role PostgreSQL replay. The live bus fixture
has no PostgreSQL provider, so it establishes process/wire parity, while the
separate replay establishes storage behavior. Full isolated replay assets and
standalone process hardening parity remain unfinished. Strict G0 remains 168;
this consolidation does not retire the remaining C adapters.

The unused native `pgvec_memory_search` implementation, private declaration and
SQL scope macros are removed, along with the native pgvector test's memory-scope
object dependency. The boundary check rejects restoration. Go vector tests now
distinguish successful empty retrieval from unavailable storage and verify
no-context visibility at both 1024 and 2560 dimensions. CLI, Server and KB builds
and the remaining native pgvector suite pass. Strict G0 falls from 168 to 166
findings without changing its baseline. Native semantic assertion retrieval,
indexing and vector writes outside the memory directory still need migration;
the memory-tree inventory remains two C sources (204 lines) and five headers.

Semantic assertion search now runs in the shared Go owner. The native KB search
and typed-context callers use its result instead of performing SQL selection,
embedding, hybrid ranking or graph expansion themselves. Go preserves independent
valid/transaction-time filters, historical labels, evidence locators, reciprocal
rank fusion, the cosine floor and bounded graph hops. Candidate filtering applies
before caps; every live memory evidence source must resolve to an active visible
parent, so a visible source cannot mask a hidden one. Explicit scope remains
binding even with include-all authority. Assertion vectors use canonical rendering
and version-checked publication; embedding input is screened and bounded. Optional
vector failures roll back derived writes to a savepoint and retain lexical results.
Required retrieval failures produce the existing explicit degraded result.

Go owns complete assertion text and exact numeric IDs. The transitional native
transport restores the owner's canonical decimal token after cJSON parsing and
uses the owner's full rendering in typed-context packing. It no longer truncates
assertions into the native hit structure. Restricted-role PostgreSQL replay covers
time axes, scope and mixed evidence, graph/vector retrieval, repeated indexing,
wrong dimensions, unavailable vector/required stores and pre-egress screening.
The native transport test covers scope propagation, malformed receipts and exact
IDs through copying and serialization. CLI/Server/KB builds, Go race tests and
live Server/KB process checks pass. The obsolete C search/get/index functions,
semantic result structs, SQL scope macros and binding function are deleted. The
memory tree now has two C sources totaling 192 lines and five headers. Strict G0
remains 166 findings; typed-context channel selection, packing and watermarks,
native typed-fact writes, benchmark loaders and other clients remain unfinished.

Typed-context assembly now belongs to Go, including channel defaults/opt-outs,
selection, packing traces, freshness watermarks, sufficiency and trust envelopes.
Observations and reviewed procedures are scoped before their 64/32-row caps;
hidden rows cannot crowd out visible evidence or leak IDs through dropped-row
traces. Episode/profile reads and watermarks honor explicit scope, including
include-all requests. Each channel runs behind a SQL savepoint so an unavailable
channel remains explicit without aborting other reads. Watermark failures are
reported as unavailable rather than as an empty successful watermark.

Per-channel text estimates remain in `used_tokens`; `rendered_tokens` additionally
accounts for the complete JSON and trust envelopes. Go drops complete rows in
reverse packing order to fit the total rendered budget, preserving higher-priority
assertions and UTF-8. An empty envelope that cannot fit a tiny budget reports
`envelope_excess_tokens`. The native transport carries the complete Go JSON as
text, so proposal IDs and nested procedure numbers remain exact. More than 480
lines of native selection/packing/rendering are removed. Schema 17 adds only the
learning observation/proposal SELECT columns required by this read path; the
runtime role cannot update learning proposals or read their unrelated evidence
references. Restricted-role replay covers scope-before-cap behavior, metadata
visibility, exact IDs, independent channel failures, opt-outs and malformed time.
Strict G0 is now 165 findings; the two memory-tree C sources remain 192 lines.
Native transports, typed-fact writes, benchmark loaders, Server recall composition
and the other previously listed migration work remain unfinished.

CSS convention inference and recall now run in the shared Go memory owner.
Host-only `css-convention-sync` and `css-conventions` operations preserve the
public CSS response fields and successful unchanged assertion count. Inference
reads rule, naming and token measurements from one current-generation snapshot,
honors `css_style_graph_enabled`, and commits both convention assertions in one
transaction. Its fixed system authority cannot overwrite human corrections;
canonical evidence deduplication, functional supersession and sealed graph
commits remain in force. Missing configuration or failed SQL returns an error.
Recall preserves full text and filters hidden memory evidence before its cap.

The native typed-fact implementation/header and obsolete native fixture are
removed. KB retains only bounded request/JSON receipt transport; related CSS
analysis and migration-unit logic remain in their own module. Schema 18 grants
only the CSS selector/property and project/file identity columns needed for
inference, with no CSS writes or declaration value access. Restricted-role
PostgreSQL replay covers opt-outs, replay, generation changes, authority, Unicode,
scope-before-cap behavior, denied reads and rollback of a failed second assertion.
Legacy novel/personal-fact promotion and credential rejection checks also run in
Go. CLI/Server/KB builds, Go race tests, CGO-disabled module builds, native
transport, native CSS migration and live Server/KB process checks pass. All 77
repository lint checks pass after regenerating the declaration ledger. Strict G0
falls from 165 to 163;
the memory tree still contains two C sources totaling 192 lines and five headers.
The complete migration remains unfinished.

The obsolete native fact ingestion writer, KB policy callback, host callback
contract, confidence/lifecycle implementation and provisional-relation writer
are retired. The `memory_fact_gate.h` compatibility header and native policy
stub are removed. Ingestion uses the shared Go gate-to-commit implementation,
including normalized canonical aliases, literal endpoints, provenance classes,
provisional/live ontology handling and distinct evidence receipts.

Host-only `fact-maintenance` now implements bounded candidate promotion/expiry
in Go. It uses the canonical mutation lock, recorded changes and WORM seal, with
fixed system authority and no upgrade to Class A. Promotion excludes conflicting
functional candidates before the 64-change batch cap, preventing both displacement
of human corrections and starvation of later eligible facts. Keyset pages also
skip unkeyed Unicode legacy conflicts and explicit rejection tombstones. Expiry handles
legacy space and ISO-T timestamps, retains confirmed candidates and does not
expire persistent/user assertions. PostgreSQL replay verifies these contracts
and atomic rollback on a failed seal. The remaining native graph traversal,
review and rollback fixture retains its coverage independently of ingestion.

Strict G0 falls from 163 to 151. The memory tree contains two C sources totaling
192 lines and four headers. Benchmark loading still requires isolated Go store
sessions: replacing the native DB2 scratch connection does not change where the
Go owner reads or writes. Native graph mutation/review, other clients and the
previously listed migration work remain incomplete.
CLI/Server/KB builds, the retained native lifecycle and ontology fixtures, Go
memory/module race suites, the final restricted-role maintenance replay,
CGO-disabled builds, both live process placements and all 77 lint checks pass.


The local labelled-corpus evaluator now runs entirely in Go. `aimee memory
benchmark corpus` launches `aimee-memory-eval`, which owns a fresh disposable
PostgreSQL database for seeding, metadata derivation, versioned embedding,
activation, retrieval and scoring. It requires `AIMEE_DB2_EVAL_URL`; HTTP
embedders additionally require `AIMEE_MODULE_BUS_SOCKET` for governed egress.
The Make install includes the helper and its packaged schema. The standalone
helper accepts `-schema`, `-corpus`, `-embedding-command`, `-embedding-dim`,
`-baseline`, `-update-baseline`, `-format`, `-fields` and `-profile`.

Malformed fixtures, unknown relevance labels, changed baseline denominators,
incomplete embedding and unavailable query embedding fail the evaluation.
Baseline replacement is atomic and happens only after the isolated database
has closed successfully. Scores and latency come from the shared Go owner;
latency does not include a Server-to-KB hop. Legacy route/shape buckets are
unmeasured and remain empty. This evaluates the Go owner's current retrieval
behavior; unit/temporal candidate weights now run in Go, but this does not
certify the entire historical ranking pipeline. The C corpus loader and its dependent corpus fixtures are
retired; production-corpus/agent-manifest C fixtures remain until those runners
migrate. Go tests cover isolated semantic recall, command and governed HTTP
embedders, full fixture identities, input failures and baseline protection.

`aimee memory benchmark --suite locomo --dataset <path>` and `--suite longmemeval`
also launch the Go evaluator. The helper accepts `-suite`, `-dataset` and
`-max-cases`; the cap counts evaluated conversations for LoCoMo and questions for
LongMemEval. Each sample gets its own database, including when source IDs repeat
across samples. Every selected sample is validated before database creation.
Scores aggregate by evaluated question count, and output is withheld until all
samples finish and their databases close. Empty relevance sets and LongMemEval
abstention questions are excluded explicitly; both text and JSON report the
exclusion counts and sample count. Unknown or duplicate relevance IDs, incomplete
embeddings and malformed histories fail instead of shrinking the denominator.

The versioned `full-text-raw-query-v1` fixture policy preserves complete Unicode
source text and natural-language questions. It replaces the native truncation
and query normalization, so historical native scores are not interchangeable.
Dataset input is bounded to 512 MiB and each sample to 4096 fixtures and 4096
questions; the labelled-corpus input retains its 4 MiB bound. Dataset suites do
not accept corpus baseline options, and Go evaluation rejects legacy weight
profiles. `AIMEE_DB2_EVAL_URL` remains required; there is no live-store fallback.

All local memory evaluation uses `memory.NewEvaluationModule(ctx, store, executor)`:
instantiate the production handler with caller-owned isolated storage, call
`Seed(fixtures, embedder)`, then invoke normal module operations through `Call`.
`Seed` uses the production metadata worker and embedding activation path. The
caller closes the store; no global database retargeting or alternate memory
implementation is involved. JSON-lines, corpus, retrieval, QA, support and miss
adapters share this setup. In-process invocation measures owner behavior; the
separate live C-bus tests still cover transport in both placements.

Additional CLI suites are `locomo-qa`, `longmemeval-qa`,
`locomo-session-support`, `locomo-misses` and `longmemeval-misses`. Session support
expands each evidence label to the records from its conversation session, bounded
by the existing 128-label scoring contract. Miss reports use the module's
`benchmark-miss` operation; `--limit` (1..20) controls the cutoff and
`--max-misses` (1..100) caps reported details without changing the evaluated count.

QA requests `benchmark-context` from the same isolated module, then uses the
existing tool-free `agent_generate` executor via `aimee --json agent generate`.
Its bounded JSON stdin protocol carries `system`, `prompt`, `max_tokens` and
`temperature`. The helper receives `-agent-executable` from the CLI automatically.
Provider selection follows `agent_generate`: configured default or first enabled
non-CLI agent. Missing eligible providers fail; there is no implicit Codex fallback.
The ordinary agent-run path is deliberately avoided because it consumes hints
and stores feedback. The C agent runtime and C bus remain their existing owners.

`--top-k` (1..32) and `--token-budget` (1..131072) bound context. The
`module-context-strict-judge-v1` QA policy requires every answer and judge call to
succeed, with a judge JSON score of exactly 0 or 1. Failures produce no partial
score or exact-match fallback. LoCoMo QA includes gold-labelled questions without
retrieval evidence; LongMemEval retains explicit abstention/no-evidence exclusions.
QA uses the same full-text fixtures as retrieval, replacing the old native QA-only
turn/fact expansion. Exact-match normalization retains full Unicode text. These
versioned choices change historical comparability and do not certify old QA parity.

`--report-failures` includes at most `--max-failures` (1..100) details from the
attempts already scored; it does not rerun either model. Token counts use provider
usage when available and otherwise a labelled byte/4 estimate. Citation coverage
measures numbered-marker presence, not citation validity. The legacy
`hallucination_rate` field remains 1 minus judged accuracy; it is not a separate
hallucination classifier. QA latency includes context retrieval, answer and judge
calls. Route/shape buckets remain unmeasured. Text QA/miss reports use formatted
JSON so all provenance and exclusions remain visible.

The public `memory.search_assertions` and `memory.assemble_typed_context` routes
now invoke the shared Go owner directly. Their native handlers and receipt
decoders are retired. The generic KB command transport validates complete JSON
and forwards the original tokens, preserving 64-bit IDs and full Unicode text.
Scope is request-local; assertion responses retain `active_context_missing`, and
unavailable required evidence remains explicitly degraded. Public discovery does
not grant plugins the host authority required by these evidence operations.
