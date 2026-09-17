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
those C paths remain. The module currently retains four C sources and seven
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
