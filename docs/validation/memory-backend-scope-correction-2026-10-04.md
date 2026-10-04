# Memory backend scope correction — 2026-10-04

The previous implementation expanded a replaceable-memory request into a parallel platform redesign. It has been withdrawn from the working tree. Historical work is archived at `/tmp/aimee-memory-scope-correction-20261004-130802`, with a tracked patch and an explicit removal manifest. No migration or deployment was performed by this correction.

| Addition | Existing mechanism or demonstrated purpose | Decision |
| --- | --- | --- |
| Extra memory binding process, switch generation state machine and lexical engine | Existing module composition, executable admission, bus routing and supervisor restart | Remove the parallel runtime machinery. |
| Database credential routing and ownership cutover | Existing PostgreSQL module and bus principal admission; not required to extract a memory API | Remove the additional router and proposed cutover. |
| Separate control WORM worker and audit adapter | Existing audit owner and WORM worker | Remove the parallel audit delivery path. |
| New session-to-memory erasure ledger and restore migrations | Existing subject erasure receipt, erasure intents and startup replay | Remove the unfinished replacement. Existing mechanisms remain. |
| Parent registry/feed/federation, unified node profiles and console engine controls | Separate node/federation proposal; no bearing on implementing another memory backend | Remove from this implementation; do not claim the earlier proposal is complete. |
| Independent memory API package and compatibility aliases | Consumers previously import the native implementation for types and framing | Retain. This is the actual contract extraction. |
| Export and fixture extensions for the removed components | No remaining runtime consumer | Remove; add only dependencies needed by the retained contract. |

The retained contract preserves existing native wire identifiers and failure behavior. Native implementation tests continue to exercise the existing system. New provider code must use this contract, rather than importing native implementation types or defining another supervisor, binding service, authority or audit pipeline.

## Corrected implementation and evidence

- Independent contract: `server-go/memory`, compatibility client/type aliases and native Store adapter. Existing wire IDs and the existing module remain unchanged.
- Cognee adapter: `server-go/modules/memory/cognee`, importing only the independent contract and standard library. Selection and transport reuse the existing memory handler, egress and Vault.
- Cleanup is an optional contract extension invoked by the existing deletion lifecycle. A failed remote cleanup stops ordinary and versioned deletion; no separate erasure ledger was added.
- Narrow source-export repairs fix demonstrably broken constructors/dependency closure for existing egress, sandbox, git, roundtable and tools exports. All 25 internal exported Go module repositories build.
- Full Go race/short suite passed; the existing real PostgreSQL `memory-owner-replay-check` passed after the cleanup. The Vault helper compiled with existing warnings-as-errors.
- A real disposable Cognee 1.6.2 API, SQLite/LanceDB/Ladybug and deterministic local model fixtures passed retrieval and deletion. This caught and corrected `only_context=true` returning plain text instead of scored CHUNKS. This tests protocol integration, not model quality or production throughput.

The first Cognee adapter is bounded to 256 eligible source records per scope and synchronous indexing. Existing extended native graph/learning/history/code contracts remain separate. The canonical subject-erasure journal was preserved and is now used for failed/unknown provider cleanup retries. Private/shared erasure completion includes verified cleanup of the selected node's Cognee derived namespace. Module startup cleans restored derived storage before readiness, while a handler gate prevents old indexing from recreating erased copies. Independently retained external backups remain outside managed live-store coverage. These limits are explicit in the [backend guide](../modules/memory.md#memory-backend-contract), rather than presented as completion of the withdrawn platform proposal.

## Completed subject-wide cleanup

- The generic `DerivedResetter` extension and internal, trusted-host-only `reset-derived` data operation reuse the existing module and wire IDs.
- Both shipping private/shared erasure coordinators invoke cleanup before completion acknowledgement. Failure keeps the existing erasure request retryable; no new SQL schema or receipt ledger was introduced.
- Cognee cleanup enumerates and deletes every dataset in this node's namespace, including historical/private/shared revisions with no remaining canonical row, then verifies absence. Other nodes are untouched. Tests cover more records than the retrieval snapshot limit.
- Lost deletion replies, falsely successful responses with retained datasets, cancelled resets, missing reset capability, successful retry and restored derived copies are covered.
- A provider request gate is acquired before opening source snapshots. A race test reproduces an old indexing write and verifies cleanup follows it; queued expired requests cannot run cleanup.
- Real Cognee 1.6.2 passes canonical subject-erasure followed by cross-scope cleanup, retained-canonical checks and repeat cleanup. Local model fixtures were used; no shared model service was involved.
- Full Go race suite, existing PostgreSQL owner/replay gate, shipping native private-erasure protocol, complete native KB HTTP route suite, host adapter builds and module JSON-call tests pass.
