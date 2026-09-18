# memory module

## Purpose and non-goals

Memory is one Go module deployed in two placements. `AIMEE_MODULE_PLACEMENT` is
required for a running process:

- `server` owns the appliance user's private `user_memories` rows.
- `kb` owns `global`, `workspace`, and `project` rows in `memories`.

The placement is validated before every data operation. The server's C bus
adapter also replaces any supplied scope with `user`, so a client cannot use the
server placement to address KB memory. The KB placement rejects user scope.

## Public contracts

The supervised `aimee-module-memory` process is pure Go. It owns extraction,
write gating, embedding, retrieval safety, reranking, command declaration, and
the scoped memory data API.

| Stage | Event | Responsibility |
| --- | ---: | --- |
| `extract_index` | 5889 | pattern extraction and retraction scan |
| `write` | 5890 | typed-fact write gate |
| `embedding` | 5891 | governed HTTP embedding and breaker state |
| `retrieve` | 5892 | sensitive-data recall decisions |
| `reranking` | 5893 | confidence-band decision |
| `command-declaration` | 5894 | canonical command inventory |
| `memory-data` | 5895 | scoped CRUD/search, typed-fact extraction and recall, temporal checks, feedback, and maintenance |
| `command-execution` | 5896 | public private-memory get, store, list, search, delete, supersede, and stats |

The data stage reaches PostgreSQL over the module bus with the storage-only
principal 73. It owns neither a DSN nor a database connection. Both placements
therefore deploy the same executable and storage implementation while retaining
different scope and table boundaries.

## Dependencies and consumers

The descriptor declares these dependencies. Server and KB adapters consume the
same `memory` process contract, while scoped database operations use the shared
PostgreSQL bus service rather than a caller-owned connection.

- `config`: validated memory, embedding, and retrieval policy configuration.
- `ir`: gateway request/response integration for the memory stages.
- `module-runtime`: process attachment, principal identity, and bounded event calls.

## Providers and readiness

The `memory-data` stage needs its placement's PostgreSQL schema and registered
storage operations. Semantic retrieval additionally needs an available embedding
provider. A live bus process alone does not establish database or model readiness;
the deployment matrix tests personal and shared recall with the actual services.

## Configuration and activation

- `runtime_toggle.supported`: `false`; memory is required by both role compositions.

`AIMEE_MODULE_PLACEMENT` must be `server` or `kb` and is validated before data
operations. Configure embedding through the deployment's model/provider settings.
Selecting an optional shared KB never changes the Server's private-store scope
or permits the KB placement to read user rows.

## Surfaces

The maintained JSON surface supports `get`, `search`, `visible-search`,
`recall`, `briefing`, `list`, `store`, `supersede`, `delete`, `feedback`,
`maintenance`, `upsert-workflow`, `prospective-count`, `fact-recall`,
`valid-at`, `recall-gate`, and `pii-inject`.
The same surface owns the `memory-facts-claim`, `memory-facts-parse`, and
`memory-facts-finish` worker phases used by the KB curator connection adapter.
Host integration also uses `redirect-classify`, `redirect-bash`, and
`content-gate`; those operations are policy in Go and do not touch the store.

KB visible search applies local-first scope ordering: project, workspace, then
global, with ID de-duplication and a single bounded result limit. Server calls
are always user-scoped. Writes use active-row replacement semantics and never
reactivate a retired KB row accidentally.

### Personal recall

The `recall-bundle` operation runs in either placement. Server placement reads
its own user store through the same Go retrieval implementation; it needs no
shared schema or KB-generated envelope. Expired and retired personal records
are excluded. API, CLI, and MCP recall default to that local store; an explicit
`store=kb` selects shared recall without merging personal records. Recall items
include `text`, `memory_id`, and a scoped handle for prompt consumers.

The published 0.4.2 testing candidate also passes KB-free semantic recall with
local embedding, personal vector persistence, and local-model inference.
Structured reminders and directives remain KB-placement operations requiring
the shared schema; personal recall does not imply those shared operations are local.

## Go caller migration

`server-go/modules/memory/client.go` and `public_commands.go` implement the Go
caller for all eight stages using the existing module bus. It shares wire constants and request/result
types with the handler, bounds requests and replies, and preserves transport
errors without retries or local memory decisions. The caller supplies its admitted
bus connection and trace ID. Scope travels unchanged to the placement owner for
validation. A request context can shorten the configured call deadline.

The live `aimee-memory-bus-probe` now uses this client. Its principal remains
test-only and requires an explicit probe grant. The client borrows the connection;
its owner must drain the concurrent bus caller before detaching.

The first G0 implementation slice from the memory reliability proposals supplies
the Go client. The next slice removes the unused native fact-gate and extraction
callbacks, their Server registrations, and the obsolete C fixture generator.
It also deletes the unused inline context-assembly helpers and their C-only test.
The Go client/handler conformance tests retain the frozen historical fixtures;
DB2 ingest tests inject candidates directly into their commit-path fixture.
The remaining fact-gate header contains only legacy DB2 result codes.

PII classification and the credential write boundary now execute together with
their Go operations. `fact-write-decision` returns the ontology verdict and
commit eligibility in one reply; an incomplete decision defers the DB2 write.
Query-scoped typed-fact recall classifies the query in Go rather than accepting
a native caller's PII flag. The native PII callbacks, their headers and their
binary gate/PII encoders are deleted. The live process smoke test uses the Go
client in both placements, including concurrent calls and version rejection.

The server's private-memory public commands now pass their argument objects
through the shared module command dispatcher to stage 8. Go validates arguments,
supplies the user scope, applies defaults, and builds the complete public reply.
The server only selects the explicit user/KB destination and applies its HTTP
error classification. Shared-KB commands still use the native KB client. The KB recall endpoint now
passes its complete argument object to stage 8. Migrated KB verbs are declared
by Go at the common discovery stage (255) and invoked by the generic host
dispatcher; the remaining verbs still use their native handlers.
The command wire tests cover the existing CMPQ/CMPS frame, while Go tests cover
private scope isolation, mutation defaults, missing records, and typed failures.

KB recall decodes conversation activation snapshots in Go. Cooldown, delay, and
suppression are applied before each section cap; sticky state can preserve
relevance but cannot override cooldown. Graph-expanded candidates pass the same
gate and eligible lexical candidates backfill held rows. Missing conversation
state fails open, while stored suppression remains effective. Recall does not
advance the DB1 conversation turn or write reinforcement signals. PostgreSQL
regressions cover the selector, graph backfill, public command, malformed state,
and sticky/cooldown boundaries; DB1 owner tests cover persisted turns and events.
The native activation header, snapshot parser, and discarded-snapshot wrapper
are deleted. The local PostgreSQL fixture runs with `AIMEE_MEMORY_EVAL_URL`.

The public KB prospective-memory and directive commands now validate and shape
their replies in Go, including dashboard cards and session-start Markdown.
The native KB adapters and memory CRUD wrappers for those operations are retired. PostgreSQL tests
cover create/list/match/trigger/complete/expire, directive deduplication,
priority zero, terminal states, 256-row lists, dashboards, and briefing filters.
Reminder matching uses PostgreSQL text search so morphological matches survive
the migration. Directive metrics count actual inserts, not duplicate requests.
Maintenance and lint commands also render their complete results in Go. A
maintenance dry run no longer writes the last-run timestamp and delays the next
real cycle. Unused native background-embedding hooks and their suppression state
are deleted; the Go embedding worker and explicit embedding operations remain.
Graph queries, entity profiles, episode lookup, provenance, links, conflict
lists, and health/statistics responses are also assembled in Go. The dashboard
uses one grouped query for scope counts and includes all unresolved conflicts.
Derived graph, episode, link, and provenance queries consult their parent memory
rows so the runtime role's scope policy also protects these child tables.
PostgreSQL tests exercise these commands with a non-owner role and verify that
request scope does not remain on the pooled connection.
The shared command owner also serves key lookup, effectiveness/unused/superseded
lists, artifact updates, and memory review (including the operator console).
Review reasons are matched to the memory's exact scope. Diagnostic list caps are
validated consistently through the 256-row transport limit; PostgreSQL tests
exercise the actual unused-memory interval binding and public response fields.
Go declares and dispatches its migrated public KB commands from one route
table. Fixed modules opt into stage 255 using DCMD/DCMR version 2, which carries
the invocation stage; host dispatch takes a route snapshot before waiting on the
bus. Internal dashboard and briefing builders are not declared as RPC actions.
The legacy plugin version-1 admission and invocation protocol remains compatible.
Authenticated admission and the server-to-KB transport remain native callers.

Production C memory clients, native headers and gateway integration still need
replacement by Go callers. They must be deleted at cutover, not moved into host
directories. Passing a pure-Go process/client build does not complete G0 while
those C paths remain. The module currently retains four C sources and six
headers. The descriptor's `ownership_complete` flag verifies the declared file
inventory; it does not assert that the Go migration is complete.

## Data and migrations

The Server placement persists private rows in `user_memories`; the KB placement
persists global, workspace, and project rows in `memories`. The Go owner applies
scope ordering, active-row replacement, and temporal/retirement filtering.
PostgreSQL owns the transport and transactional migration boundary. Preserve
the instance home, Vault state, and database together for upgrade and rollback;
the published 0.4.1-to-0.4.2 deployment checks verify existing and new canary rows.

## Security and privacy

The Server C adapter overwrites requested scope with `user` before dispatch;
the Go placement check independently rejects cross-placement operations.
Storage-only principal `73` accesses the database through the bus. PII gates,
write decisions, and sensitive retrieval policy execute in the Go owner; a
missing module cannot authorize a substitute local policy engine.

## Supported journeys

KB-free Server supports persistent personal CRUD, recall, and semantic search
when its local embedding service is available. Explicit `store=kb` selects shared
recall through the optional KB. The KB curator uses the memory-facts worker
phases for claim, parse, and finish. See the published testing qualification in
`docs/validation/release-0.4.2-testing-a8abc2e-2026-09-07.md` for exercised journeys.

## Tests and failure behavior

Required policy stages do not silently run a second implementation. Extraction
returns an error, write gating defers, and PII injection fails closed when the
Go module is unavailable. The cheap recall gate fails open because omitting that
optimization must not suppress a valid recall. Every linker-live legacy ABI
operation now crosses the Go data stage; there is no unavailable shim.

The Go package tests cover placement isolation, scope expansion, CRUD,
maintenance, workflow identity, recall gating, extraction, ontology, embedding,
typed-fact planning/grounding, and PII behavior. C tests cover only message
framing and host/connection integration.
Descriptor validation enforces the source inventory, and both `aimee-server`
and `aimee-kb` must link without any retired C memory implementation.

## Operational diagnostics

Check module attachment, placement, PostgreSQL readiness, and embedding health
when recall is unavailable. Personal data calls receive a bounded five-second
budget through the Server adapter; the regression holds a real database lock
for 1.5 seconds and requires a successful `recall` with the exact canary.
An expired call still fails rather than retrying indefinitely or changing stores.

## Compatibility

The remaining C transport and host integration is migration debt:

- `memory_data_bus.c` and `memory_domain_bus.c` encode/decode bounded event-bus
  messages. `memory_scope_connection.c` only binds caller scope to an already
  prepared connection request.
- `gw_stage_memory.c` connects the gateway IR stage to the module.
- `server_hooks.c` connects retired local memory-file writes to the Go policy over
  memory stage 7; classification and shell-write detection live in `redirect.go`.
- `fact_recall.c` preserves the old DB2 ABI but is now only a stage-7 JSON
  adapter. Typed-fact SQL, entity matching, ordering, formatting, and PII
  decisions live in `fact_recall.go`.
- `kb_memory_facts.c` connects the KB drain to its existing curator provider and
  transactional fact-commit connection. Job leasing/reclaim, retry policy,
  exponential jittered backoff, prompt construction, deterministic extraction, model-output parsing,
  grounding, relation canonicalization, kind selection, and provenance are in
  `memory_facts.go`.

`scripts/check_memory_c_boundary.py` reduces that boundary: only the six named
bus/integration translation units may exist under the memory module, none may
include a DB client, and DB2 may not regain a `memory_*.c` implementation.
The same check prevents the former POSIX/Windows regex-policy files and the
retired in-process C query rewriter from returning; those gates now use
`content_gate.go` through the generic command route.
It also rejects restoring or relocating the deleted native gate, extraction and
context-assembly APIs, including declarations and macro aliases.

`scripts/check_memory_go_only.py --report` audits the final G0 boundary across
the repository, including native callers, types, forwarding headers, cgo imports
and build registrations. It exits unsuccessfully while any such debt remains;
the transitional allowlist passing is not evidence that G0 is complete.

There is no C memory engine and no C DB2 `memory_*.c` implementation. A legacy
operation must be added to the Go data handler before its adapter may report
success; a local fallback is forbidden.

## Extension and removal

Add operations to the Go `memory-data` handler with placement-isolation and
bus-framing tests, and update the descriptor-owned sources. Do not restore a
retired C policy or storage implementation to satisfy a legacy ABI. Both role
supervisors require memory; removing it requires migrating those consumers
and their event contracts rather than silently dropping recall behavior.

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
