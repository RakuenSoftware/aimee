# kb-synthesis module

## Purpose and non-goals

`kb-synthesis` is an optional, default-off, heavyweight knowledge-curation module. It uses a capable LLM or
substantial GPU to reason across already-ingested evidence, resolve higher-order relationships, and write
cited narrative artifacts such as topic synthesis. It is not normal response-composition, required
embedding, code indexing, basic memory recall, or the deterministic ingest/index lane. There is
no reranking role: the cross-encoder was measured out of the stack and `kb_ranker` (linear, in-process)
took its place. See [Local inference](../LOCAL_INFERENCE.md).

## Public contracts

`src/modules/kb-synthesis/` owns the KB curator family: 21 sources and 16 headers relocated from
`src/kb/`: the curator pipeline and queue, extraction (evidence/code), grounding, entity resolution,
contradiction reconciliation, judging, promotion, the index writers (narrative/claims/code-unit),
artifact linking, LLM and sidecar drivers, notify/version, and `kb_curator_synthesize.{c,h}`. These are
**KB-tier** sources: they include KB-internal service headers (`kb.h`, `index.h`, `kb_service_*`,
`kb_mdl.h`, `kb_learning_synth.h`, …), so they compile with the KB build flags into
`$(OBJDIR)/kb/modules/kb-synthesis/` (the `KB_SYNTHESIS_SRCS`/`KB_SYNTHESIS_OBJS` pair) and link only
into `aimee-kb`. The curator's public headers are reached by its in-KB consumers (`kb.c`, `cmd_kb.c`,
the curator config/profile) through `-Imodules/kb-synthesis`; per the flat-layout convention the
module-root headers are declared as `private_headers`. `kb_curator_provider.c` (the provider adapter in
core, not KB-tier) and the DB2 artifact/link storage APIs stay their owners' and are consumed through
their contracts.

The separately supervised `kb-synthesis` process serves one bounded Go stage at principal 22/event
9729. It evaluates the existing code-unit grounding rule: when a parsed artifact claims no side
effects, structural callees are checked against the exact side-effecting function set and the first
contradiction is returned. The caller still owns JSON parsing, evidence authorization, queue state,
model invocation, persistence, and artifact acceptance. Production `aimee-kb` shapes each request in
`kb_curator_grounding.c`, batches an unbounded structural call graph into the bounded 64-callee wire
contract, and calls the process only through its local event bus. A missing module, malformed response,
or transport failure rolls back the artifact transaction and retries the extraction; there is no local
grounding-policy fallback. The C `module_adapter.c` remains a process-parity fixture only and is not a
second curator worker.

## Dependencies and consumers

- `config`: supplies explicit enablement, capable provider/sidecar selection, limits, and prompt versions.
- `ir`: supplies provider-neutral request/response shapes for model-backed curation work.
- `memory`: supplies embedded evidence and receives cited narrative artifacts and links.
- `module-runtime`: will supply lifecycle and extension contracts when the target optional boundary is physically separated.
- `response-composition`: supplies canonical model-result assembly without making KB synthesis part of every answer.

Consumers include KB curator workers, `/v1/synthesize`, curator CLI/status surfaces, narrative indexing and
search, and memory retrieval that may later surface committed artifacts. Core memory remains a valid
consumer even when no synthesized artifact exists.

## Providers and readiness

Every stage resolves the same provider: `provider.*` config, or failing that the deployment's single
synthesis endpoint (`config_synth_chat_endpoint`, i.e. `SYNTHESIS_ENDPOINT`), or idle.

There used to be two. The reasoning stages read a `tier_b.*` provider and were forbidden from falling
back to the mechanical stages' one, because letting a weak model serve the reasoning stages is the
graph-poisoning case the split existed to prevent. Measurement did not support running a cheaper model on
the mechanical stages, so both families resolved to the same model in practice and the split was removed:
`tier_b.*`, `AIMEE_KB_CURATOR_TIER_B_API_KEY` and the `tier_a`/`tier_b` blocks in `/v1/health` are all
gone, replaced by one `synthesis` block. See [Choosing a synthesis model](../SYNTHESIS_MODELS.md), and
[Upgrading](../UPGRADING.md) for the key mapping.

Readiness is idle, not degraded core, when disabled or unconfigured. It is ready only when storage,
source evidence, provider, prompt/version policy, and bounded worker lane are all operational.

## Configuration and activation

- `runtime_toggle.supported`: `false`; the target optional boundary is profile-gated at build/startup, not hot-toggled in a live process, and separation from the mechanical ingest/index stages remains implementation work.

The descriptor sets `enabled_by_default` to false. Current concrete gates include
`kb.curator.synthesize.enabled`, command/provider settings, source count, worker scheduling, and related
reasoning-stage controls. A future profile that excludes the physical module must hide this family from web
configuration and expose it only on the KB management GUI when the module is selected.

## Surfaces

Surfaces include KB curator health/status, `aimee kb curator`, curator synthesis commands and routes,
`/v1/synthesize`, logs under `kb.curator.synth`, and committed synthesis artifacts with `about` and
`cites` links. These belong to the KB management plane; the runtime/user GUI must not present them as
ordinary response controls.

## Data and migrations

The module reads entities, claims, evidence, and embeddings, then writes versioned `synthesis` artifacts,
features, vector rows, and provenance links through DB2/PostgreSQL. Migrations must preserve prompt/model
version, topic identity, citations, artifact state, and suppression of duplicate work; generated
syntheses are rebuildable only when their complete source evidence and version policy remain available.

## Security and privacy

Only evidence within the authorized KB scope may enter a synthesis request, and outputs must retain `cites`
provenance so generated claims can be audited. Provider credentials stay in vault/config ownership; logs
and sidecar errors must not echo keys or full sensitive documents. Model output is untrusted until parsed,
bounded, linked, and accepted under artifact policy.

## Supported journeys

When currently enabled, the curator selects an eligible high-centrality topic, gathers top-K linked evidence, sends
a grounded `synthesize_topic` request to the synthesis provider, validates the response, writes a
`synthesis` artifact, and links it about the topic and to its cited sources. Acceptance for the future
optional profile requires ingest, embedding, ranking, code intelligence, memory search, and normal
answers to remain operational when this module is omitted.

## Tests and failure behavior

`test_curator_synthesize.c`, `test_curator_serve.c`, `test_kb_curator_provider.c`,
`test_curator_pipeline.c`, and curator queue/index tests cover selection, provider separation, persistence,
serving, and scheduling. C/Go process parity tests cover only the bounded grounding decision described
above, while the curator tests inject that wire handler at the production provider seam. A missing or
failed grounding process is a visible retryable extraction failure, never a clean decision. Disabled or
unconfigured synthesis providers and no eligible topic are clean idle results; malformed output,
provider failure, or artifact-write failure must not mark a topic successfully synthesized.

## Operational diagnostics

Operators use curator health, queue depth, stage/provider readiness, prompt/model version, worker logs,
artifact/link queries, and `/v1/synthesize` results. Diagnostics must preserve decoded process/HTTP/database
errors and distinguish synthesis provider unavailable, no eligible evidence, parse rejection, and persistence failure;
core memory health must not fail merely because `kb-synthesis` is idle.

## Compatibility

Curator route envelopes, stage names, artifact kinds/states, `about`/`cites` provenance, prompt versions,
and provider-tier isolation are compatibility contracts. Moving files into
`src/modules/kb-synthesis` must not pull mechanical-stage extraction, embedding, indexing, or core response assembly
with them, and stored artifacts require explicit migration if their schema or semantics change.

## Extension and removal

Additional heavyweight reasoning passes belong here only when they consume established KB evidence,
retain provenance, share provider/readiness policy, and have a real management journey and consumer.
Self-only curator experiments should be deleted after liveness review. After the retired Tier-A/Tier-B split, the
target module must be wholly omittable: exclusion must hide its `config`, routes, GUI, and workers while
preserving all required memory journeys.
