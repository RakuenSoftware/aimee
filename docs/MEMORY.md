# Memory behavior and migration

The [module contract](modules/memory.md) defines ownership, dependencies and activation.
This guide retains the detailed behavior and migration record.

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

The [G0 closeout](proposals/pending/memory-reliability-g0-closeout.md) records
file/API dispositions and validation. Complete DB2 retirement is a
[future proposal TODO](proposals/pending/db2-as-a-go-module.md#future-proposal-todo-retire-db2-completely),
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

The Go `current-validity-v6` predicate applies active lifecycle, suppression and
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

Direct-ID current reads apply the same lifecycle, suppression and valid-time
predicate. Legacy `get --as-of` remains an inspection of a specific retained
version with a separate `valid_at` label; it does not reconstruct belief time.
It permits superseded, archived and retired versions, including their ordinary
activation suppression, but excludes deleted, rejected, revoked, quarantined and
unknown states and suppressed active rows. Scope checks still apply. Mutation
admission reads the scoped identity independently of serving eligibility so an
authorized caller can retire an excluded active row.

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

The [retrieval compatibility decisions](proposals/pending/memory-reliability-retrieval-compatibility.md)
state what is retained and retired. This first manifest does not certify real-model
quality, historical C ranking parity, temporal reproducibility or the full release
matrix. Existing aggregate-only baselines require explicit regeneration.

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

## Migration history

[Earlier implementation checkpoints](validation/memory-migration-history.md) retain the
chronological record. Counts and pending statements there describe their original
checkpoint, not the current inventory. Current scope and validation limits appear
above; the [delivery tracker](proposals/pending/memory-reliability-delivery.md)
tracks remaining program work.
