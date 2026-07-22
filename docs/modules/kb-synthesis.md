# kb-synthesis module

## Purpose and non-goals

`kb-synthesis` is an optional, default-off, heavyweight knowledge-curation module. It uses a capable LLM or
substantial GPU to reason across already-ingested evidence, resolve higher-order relationships, and write
cited narrative artifacts such as topic synthesis. It is not normal response-composition, required
embedding/reranking, code indexing, basic memory recall, or the deterministic Tier-A ingest/index lane.

## Public contracts

Current contracts include `src/kb/kb_curator_synthesize.h`, `kb_curator_provider.h`, curator pipeline
stages, serve endpoints, and DB2 artifact/link APIs. The descriptor directory currently has no source;
implementation is distributed across `src/kb`, `src/db2`, root curator profile/config, and HTTP/CLI code.
That placement is migration debt and should be narrowed only after separating required ingestion stages.

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

Tier-B stages require an explicitly configured capable provider through `tier_b.*` or a synthesis sidecar;
`kb_curator_provider_for_stage` deliberately provides no Tier-A environment fallback. Readiness is idle,
not degraded core, when disabled or unconfigured. It is ready only when storage, source evidence, provider,
prompt/version policy, and bounded worker lane are all operational.

## Configuration and activation

- `runtime_toggle.supported`: `false`; the target optional boundary is profile-gated at build/startup, not hot-toggled in a live process, and separation from Tier-A stages remains implementation work.

The descriptor sets `enabled_by_default` to false. Current concrete gates include
`kb.curator.synthesize.enabled`, command/provider settings, source count, worker scheduling, and related
Tier-B stage controls. A future profile that excludes the physical module must hide this family from web
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
a grounded `synthesize_topic` request to the capable Tier-B provider, validates the response, writes a
`synthesis` artifact, and links it about the topic and to its cited sources. Acceptance for the future
optional profile requires ingest, embedding, reranking, code intelligence, memory search, and normal
answers to remain operational when this module is omitted.

## Tests and failure behavior

`test_curator_synthesize.c`, `test_curator_serve.c`, `test_kb_curator_provider.c`,
`test_curator_pipeline.c`, and curator queue/index tests cover selection, provider separation, persistence,
serving, and scheduling. Disabled/unconfigured providers and no eligible topic are clean idle results;
malformed output, provider failure, or artifact-write failure must not mark a topic successfully synthesized.

## Operational diagnostics

Operators use curator health, queue depth, stage/provider readiness, prompt/model version, worker logs,
artifact/link queries, and `/v1/synthesize` results. Diagnostics must preserve decoded process/HTTP/database
errors and distinguish Tier-B unavailable, no eligible evidence, parse rejection, and persistence failure;
core memory health must not fail merely because `kb-synthesis` is idle.

## Compatibility

Curator route envelopes, stage names, artifact kinds/states, `about`/`cites` provenance, prompt versions,
and provider-tier isolation are compatibility contracts. Moving files into
`src/modules/kb-synthesis` must not pull Tier-A extraction, embedding, indexing, or core response assembly
with them, and stored artifacts require explicit migration if their schema or semantics change.

## Extension and removal

Additional heavyweight reasoning passes belong here only when they consume established KB evidence,
retain provenance, share provider/readiness policy, and have a real management journey and consumer.
Self-only curator experiments should be deleted after liveness review. After the Tier-A/Tier-B split, the
target module must be wholly omittable: exclusion must hide its `config`, routes, GUI, and workers while
preserving all required memory journeys.
