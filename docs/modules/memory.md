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

C is restricted to transport and host integration:

- `memory_data_bus.c`, `memory_domain_bus.c`, `memory_domain_runtime_bus.c`, `memory_embed_bus.c`,
  `memory_extract_patterns.c`, `memory_content_gate_bus.c`,
  `memory_fact_gate.c`, and `memory_pii_gate.c` encode/decode bounded event-bus
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

`scripts/check_memory_c_boundary.py` freezes that boundary: only the ten named
bus/integration translation units may exist under the memory module, none may
include a DB client, and DB2 may not regain a `memory_*.c` implementation.
The same check prevents the former POSIX/Windows regex-policy files and the
retired in-process C query rewriter from returning; those gates now use
`content_gate.go` through the bus adapter.

There is no C memory engine and no C DB2 `memory_*.c` implementation. A legacy
operation must be added to the Go data handler before its adapter may report
success; a local fallback is forbidden.

## Extension and removal

Add operations to the Go `memory-data` handler with placement-isolation and
bus-framing tests, and update the descriptor-owned sources. Do not restore a
retired C policy or storage implementation to satisfy a legacy ABI. Both role
supervisors require memory; removing it requires migrating those consumers
and their event contracts rather than silently dropping recall behavior.
