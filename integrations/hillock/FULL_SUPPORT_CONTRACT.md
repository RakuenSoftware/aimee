> Current implementation (2026-10-08): canonical CRUD, scoped listing, history/CAS, retry receipts and retained erasure controls are supplied by Aimee's backend-owned compatibility catalog. The rank/health sidecar is implemented against pinned upstream code. The routes below describe what Hillock should expose to own these primitives directly or add scalable/graph/learning support; they are not claims that upstream currently implements them. Portable native record/history transfer is implemented; optional extended feature parity and workload qualification remain pending.

# Hillock upstream capability roadmap

This is a proposed versioned API contract, not an assertion that upstream Hillock already provides these routes. The POC supplies `/v1/health` and bounded stateless `/v1/rank`; upstream currently exposes a chat-completion API. Aimee retains admission, policy authority, human review, privacy orchestration and reply composition. The selected memory backend must own durable records, not require native memory as a hidden canonical store. Hillock must expose retrieval/extraction/derived-state primitives rather than become another authority or send generated chat prose as evidence.

The normative, reusable integration contract is [memory/CONTRACT.md](../memory/CONTRACT.md). Its conversation profile is supplied by the supported Aimee catalog plus Hillock ranker. The routes below are a roadmap for upstream-owned storage and optional features; they are not additional requirements to run the current integration.

## Requirement classification

[The capability gap assessment](CAPABILITY_GAP_ASSESSMENT.md) maps observed upstream shortcomings to Aimee needs and separates must-haves from optional features. The interfaces below are a proposed feature roadmap: persistent indexing/jobs, TALON extraction, semantic expansion, graph reasoning and Hebbian learning are not prerequisites for a bounded stateless retrieval backend. Their provenance/isolation/lifecycle controls become mandatory if those features are enabled. Aimee's canonical authority and conversational memory remain existing host responsibilities.

## P0: actual durable backend replacement

The current POC and Cognee adapter use independent adapter-owned durable compatibility catalogs. Native is not required to comply with this external integration contract. Full replacement requires the selected backend or its backend-owned compatibility layer to expose:

- Durable Get/Put/Delete and eligible Search/list with stable IDs, upsert semantics, scoped pagination and idempotent operation IDs.
- Required revision-bound mutations and preserved current/history/conflict/provenance/validity metadata. Aimee supplies policy/admission; the backend enforces atomic storage preconditions.
- Verified record/subject erasure across canonical and derived state, restore/restart replay and explicit unknown effects.
- Portable export/import preserving source IDs, revision/history, authorship and retained erasure metadata.
- Shared conformance with native memory record storage disabled for alternatives. A read/write facade that forwards to native does not pass.

Exact route names and compatibility storage are implementation choices. Existing Aimee policy/audit/control-plane infrastructure remains shared; optional graph and learning features do not have to duplicate native algorithms unless advertised as supported.

## Shared record and failure contract

Accept canonical IDs as positive 64-bit integers and preserve Aimee's `scope`, `tier`, `kind`, `key`, `content`, `confidence`, version/owner identity, historical marker, authorship/provenance and utility-horizon metadata, or reference the exact canonical revision where Aimee supplies those fields. Source spans need document/message ID and byte offsets into immutable original text. Speaker identity is supplied by the authenticated host, never inferred as authorization from message text. Aimee constructs node/audience namespaces and eligibility filters; Hillock cannot expand them.

Expose stable machine-readable outcomes for invalid input, unsupported operation/profile, missing record, revision conflict, capacity, unavailable dependency, deadline/cancellation and unknown effect. Include a correlation/job ID without echoing private data. Capabilities must explicitly say when canonical CRUD/history, graph or learning operations are delegated to Aimee rather than silently claiming support.

## P0: persistent, scalable retrieval backend

| Interface to expose | Required inputs and outputs | Acceptance requirement |
|---|---|---|
| `GET /v1/capabilities` | Protocol/engine versions, build revision, enabled profiles, supported operations, request/corpus limits, score meaning, persistence policy and index/model generation | Aimee rejects unsupported versions or missing capabilities before readiness. Distinguish lexical HDC, semantic retrieval and graph modes. |
| `POST /v1/snapshots`; `PUT /v1/snapshots/{id}/pages`; `POST /v1/snapshots/{id}/commit` | Host-bound node/audience namespace, source generation, page sequence/hash, records `{id, revision, kind, tier, key, content}`, terminal record count; commit returns a durable snapshot/generation ID | Atomic publication of a complete authorized corpus, resumable/idempotent upload, no partial snapshot serving, explicit capacity/backpressure. Old/in-flight pages cannot resurrect a retired revision or an erased namespace. |
| `POST /v1/records:upsert` | Namespace, canonical ID, revision, expected prior revision, content and operation ID; durable accepted/indexed revision or job ID | Idempotent retry, conditional revision conflict, no lost updates. Canonical content changes through Aimee's admitted API backed by the selected durable store; this operation updates a separate derived index if present. |
| `POST /v1/search` | Namespace, committed snapshot/index generation, query, eligible ID/revision filter, kind/tier, limit/cursor, deadline and optional retrieval requirements | Returns only `{id, revision, score, source_spans, route}` plus exhaustion/coverage and generation. No generated answer. Never cross tenant, silently drop candidates, mix generations or claim a result is complete after truncation. |
| `POST /v1/records:forget` | Namespace, ID, all revisions, operation ID and erasure watermark | Verified durable deletion from graph, vectors, synapses, reservoirs and caches; absence proof/counts or a pollable job. Idempotent when absent. An unknown outcome is not success. |
| `POST /v1/namespaces/{id}:reset` | Host-authorized namespace, operation ID/watermark | Verified complete reset without affecting other nodes/audiences; used at restore/startup and subject erasure. Reject stale writers predating the watermark. |
| `GET /v1/jobs/{id}`; cancellation | Upload/index/delete/rebuild status, accepted source generation, progress, retry classification and durable result | Bounded deadlines, cancellation, explicit queued/running/complete/failed/unknown states. Acknowledging a write is distinct from searchable index completion. |

For a persistent provider these are required before replacing the POC's stateless profile. Aimee can host the public Get/Put/Delete API and policy checks, but its implementation must call the selected backend's durable store for complete replacement. The current adapters use their own durable compatibility catalogs, not native record storage. Persistent derived Hillock indexing remains a separate extension.

## P1: Hillock extraction and graph support

| Interface to expose | Required behavior |
|---|---|
| `POST /v1/extract` | Accept original message/document spans and immutable source/message IDs, speaker IDs, timestamps, scope and extraction profile. Return **proposed** entities/aliases/triples/claims with exact source offsets, normalized values and units, temporal qualifiers, confidence/calibration basis, negation, uncertainty, speaker attribution and claim type (self-report, third-party report, quotation, joke/fiction/test). No direct authoritative graph writes. |
| `POST /v1/graph:query` | Return bounded paths/edges linked to canonical claim IDs and source revisions; include edge type, direction, provenance, temporal validity and generation. Inferred edges identify supporting paths and remain distinct from asserted facts. |
| `POST /v1/entities:resolve` | Return candidate entity IDs, aliases, type and disambiguation evidence. Preserve uncertainty; do not merge a Discord person and a mountain because their names overlap. Entity merge/split are versioned proposals requiring Aimee review. |
| `POST /v1/claims:inspect` | Explain supporting sources, independent lineage groups, contrary/retired claims and how a score was formed. A bot paraphrase must not become another independent supporter. Correcting/retracting an input invalidates every derived edge/reservoir contribution. |
| `POST /v1/generations:build`; inspect/activate | Separate active and staging model/vector/HDC generations, pin tokenizer/embedding vocabulary/encoder settings, expose progress and atomic activation/rollback. No mixing incompatible spaces or background downloads during serving. |

Aimee's claim eligibility policy, historical/current selection rules, review/promotion and independent-support accounting remain authoritative; their required records/history must persist in the selected backend. Hillock must preserve the fields or link back to their canonical owners; it must not flatten them into an unversioned `(subject, predicate, object)` tuple.

## P2: Hebbian/adaptive learning and operational qualification

1. `POST /v1/feedback`: authenticated, idempotent feedback linked to query group, candidate ID/revision, exact decision-time policy/version, selection/sampling propensity, outcome source and timestamp. Human feedback and observed task outcomes are distinct from bot engagement/agreement. Missing propensity is explicit; never invent IPW probabilities retrospectively.
2. `GET /v1/policies/{id}` and inspect/activate/rollback operations: versioned synaptic/ranking state, training lineage, held-out evaluation and bounded influence. Decay and reservoir state are scoped by node/audience and resettable; reading must not silently train on an untrusted conversation.
3. Retrieval receipts and aggregate diagnostics: candidate counts, omission/capacity reasons, corpus/index lag, route/exposure, timings and token/model cost basis. No raw private text or credentials in routine logs; export/delete learned state and provenance for erasure.
4. Backup/restore and health/readiness: consistent snapshot with source/index generations and erasure watermarks, corruption detection, interrupted-job replay, authenticated readiness, aggregate resource metrics and bounded queue limits. Restoring a backup must replay retained erasure intents before serving.
5. Reproducible real-engine qualification: multi-tenant concurrent update/search/erase/restore tests; correction races; stale snapshot refusal; large-corpus pagination; clock/retirement/history eligibility; exact-unit/source conversational trajectories; and powered paired answer-quality, p95 latency and cost comparisons. Publish actual limits and rollback settings before promoting a persistent/adaptive profile.

## Work Aimee must own

Hillock endpoints alone will not fix chatbot memory. Aimee still needs backend-neutral durable speaker-attributed episodes and conversation projections; intent-aware assembly from served views and claim cards; admitted input capture independent of generated wording; helpful uncertainty/source-aware replies; and the end-to-end conversational tests in [the pending proposal](../../docs/proposals/pending/conversational-memory-and-human-interaction.md). Bot conversation turns remain unlimited. The Store contract does not automatically replace Aimee's native history, code index, procedures or learning APIs.
