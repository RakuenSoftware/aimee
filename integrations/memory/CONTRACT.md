# External memory contract, version 2

This contract describes what an external integration must provide to support Aimee's conversational memory. Cognee and Hillock share the host implementation. Native memory is exempt: it can use its own APIs, schema and algorithms. Optional native adapters and maintenance transfer tools do not make native subject to this contract.

## Ownership and composition

A **canonical owner** stores admitted records, versions, history, retry receipts and independently retained erasure controls. A **retrieval provider** selects or ranks canonical references. One service may implement both; an Aimee adapter may supply durable compatibility storage while calling a separate provider. Capabilities must identify who owns each responsibility. Calling native record storage does not count as independent external storage.

The `aimee-conversation-v2` profile requires the capabilities below. Protocol version 2 is additive; the bounded portable catalog format remains version 1. Consumers must inspect profile and operations rather than infer support from an engine name. Graph, extraction, learning, reminders, directives and native served views are separate optional profiles. Empty optional sections mean no such data is managed by this profile; they must not be advertised as implemented algorithms. Generated provider prose is never canonical evidence or policy.

## Must-haves

| Capability | Required behavior |
|---|---|
| Canonical records | Exact content, unit strings, stable positive int64 IDs, owner/revision tokens, scope, kind, tier, confidence and captured authorship. Upsert by scope/kind/key is explicit. Authenticated host context remains separate from untrusted arguments. |
| Revisions and retries | Atomic expected-version checks; retained historical payloads; historical content excluded from current retrieval. Same operation/digest returns its surviving receipt; a conflicting digest refuses. Lost/obsolete receipts do not fabricate success. |
| Audience | Exact-scope operations remain exact. Ambient shared recall admits the active project/workspace and shared/global scopes; private memory stays in its separate owner. Provider output cannot widen this audience. |
| Search at scale | A bounded candidate pool selected from the entire eligible scope, or paged provider indexing. Advertise selection policy, request/body/catalog limits and whether results are exhaustive. Candidate limits are not corpus limits. Unsupported legacy sources must explicitly refuse oversize snapshots. |
| Conversation views | Recall, personal/shared composition, briefing and context assembly use existing Aimee response shapes and whole-record budgets. Personal identity/preferences override shared keys. Provider scores confer no factual truth, identity resolution or instruction authority. |
| Source checks | Current canonical revisions and collection observations accompany recall. Check the same audience and revisions before handoff, including empty results and temporal boundaries. An acquire/release lease prevents canonical mutation during the bounded handoff window. Expired leases cannot certify a send; unresolved barriers remain until authenticated completion. |
| Erasure | Destroy current/history/retry payloads; verify derived cleanup before completion. Retain hashed IDs/subjects/sessions and applicable payload restrictions outside content backups. Restore and import replay these controls before readiness. Unknown cleanup outcomes are retryable failures. |
| Fresh admission after erasure | The authenticated host captures an admission epoch before writing. Erasure advances it atomically. Old in-flight writes and stale restores refuse; fresh admitted writes can resume. Erasure operation IDs are durable and idempotent, so retrying completed erasure does not remove later writes. Epochs cannot be supplied by model/user arguments. |
| Transfer | A bounded complete snapshot preserves current/retired records, IDs, revisions, history, authorship, metadata and erasure controls. Import is explicit into an empty owner. Merge retained erasure controls monotonically, reject prohibited payloads, discard transient leases, reset derived state and restart before serving. A bulk file transport is acceptable; RPC size limits must be explicit. |
| Failures and readiness | Invalid input, not found, revision/idempotency conflict, unsupported capability, capacity, unavailable provider and cancellation remain distinguishable. Outages do not cause implicit native fallback. Validate configured provider compatibility and reset derived state before readiness. |

## Current shared host implementation

`server-go/modules/memory/backendstore` owns a process-locked, atomically fsynced compatibility catalog. Both adapters use it independently of native memory tables. Every mutation, import and erasure shares its lock. `erasures.json` is retained separately from `records.json`; backup/restore must retain it. This is a bounded file catalog, not an unlimited or high-throughput database.

Provider pools contain at most 16 records for Cognee or 256 for Hillock, selected lexically across the whole catalog with stable tie ordering, then reranked by the selected provider. This removes the 257-record search outage. It does **not** prove exhaustive semantic recall: lexically unrelated candidates can be outside the pool. Search re-reads current canonical hits and rejects stale or foreign revisions. Cognee reconciles revision-specific datasets for each pool; Hillock uses a stateless rank request. Request deadlines bound cold indexing/re-ranking.

Limits are exposed by `memory.backend_capabilities`: 16 KiB query text, 16 Cognee or 256 Hillock provider candidates, 1 MiB provider body and 64 MiB canonical snapshot. Public snapshot RPC is further bounded by the memory bus envelope; use the offline file transfer utility for larger snapshots. Hillock additionally imposes its advertised token/vocabulary/compute limits; capacity errors must remain explicit.

Recall binds the catalog generation and observed audience, including empty views. Send guards are conservative across an entire catalog and persist across cooperating processes until explicit completion. Their five-second deadline bounds dispatch; expiry does not prove non-dispatch or release storage protection. A guarded mutation refuses and may be retried. This implementation prioritizes correctness over concurrent write throughput. Native-style graph and learned ranking parity is not promised.

Metadata is optional JSON carried unchanged by storage/transfer. Native transfer preserves original row metadata in `metadata.aimee_native_row`; it is not automatically trusted as an instruction. Validity boundaries use RFC3339 timestamps. Secret and expired/not-yet-valid records are excluded from ambient selection. Import must preserve restrictions rather than discard them.

## Portable transfer

Build `server-go/cmd/aimee-memory-transfer`. Export/import with `-operation export|import -directory ABSOLUTE_CATALOG -namespace HOST_NAMESPACE_UUID -file SNAPSHOT`. Files are created mode0600, are bounded to 64 MiB, and contain private memory: keep them out of public evidence. Never delete a target's retained erasure ledger to make import pass. The namespace is the destination's host-derived namespace; imported version owners remain the original canonical owner.

`scripts/validation/memory/native-transfer.py` supplies an **offline native maintenance bridge** using operator-owned `PG*` connection configuration. `native-export --placement server|kb --schema SCHEMA --file SNAPSHOT` exports canonical rows, private retained versions, captured authorship and native erasure restrictions. `native-import` requires an empty stopped native record owner and the generic validator utility. Native keeps its own schema; migration preserves mechanical revisions under an exclusive lock while its erasure guards remain enabled. Native-only graphs, vectors, learned state and control services are outside this record snapshot; rebuild derived state after transfer.

## Nice-to-haves

- Incremental persistent provider indexing with resumable jobs, freshness watermarks and model/index generations, avoiding repeated cold indexing.
- Provider pagination and semantic candidate routing for large catalogs.
- Typed extraction with immutable source spans, explicit entity IDs, units, polarity and evidence families.
- Graph paths, provenance-aware conflicts, scoped learning and rollback/evaluation controls.
- An extensible distributed canonical store behind the same host interface.

These become must-haves when an integration advertises the corresponding profile. They are not prerequisites for supporting Aimee's core conversational record workflow.
