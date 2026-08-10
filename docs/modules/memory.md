# memory module

## Purpose and non-goals

Memory is required core: it recalls, ranks, assembles, stores, and maintains information that makes
Aimee useful across turns and repositories. It includes code intelligence, embedding, and reranking;
`kb-synthesis` is not part of this module. Current ownership is split between `src/modules/memory`,
`src/db2`, KB services, clients, and root command files, which is migration debt rather than a second
memory subsystem.

### Go process stage

The supervised memory process now uses the shared pure-Go module runtime and
serves four of the five stage identities. Its `reranking` stage preserves the
MRNK/MCNF fixed-point confidence contract and the existing 330000/660000
thresholds.

| Stage | Event | What the module decides |
| --- | --- | --- |
| `write` | `5890` | the typed-fact write gate: whether a candidate triple may commit as a semantic edge |
| `extract_index` | `5889` | pattern-first extraction, and the §4 retraction pre-scan (two request shapes, distinct magics) |
| `retrieve` | `5892` | the §7 PII recall gate: whether a turn asks for sensitive data, and each relation's sensitivity tier (two request shapes) |
| `reranking` | `5893` | the fixed-point confidence band |

Only pure decisions moved. Each has a seam in the C: a registered provider that
is authoritative and never falls back to the local implementation, because a
silent fallback lets a broken module look healthy. What a failure does instead
is chosen per seam: the write gate defers (nothing written, caller retries),
extraction returns an error rather than "no facts", and both halves of the PII
gate fail closed, withholding rather than exposing. The retraction scan does not
retract, because that path deletes and a fact left behind is recoverable where
one deleted by mistake is not.

`embedding` (`5891`) is still unimplemented and continues to fail closed. It is
not a pure decision (it needs a model and a DB handle), so there is no
in-process core to relocate; moving it would relocate I/O, not logic. Storage,
graph, and lifecycle units likewise remain C.

Every ported decision is held to the C by differential fixtures generated from
the C itself (`scripts/gen-memory-pattern-fixtures.c`,
`scripts/gen-memory-ontology-seed.c`, `scripts/gen-memory-pii-fixtures.c`), not
by expectations transcribed from reading it. Each generator's header carries its
build and run line; regenerate rather than edit the fixtures.

The server's context pre-injection path requests its `high`/`medium`/`low`
confidence tier from that process over event `5893`. It no longer substitutes a
local `low` tier when the process is absent, fails, or returns an invalid value;
instead it omits the envelope and logs the unavailable classification. The
envelope formatter also rejects missing or unknown tiers, so callers cannot
bypass the module by leaving confidence unset.

## Public contracts

The current C contract is principally `src/headers/memory.h`, with platform seams in
`src/modules/memory/memory_platform.h` and provider seams in
`src/modules/memory/memory_provider.h`. Code-index operations in `src/db2/code_index_ops.c`, vector
search, `memory_assemble`, and `gw_stage_memory` are one required capability family even though their
physical paths have not all reached the module directory.

The descriptor declares this module's thirty-two sources, twelve module-root headers, sixteen direct
tests, and this document; it sets `ownership_complete: true`. All twelve headers are declared as
`private_headers` because they live at the module root rather than under
`src/modules/memory/include/aimee/memory/`, the layout the header-layout checker treats as private;
five carry no paired source (`memory_assemble_util.h`, `memory_core_internal.h`, `memory_ontology.h`,
`memory_platform.h`, `memory_rewrite_llm.h`), the highest unpaired count in the module graph. Make
compiles all thirty-two sources; CMake compiles nineteen, omitting the memory-core CRUD, search, tiers
and scope family, extraction, the fact and PII gates, graph fusion, and the gateway stage. These
server/KB-side units follow the same intentional thin-client boundary recorded for the earlier modules.
`docs/validation/core-modularization-slice-56.md` records the declaration audit and
`docs/validation/core-modularization-slice-57.md` the completeness audit; the two were split so the
latch reviews declarations merged on their own first. Adding a new module-local source or module-root
header without declaring it now fails CI on `rule=ownership-complete`.

## Dependencies and consumers

- `config`: supplies embedding, reranking, recall, retention, and safety policy used by memory paths.
- `ir`: supplies the provider-neutral request and response shapes used for recall and context injection.
- `module-runtime`: supplies the required lifecycle and extension contracts used by every core profile.

Consumers include `src/server/agent_runtime.c`, the universal gateway memory stage, the `aimee memory`
and `aimee kb` commands, learning, skills, response-composition, and KB ingestion/search services.

## Providers and readiness

Memory is ready only when durable storage can open and the configured embedding path has a compatible
dimension; semantic ranking additionally requires the reranking path selected by `config_t`.
Lexical fallbacks may preserve a degraded lookup journey, but they do not make a deployment compliant
with the required embedding-and-reranking capability boundary.

Reranking here is `kb_ranker`, a linear in-process stage over lexical, dense and recency features. It
is not a model role and needs no endpoint: the cross-encoder reranker was measured out of the stack.
Embedding is the only model this module requires. See [Local inference](../LOCAL_INFERENCE.md).

## Configuration and activation

- `runtime_toggle.supported`: `false`; memory is required and cannot be removed from a running profile.

Individual policies such as `AIMEE_STAGE_MEMORY`, embedding commands, dimensions, recall limits, and
rerank modes tune journeys rather than disabling ownership. Configuration surfaces must describe the
actual provider readiness and must not imply that core `memory` itself is optional.

## Surfaces

Operator surfaces include the `aimee memory` command family, server-to-KB memory routes, health and
benchmark output, and context injection through `gw_stage_memory` and `ir_stage_memory`. The current
code-intelligence HTTP/CLI surfaces are also memory surfaces because indexed symbols and blast-radius
evidence are recalled from the same required knowledge capability.

## Data and migrations

Durable state spans DB1 memory records and DB2/PostgreSQL memory, embedding, code-index, graph, and
artifact relations; their concrete schemas live under `src/db1` and `src/db2`. Embedding dimension and
model-version migrations must keep text, vector rows, provenance, and active-version metadata coherent;
deleting vectors is safe only where the source records are demonstrably regenerable.

## Security and privacy

Before persistence, memory applies sensitivity, ephemeral-content, evidence, and PII gates such as
`gate_check_sensitive` and the implementations in `memory_pii_gate.c`. Recall must preserve scope and
identity boundaries, and injected `<aimee-context>` content remains untrusted evidence rather than
authorization or executable instruction.

## Supported journeys

A normal turn extracts a query from `aimee_request_t`, retrieves scoped lexical/vector candidates,
reranks them, assembles a bounded evidence envelope, and gives response-composition that context.
Repository ingestion creates code units and embeddings so symbol lookup, semantic code search, and
blast-radius analysis participate in the same required memory journey.

## Tests and failure behavior

The descriptor's sixteen direct tests are `test_memory.c`, `test_memory_advanced.c`,
`test_memory_assemble_util.c`, `test_memory_embed_dim_guard.c`, `test_memory_fact_gate.c`,
`test_memory_filter.c`, `test_memory_health.c`, `test_memory_lanes.c`, `test_memory_profiles.c`,
`test_memory_provider.c`, `test_memory_ranker_boundary.c`, `test_memory_recall_pivot.c`,
`test_memory_redirect.c`, `test_memory_retrieval_eval.c`, `test_gw_stage_memory.c`, and
`test_workspace_memory.c`. The last is claimed here because its subject `memory_auto_tag_workspace` is
defined in `memory_core.c`, which is why slice 44 excluded it from workspace.

The `memory` name collides in two directions, so a `*memory*` filename is not an ownership signal.
DB1 owns a separate working-memory store (`src/db1/wm.c`, tested by `test_working_memory.c`), and the
root-level `src/harness_memory_*.c` files implement the memory-interception harness (tested by the four
`test_harness_memory*.c` files). Neither is claimed here, nor is `test_kb_client_memory.c` (a kb_client
test) or `test_server_memory_benchmark.c` (a server test). Together with the code curator/index tests
they cover current behavior. Provider, database, dimension, and corruption failures must surface as degraded or
failed readiness; an empty recall is a valid no-op, while silently bypassing required ranking is not.

## Operational diagnostics

Use memory and KB health output, embedding counts/version state, rerank benchmark deltas, queue status,
and `memory_health` diagnostics to separate empty knowledge from provider or index failure. Logs must
retain the concrete database, sidecar, and HTTP error rather than collapsing every failure into a generic
`index unavailable` message.

## Compatibility

Public `memory_*`, KB-client JSON, CLI, route, schema, and embedding-version contracts remain stable
during physical relocation. The target `src/modules/memory` boundary permits path and include cleanup,
but any changed response, ranking, persistence, or context bytes require an explicit compatibility and
baseline decision.

## Extension and removal

New recall sources must enter through the shared candidate, ranking, scope, and evidence contracts, not
an independent store-and-inject path. The root and `src/db2` implementation inventory is a relocation
queue; each move must prove consumers and delete duplicate glue. Removing `memory`, embedding, reranking,
or code intelligence would break the core product and is not an allowed optional profile.
