# MR-11: Versioned embedding generations and bounded index freshness

- **State:** Proposed
- **Priority:** P1: retrieval integrity and operations
- **Owner:** DB2 and personal index adapters
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md), [MR-02](memory-reliability-02-authority-preserving-mutations.md); trace integration with [MR-06](memory-reliability-06-ranking-traces-and-context-receipts.md)
- **Delivery:** Four implementation slices

## Problem and intended result

Equal vector dimensions do not mean equal embedding spaces. Changes to a model, tokenizer, input prefix, pooling or normalization can silently mix incompatible vectors. Refreshing a large pending batch during recall also makes latency depend on ingestion backlog.

Use explicit embedding/index generations, resumable background work and atomic reader cutover. Keep a small optional freshness wait bounded by the request deadline; expose remaining lag instead of pretending the index is complete.

## Existing integration points

Extend `server-go/modules/db2/lifecycle_reembed.go`, `server-go/modules/memory/{embed.go,personal_vectors.go,code_vectors.go}` and semantic-assertion indexing in `src/kb/db2_adapters/kb_service_backend_context.c`. Preserve existing content fingerprints and serving identity checks. Personal embeddings stay within the personal store and its allowed model service.

## Identity and lifecycle

Define an `EmbeddingIdentity` commitment over model/artifact revision, tokenizer revision, query/document preprocessing, prefixes, pooling, normalization, output dimensions and relevant provider configuration. Index-generation identity additionally binds chunking/extractor policy and distance/index settings. Keep secrets out of identity material.

Unknown identity is an explicit legacy state. It cannot silently join a verified generation just because dimensions match. Readiness is tracked per record class and authorized index domain.

Generation state is `created → backfilling → catching_up → validating → active → retired`, with failed/cancelled branches. Each indexed row carries canonical record ID/version, content fingerprint and generation. The indexing job has an idempotent key over those values.

## Backfill and cutover

Snapshot the canonical input watermark, backfill in bounded batches, then consume changes and tombstones through a durable queue. Before committing an embedding, recheck the record version/fingerprint and serving eligibility. Stale jobs become superseded work rather than overwriting the current vector.

Validation covers identity, coverage by record type, dimensions, content-version consistency, tombstone application and fixed retrieval fixtures. Atomic cutover updates the active generation pointer. In-flight reads pin one generation; query vectors and document vectors must match it. Multi-arm results report any independent index generations explicitly rather than implying one universal snapshot.

Keep a rollback generation only while it remains authorized and receives required deletion/revocation updates. Never restore an old generation that can resurrect removed records. Retire it under normal storage policy after the rollback window.

## Recall behavior

Move routine embedding refresh to the background queue. A read may request a small bounded settle operation only within remaining deadline and operator work policy. Return `ready`, `lagging`, `rebuilding`, `unavailable` or `identity_mismatch` with appropriate watermark information. Lexical fallback remains available if authorized; complete semantic coverage is not claimed during lag/unavailability.

Expose queue age, pending count, retry/failure rate, watermark lag, generation coverage and cold/warm latency. Do not publish sensitive record IDs as metric labels.

## Implementation slices

1. Formalize identities and generation metadata; reject known mismatches and label legacy unknowns.
2. Implement resumable queue/backfill with content-version checks and deletion priority. Remove unbounded dependence on read-time maintenance.
3. Add validation and atomic activation/rollback with pinned-reader behavior.
4. Add operator readiness/rebuild diagnostics and compatibility migration for each vector-bearing record class.

## Acceptance gates

- A same-dimension model/pooling/prefix change creates a new generation and cannot mix with the old one.
- Crash/resume does not duplicate current vectors or lose the change watermark.
- Edit, delete and revoke during backfill cannot become stale authorized vectors at cutover.
- Query-time model identity is checked against the generation it searches.
- A large indexing backlog cannot consume the entire recall deadline; fallback reports its limitations.
- Rollback honors all intervening deletions/revocations and retains provenance.

## Rollout and rollback

Migrate one record class/store at a time and measure temporary disk, queue and model load. Preserve old readers until generation-aware readers are deployed. Binary rollback requires a compatible active generation or an explicit lexical-only mode; it must not disable identity validation.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
