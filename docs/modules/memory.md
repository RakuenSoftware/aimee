# memory module

## Purpose and non-goals

The [memory behavior guide](../MEMORY.md) details Go ownership, retrieval validity,
mutation admission, evaluation and caller migration.

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

- `audit`: records governed memory actions through the audit publisher.
- `egress`: supplies governed access to external memory providers.
- `postgres`: executes owner-scoped storage operations over the module bus.
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

Check `memory` module attachment, placement, PostgreSQL readiness, and embedding health
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
| `memory_ontology.h` | Persisted graph codes shared with native indexing belong to `src/modules/db2/include/aimee/db2/graph_kinds.h`. They contain enum declarations only. Go conformance tests pin node and relation codes to the Go ontology. |
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
