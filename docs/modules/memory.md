# memory module

## Language and bus boundary

The memory module, including its bus producer and consumer, must be pure Go.
The existing bus in `src/core/event_bus` remains C. `server-go/bus` is the Go
binding to that bus, not a replacement implementation. Server and KB use one
shared Go memory implementation with placement-specific storage and permissions.

External C hosts may retain transport callers and their protocol declarations.
Memory policy, retrieval, ranking, persistence orchestration and fallback behavior
belong in Go; moving those into a C host does not complete the migration.
The memory process must build and communicate without cgo. Integration tests
must exercise its Go producer and consumer against the actual C bus.

Both memory trees now contain zero native files, and the memory descriptor has
no native sources, headers or tests. `check_memory_c_boundary.py` enforces this
recursively, rejects cgo and retains the repository-wide retired-policy checks.
`check_memory_go_only.py --module-only` exposes the same source/descriptor gate.
The memory executable and live Go probe must also build with `CGO_ENABLED=0`.
This language boundary does not certify all historical behavioral parity or the
numbered reliability proposals.


Shared KB get and ID-based mutations also accept canonical positive decimal-string
IDs, preserving int64 identities through native JSON transports. Unsafe numeric
IDs, noncanonical strings and overflow remain invalid. Server delete reports
retirement versus destruction from the admitted Go authority, and preserves
`review_required` and `conflict` refusals. The legacy Server context-read route
forwards the Go envelope too; a retrieval outage is no longer an empty context.

The unlinked `cmd_memory*` console family and its obsolete record wrappers are
retired. The shipping served CLI, HTTP and MCP surfaces use the shared Go owner.
Go renders live session context, agent search text, MCP diagnostic text and full
record envelopes, preserving exact int64 IDs, complete content and explicit owner
refusals. Native adapters retain host authorization and transport duties.

The [G0 closeout](../proposals/pending/memory-reliability-g0-closeout.md) records
file/API dispositions and validation. Complete DB2 retirement is a
[future proposal TODO](../proposals/pending/db2-as-a-go-module.md#future-proposal-todo-retire-db2-completely),
separate from the memory language boundary.

## Personal/shared recall composition

The Server placement owns the composition of a scoped KB recall envelope with
personal identity and preferences. Personal rows override matching shared keys
within those sections; equal numeric IDs from different placements do not
collide. The Go owner reads current personal rows, retains full content and exact
int64 IDs, preserves scoped handles, and rebudgets the final serialized bundle.
Personal rows are never sent to the KB. Shared-only requests skip composition.

The Server personal-recall adapter also forwards the complete Go envelope as
text, including exact IDs and owner errors; the native integrity check remains
at materialization.

The internal `memory.runtime` operation `compose-recall` accepts the shared
response as JSON text and returns the complete composed response as text, so C
transport cannot round IDs while decoding it. Plugins and the KB placement
cannot invoke this personal-store operation. A failed private read refuses the
whole composition; shared errors/quarantine remain refusals. The retired
`src/user_memory_merge.c` policy and mutable-array ABI are guarded against return.
This does not make cross-store reads one atomic snapshot or provide durable
release receipts. Activation snapshots transport canonical decimal strings; the
Go owner validates them and returns scoped handles. Native receipt transport
uses those handles without double conversion and refuses ambiguous legacy IDs
above 2^53-1.

## Current-state retrieval validity

The Go `current-validity-v5` predicate applies active lifecycle, suppression and
half-open valid time before shared lexical, whole-record/unit semantic, graph/
PageRank, compatibility-window, recall-bundle, activation and briefing limits.
Pending commitments retain their lifecycle while sharing the time predicate;
sticky activation cannot extend expired validity. Briefing episode summaries and
entity counts require current parents. Memory-backed graph evidence and
semantic edge intervals share the same timestamp normalization. Every memory
evidence source on a graph edge must resolve inside the request audience; a
visible source cannot admit an edge with hidden or inapplicable dependencies. UTC legacy wall
time and explicit offsets compare as instants against one transaction clock;
malformed nonempty values refuse retrieval. Scope/RLS and current embedding
fingerprints remain additional admission requirements.

Directive/reminder matching, recall fallback and briefing views share the same
normalized expiry gate. Sweeps expire a row at the exact upper boundary; serving
does not wait for a sweep. Operator lists/dashboard counts retain stored lifecycle
state so unswept records remain inspectable.

Assertion search normalizes stored offsets and fractions for both world-valid
and belief-time intervals, including its current/historical projection. The
request contract still accepts second-precision UTC anchors; historical access
authorization and parent-version closure remain separate acceptance work.

Current typed-fact blocks apply the same assertion time checks before their
limit and require every memory source to resolve to a current visible parent.
Entity discovery and later fact reads propagate errors; a failed read cannot
leave a successful partial block. Operator review/history semantics are separate.

The baseline policy fingerprint includes this version. This current-state slice
does not certify all MR-01 surfaces, privileged historical/belief-time access,
utility horizons or a final release/revocation generation check.

## Canonical KB mutation admission

KB same-key store, edit, supersede and legacy store/content-edit adapters now use
one Go replacement admission path. Changed content gets a new row, a closed old
validity interval and a supersession link. User corrections preserve history too.
Primary/secondary scopes, owner principal and sensitivity survive replacement;
new authorship and confidence ceilings are derived from the admitted writer.

Model replacement or retirement of user-authored or unknown-origin content returns
`review_required`. This refusal preserves the active source; linked review proposals
remain to be implemented. Episode/experience content stays immutable, and policy or
instruction content requires the reviewed replacement path. These checks also
apply to same-key conflicts. Identical same-author upserts retain their ID and
original captured author. Same-key creators serialize on the scoped identity,
including before any row exists. Ambiguous legacy duplicate keys fail explicitly.

The old data-stage `update-content` operation returns the new identity in `ids`;
callers must use that identity after a successful correction. Scoped legacy delete
uses the same model retirement policy as the public command. Explicit authorized
hard deletion remains distinct. Transactional extraction capture/job failures roll
back the entire replacement. This is the initial MR-02 KB slice, not completion of
expected-version/idempotency keys, durable invalidation replay, personal-memory
versioning, reviewed proposals or the full mutation/retention policy.

## Server search orchestration

Server KB search forwards keywords to `memory.search` with the Server view. The
Go owner validates and joins them, executes fact and compatibility-window reads
in one scoped transaction, and returns complete owner envelopes. A failed lane
fails the operation; it cannot masquerade as an empty window list. The C host
only resolves placement/scope and transports the result. Integer tokens and
content survive without fixed native result structs.

Both placements retain complete Unicode queries up to the Go data-stage limit
of 16,384 bytes and reject longer queries explicitly. The old 2,047-byte silent
truncation is retired. This changes long-query behavior intentionally; the frozen
retrieval manifest must be used for comparisons.

## Live benchmark owner

`memory.benchmark` parses the live code-graph corpus, chooses scoped live-memory
self-retrieval cases, runs retrieval, scores and renders reports in Go. CLI and
Server transport raw host-owned file bytes and complete responses. Corpus text is
bounded at 1 MiB; malformed cases or labels fail before retrieval. Integer labels
are preserved exactly and duplicate labels are rejected. The retired native
case loader, ablation resolver and live scoring loops cannot be restored.

Every successful report includes per-case queries, expected/retrieved IDs and
owner latency. Any failed query refuses the entire report. Unlabelled queries
contribute latency only; quality metrics divide by the labelled-query count.
Live self-retrieval is identified separately from labelled evaluation. Reports
explicitly state that these are live diagnostics without snapshot isolation,
not frozen release gates. Latency measures owner retrieval, not the Server/KB
network hop. Dataset suites point to the isolated Go evaluator.

## Labelled audit and calibration

`memory.audit` and `memory.calibrate` are Go command-owner operations. They accept
bounded TSV labels (`query<TAB>positive memory ID`), preserve full Unicode queries
and integer IDs, and collect scoped diagnostics through the ordinary owner read
path. Audits score the actual candidate order. Expected IDs are never inserted
into the candidate pool; an unretrieved answer remains a miss.

Calibration freezes each candidate pool once and performs bounded deterministic
coordinate search over diagnostic feature multipliers. Its artifact explicitly
says `deployable: false`: these multipliers are an offline experiment, not the
Go serving ranker. `--apply-config` is refused before any read. The legacy CLI
adapter transports requests and copies Go-rendered output; `--write` now writes
a JSON analysis artifact, not an unused native ranker configuration. Invalid
labels, missing owners and failed reads cannot publish successful partial scores.
Legacy explain rendering no longer calls the deleted native effective-importance
formula; owner diagnostic score parts remain authoritative. Its unregistered C
fixture is retired with that formula. Go scope-validation and restricted-role
replay cover the supported filter boundary; unknown scopes are rejected instead
of silently becoming workspace scope.

The retired native fusion fixture is covered by production Go RRF tests for
arm fairness, per-arm deduplication, candidate limits and stable record metadata.
Repeated IDs within one arm contribute one vote; agreement between distinct arms
still contributes to fusion. This is a correctness regression, not a quality claim.

## Isolated Go evaluation transport

`server-go/modules/memory/cmd/aimee-memory-eval` runs the ordinary Go KB memory
handler against a fresh PostgreSQL database. The PostgreSQL provider owns the
database lifecycle and the same SQL client/provider wire used by production.
Seeding, searches, context construction, diagnostics and scoring within one
process therefore share one store. Each new process starts with an empty store.

Set `AIMEE_DB2_EVAL_URL` to an explicit disposable PostgreSQL admin DSN with
database-creation rights. There is no fallback to live store configuration.
The provider creates a random database from `template0`, applies the supplied
packaged schema there, and drops that database at EOF, on protocol failure or
on a handled termination signal. Cleanup rolls back abandoned transactions;
failed drops produce an error naming the database so cleanup can be retried.
A forced process kill can still leave a database requiring cleanup.

From `server-go`, with the environment variable already set:

```sh
go run ./modules/memory/cmd/aimee-memory-eval \
  -schema ../src/modules/db2/c/schema.sql -embedding-dim 1024 <<'JSONL'
{"stage":"data","body":{"operation":"insert-epistemic","tier":"L2","kind":"fact","key":"eval-fixture","content":"eval-fixture content","confidence":0.9,"project":"evaluation"}}
{"stage":"data","body":{"operation":"search","query":"eval-fixture","project":"evaluation","limit":10}}
{"stage":"command","command":"runtime","body":{"operation":"benchmark-context","query":"eval-fixture","project":"evaluation"}}
JSONL
```

One JSON response per input line carries the owner's numeric module `status`
and JSON `body`; nonzero statuses remain failures. IDs retain their integer
precision. Requests and responses use the owner contracts, not a second memory
implementation. The local evaluation transport has no model/network executor:
operations needing one report that absence. This is not a replacement for an
embedding-enabled retrieval benchmark. SQL uses the disposable database owner's
permissions; the separate restricted-runtime-role replay remains necessary.

The `locomo` and `longmemeval` retrieval suites use this Go evaluator, with a
separate disposable database for each conversation or question. QA, session-support
and miss-report suites now use the same injected Go module setup. The native
dataset runners and memory scratch-store hooks are deleted.

Corpus evaluations now emit a versioned manifest and one result per case, with
stable fixture IDs rather than ephemeral database IDs. The manifest binds the
semantic corpus digest, exact schema bytes used to create the database, embedding
serving identity/dimension, ordered case IDs and effective ranking policy. Baseline
comparison rejects missing manifests and changed identities even when the case
count matches. Baselines retain the case receipts; failed runs cannot overwrite
them or publish partial scores. The checked-in 105-case input is frozen by
`tests/eval/memory_retrieval_manifest_v1.json` and a required unit assertion.

The [retrieval compatibility decisions](../proposals/pending/memory-reliability-retrieval-compatibility.md)
state what is retained and retired. This first manifest does not certify real-model
quality, historical C ranking parity, temporal reproducibility or the full release
matrix. Existing aggregate-only baselines require explicit regeneration.

## Purpose and non-goals

Shared KB searches admit semantic-only whole-record and derived-unit matches
from the active versioned embedding catalog. The Go owner holds the rebuild lock
while it checks the model identity, embeds the query and reads candidates. Current input hashes,
parent visibility, exact scope, lifecycle, suppression, kind and tier are checked
before limiting candidates. Wrong-width and zero vectors cannot enter the result.
The channel retains the former whole-record cosine floor and dimension-dependent
scale, including the `memory_semantic_floor_scale` configuration override, then
fuses lexical and semantic ranks while preserving scope priority. Unit similarity
adds the retired native type/kind intent boosts and weight contribution, with
separate temporal/event/summary floors scaled to the deployed dimension. Intent
matching uses whole terms, so `candidate` and `Chicago` do not imply a date query.

Each semantic channel filters current input hashes, parent visibility, lifecycle,
suppression, kind, tier and vector validity before its parent budget. Unit inputs
include the source content, so editing a parent or unit invalidates stale unit
vectors. Non-finite unit weights are excluded. Units group by parent using their
best qualifying similarity; copies cannot add votes. Whole-record and unit hits
then deduplicate by parent using the stronger score. Both channels use one query
embedding and the same pinned serving identity. Unit and temporal lane counters
identify their actual contributions.

Model outages, identity changes and invalid query embeddings leave lexical recall
available; a required SQL failure remains an operation failure. This path requires
a pinned active version and a governed executor. Unversioned-vector admission and legacy semantic query expansion are deliberately
retired from this path under the compatibility decisions above; this does not
certify historical retrieval quality.

Memory is one Go module deployed in two placements. `AIMEE_MODULE_PLACEMENT` is
required for a running process:

- `server` owns the appliance user's private `user_memories` rows.
- `kb` owns `global`, `workspace`, and `project` rows in `memories`.

The placement is validated before every data operation. The server's C bus
adapter also replaces any supplied scope with `user`, so a client cannot use the
server placement to address KB memory. The KB placement rejects user scope.

The Go owner stores embedding generations in an unconstrained `vector` column,
with dimensions recorded per version. Global DB2 dimension reset only discovers
columns declared with a fixed vector dimension, so it cannot drop these Go-owned
generations. Unknown dimension-bound tables still refuse the reset, including
with force enabled. Rebuild/cutover of memory generations stays with Go.

## Public contracts

Server KB store, list and supersede forward complete Go-rendered responses through
the authenticated KB transport. The Go owner owns validation and server response
shape; native callers no longer reconstruct these records through `memory_t`.
The transport preserves numeric tokens and extra receipt fields, forwards owner
`review_required`/`conflict` errors, and rejects missing/malformed owner envelopes.
It copies command data fields, reconstructs host scope, and supplies authority
from the verified host context; caller-supplied actor/authority/operation and scope
control fields do not pass through. Supersede keeps its flat server envelope,
while the ordinary KB command retains its nested memory response. Missing private
memory still cannot trigger a KB fallback.


Private store/get/list/search/delete/supersede/stats commands also forward complete
Go envelopes through the Server placement. Their runtime transport quotes the
JSON so the native parser cannot round record IDs or error receipts. Private
get/delete/supersede accept canonical positive decimal-string int64 IDs;
unsafe numeric IDs remain rejected. Supersede retains a typed integer in its flat
Go response. The host adds HTTP error classification through its existing
runtime-web provider without rewriting owner tokens. This transport change does
not implement personal version history or durable mutation receipts.

Personal review-list rendering now belongs to the Go owner through the private
`user-review-list` runtime operation. It retains the Server envelope, both
`lifecycle_state` and `lifecycle` fields, complete content and integer IDs. The
Server only selects local-user versus explicit KB transport. KB review responses
pass through as complete JSON, and restore returns its `id` and `restored` receipt
from Go. The retired `kb_client_memory_review_list_json` and
`kb_client_memory_restore` APIs are forbidden by the native boundary guard.
Server restore still records its content-free transport audit; KB publishes the
authoritative mutation audit. Owner refusals retain their error kinds, while
missing or malformed responses are unavailable rather than successful empty
reviews or a fabricated not-found result.

Memory statistics and health console views are rendered by the Go owner through
`view=console` on the existing public commands. Statistics optionally include
effectiveness for JSON output; failures of the primary query remain failures.
The CLI forwards Go's display/text fields and search timing view. Server KB stats
forward the complete validated owner envelope, preserving integer tokens and
error kinds. Missing or malformed replies never become healthy zero statistics.
The four native stats, raw-stats, effectiveness and health client APIs are retired.
PageRank timing fields use one process-local snapshot of successful Go scoring
calls. The console timing envelope labels zero samples `unmeasured`, identifies
its source as `go-pagerank`, and separates recall and explicit candidate-scoring
sample counts. Timings publish only after the owner request commits; recall
timing includes neighbor expansion, graph loading and reranking. These are not
end-to-end retrieval or write-to-readable latency measurements.

The same console view owns maintenance mode parsing, summary rendering and the
vector-maintenance handoff indicator. The CLI passes mode names and watch timing;
Go retains the comma/space mode vocabulary, numeric API modes, default-mode
sentinel, force and dry-run behavior. Skipped cycles use the owner's boolean
receipt, and failed maintenance cannot be rendered as a successful empty cycle.
MCP also uses the Go-rendered summary and prune-removal notice, which distinguishes
completed, skipped and dry-run outcomes. The private `maintenance-model-plan`
runtime operation now owns MCP mode parsing, default expansion, prune removal,
capability selection and no-op text. It ignores caller-supplied actor/capability
fields and emits a new bounded request. Server enforces the selected capability
against the authenticated connection and forwards only that request. The KB Go
owner reapplies `model_policy` before execution: a prune-only request returns a
no-op before reaching SQL, and zero/default modes become replay plus compact.
Operator and scheduler maintenance retain their existing unrestricted path.
The retired native mode-policy functions are forbidden by the boundary guard;
MCP mutation-verb routing and other native clients still await migration.

History and stale-memory inspection also render in Go. CLI history preserves its
12-field record shape, MCP history preserves its six-field rows and empty/count
envelope, and both carry complete content and int64 IDs as rendered text through
native transport. The low-effectiveness console retains its 0.3 threshold and
limit behavior. Stale provenance combines the 14-day unused-L2 and three-version
queries with a 256-row cap each; a failed query never becomes an empty half of
the report. JSON field filtering now handles top-level arrays while retaining
number tokens. Four native history/stale reader APIs are retired and guarded.

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

Go owns recall section caps, identity-prefix selection, and complete-bundle
budgeting in both placements. KB recall also includes always-on hard rules and
the prompt-consumer `memory_id`, `text`, and `why` fields for reminders and
directives. Unexpired open directives provide the fallback when none match the
hint; only directives retained in the returned bundle increment surfaced counts,
within the request transaction. Required reads and counter writes fail the
request rather than returning an empty successful bundle.

The budget estimate uses four serialized UTF-8 bytes per token, including all
record aliases and metadata. Defaults remain 600 per turn and 1800 at session
start, clamped to 64–8192. Whole rows are removed in reverse section priority,
with hard rules last. If the required empty envelope alone exceeds a tiny budget,
`budget_exceeded` reports that explicitly. This is the owner's bundle budget;
the remaining native Server composition still needs final merged-payload
budgeting during its migration.

Go data-stage store/update/supersede/delete/reject/restore operations now publish
content-free action observations on the owning daemon's audit bus after the
request transaction commits or rolls back. Only a fingerprint of the kind/key
identity is emitted; content and prose reasons are excluded. Publication uses
the existing ACTION wire and a narrowly granted notification capability on
principal 73. Ring backpressure is bounded, and a publish failure is logged
without misreporting a committed mutation as rolled back. Enqueue success is
not a durability acknowledgement: the KB's transactional SQL WORM record remains
the durable mutation record. Background/direct-store mutations and pre-dispatch
refusals still need observation coverage before the native audit hooks can be
retired; the current request-level events do not claim that coverage.

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

The memory module has no native sources or headers. External C hosts retain
transport and shared protocol declarations as described in Compatibility below;
the C bus stays C. Memory behavior and its producer/consumer belong in Go. The
descriptor's `ownership_complete` flag verifies the declared file inventory; it
does not certify historical behavior or performance parity.

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
optimization must not suppress a valid recall. Supported production memory data operations cross the Go owner. The ownership ledger classifies every original native file/API and each external
native-name finding; native fixtures prove transport, while the Go owner tests
prove memory behavior.

The Go package tests cover placement isolation, scope expansion, CRUD,
maintenance, workflow identity, recall gating, extraction, ontology, embedding,
typed-fact planning/grounding, and PII behavior. Active C transport tests cover message framing and host/connection integration;
retired-engine fixtures are not substitutes for Go owner regressions.
The required `db2-process-replay` CI job initializes the packaged DB2 owner,
then runs `make -C src memory-owner-replay-check` with separate packaged-replay
and empty scratch connections. `AIMEE_DB2_URL`, `AIMEE_MEMORY_EVAL_URL` and
`AIMEE_DB_TEST_URL` are required; the evaluator provisions isolated databases.
The target runs the full memory, isolated evaluator and module race suites with
required PostgreSQL variables, including the restricted-role replay. Missing
DSNs fail instead of skipping. The existing cross-language conformance gate
now also kills and restarts the
Go memory process in both placements while retaining its C host/callers. It
checks unavailable discovery after reaping and reruns host and Go-client parity
after restart. This store-free process test does not prove durable database
recovery, real-provider quality or the complete surface/restart/failure matrix.

Descriptor validation enforces the source inventory, and both `aimee-server`
and `aimee-kb` must link without any retired C memory implementation.

## Operational diagnostics

Check module attachment, placement, PostgreSQL readiness, and embedding health
when recall is unavailable. Personal data calls receive a bounded five-second
budget through the Server adapter; the regression holds a real database lock
for 1.5 seconds and requires a successful `recall` with the exact canary.
An expired call still fails rather than retrying indefinitely or changing stores.

## Compatibility

The final native-file dispositions separate external protocol consumers from
memory implementation. The C bus itself is unchanged by this cutover.

| Retired memory-tree file | Disposition |
|---|---|
| `memory_data_bus.c` | Its only production callers are native benchmark hosts. Their request/response transport is now owned by `src/modules/benchmarks/agent_eval_memory_transport.c`; it calls the existing C bus and contains no memory storage, ranking or lifecycle implementation. Memory's producer and consumer remain Go. |
| `include/aimee/memory/module_api.h` | Host stage identifiers live in `src/headers/memory_stage_contract.h`, outside the memory module. Go conformance tests compare every identifier with the owner; event durability coverage follows the host contract. |
| `memory_ontology.h` | Persisted graph codes shared with native indexing belong to `src/modules/db2/c/graph_kinds.h`. They contain enum declarations only. Go conformance tests pin node and relation codes to the Go ontology. |
| `memory_core_internal.h` | Deleted obsolete declarations for the removed native engine. The unregistered lane-outcome fixture is ported to Go and runs in the normal package tests. The native performance harness explicitly reports its retired memory cases unavailable. |

`check_memory_c_boundary.py` also forbids database access from the benchmark
transport and rejects restored native memory policy in other owners. Existing
external C transport callers may remain; they cannot replace an unavailable Go
owner with local memory behavior. There are no forwarding memory headers or
native memory include roots in Make/CMake.

The historical `check_memory_go_only.py --report` inventory remains available
with its original all-native-callers criterion and immutable baseline. It still
reports external C callers and shared types, so its total is not the module's
native-file count. The default audit has not been weakened or made to pass by
renaming retained host interfaces.

Recall lane counters now live in Go and are exposed by `recall-metrics` as
`lane_counters`. They count unique eligible candidates and the final store
selection for lexical, semantic and graph lanes, including overlapping sources
and a shutout when a populated lane supplies no selected record. Empty lanes
emit no keys. Counters are process-local, concurrency-safe and diagnostic only;
they are not evidence of final context packing, transaction commit or provider
delivery. Route-qualified counts distinguish lexical from hybrid contributions.

The native `bench-perf` memory cases depended on deleted in-process functions.
They now emit null timings with `status=unavailable`, exit 2, and cannot save or
certify a baseline. Use `aimee-memory-eval` for isolated Go corpus measurements;
its results are not interchangeable with the old in-process timing baseline.
The private, host-only `runtime` operation `pagerank` now scores an explicit
candidate set in Go. Supply `ids`, optional `iterations` (default 6, range 1–16),
`weight` (default 0.35, range greater than zero through 10), and `relations`
(default `depends_on`, `related_to`, `co_edited`, `fixes`; an empty list accepts
all nonempty relation labels). Normal project/workspace or exact-scope filtering
applies before graph work; suppressed and inactive records cannot contribute.
The work budget is 128 unique positive IDs and 8192 links. Oversized graphs and
SQL errors fail instead of producing partial scores. IDs retain int64 precision.

The Go kernel matches captured output from the retired C implementation:
undirected adjacency, parallel-edge multiplicity, damping 0.85, uniform dangling
mass, and maximum-normalized bonuses. Successful owner calls record graph-query
plus kernel time only after the request transaction commits. Measurements are
process-local, not an end-to-end retrieval latency or a replacement for the old
native baseline. `BenchmarkPageRankKernel50` measures CPU work alone. This private
operation makes scoring available to the isolated evaluator.

PageRank recall is disabled by default. The KB owner honors the existing
`memory_pagerank_enabled`, `memory_pagerank_iterations`, `memory_pagerank_weight`
and comma-separated `memory_pagerank_relations` settings, with the corresponding
`AIMEE_MEMORY_PAGERANK_*` environment overrides. Defaults match the private scorer.
Environment integers clamp to the existing enable/iteration bounds; malformed
numbers, non-finite/out-of-range weights and invalid relation filters fail the
request. A settings-provider failure also fails the lookup. Caller JSON cannot
activate or override the deployment policy. Personal memory bypasses KB graph
configuration entirely.

When enabled, retrieval collects up to four times the requested result count
(minimum 16, maximum 128), applies its existing semantic/graph/negation fusion,
and adds eligible one-hop memory-link neighbors while space remains. Both ends
must satisfy scope, kind, tier, lifecycle and suppression checks before links can
consume the 8192-link neighbor budget. Candidate scoring has a separate 8192-link
budget. Overflow and required SQL failures fail the lookup, without successful
fallback metrics. Candidate visibility is checked again before graph scoring.
Project/workspace/global priority precedes the final score and result limit.

The versioned `rrf60-pagerank-v1` integration adds the kernel bonus divided by 61
to the existing Go reciprocal-rank/negation score. Link-only neighbors start with
zero base contribution. Diagnostics and traces report the actual `retrieval_base`,
`pagerank` contribution and total used in that decision, rather than recomputing
a text score. The answer-support gate still uses its existing text-support scale;
graph popularity is not corroborating evidence. Disabling PageRank restores the
previous retrieval path and does not query memory links for this feature.

This integration preserves the native PageRank kernel and its opt-in setting,
but does not reproduce the retired C ranker's different score units. Historical
end-to-end ranking parity, legacy query/candidate expansion and unversioned-vector
admission remain separate work. No production enablement or quality improvement
is claimed without paired evaluation.

There is no C memory engine and no C DB2 `memory_*.c` implementation. A legacy
operation must be added to the Go data handler before its adapter may report
success; a local fallback is forbidden.

## Extension and removal

Add operations to the Go `memory-data` handler with placement-isolation and
bus-framing tests, and update the descriptor-owned sources. Do not restore a
retired C policy or storage implementation to satisfy a legacy ABI. Both role
supervisors require memory; removing it requires migrating those consumers
and their event contracts rather than silently dropping recall behavior.

## Migration history

[Earlier implementation checkpoints](memory-migration-history.md) retain the
chronological record. Counts and pending statements there describe their original
checkpoint, not the current inventory. Current scope and validation limits appear
above; the [delivery tracker](../proposals/pending/memory-reliability-delivery.md)
tracks remaining program work.
