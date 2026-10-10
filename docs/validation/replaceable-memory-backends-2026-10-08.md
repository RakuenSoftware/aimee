# Independent durable memory backend validation — 2026-10-08

Baseline implementation is locally validated. Native remains the default. Cognee and Hillock now use separate adapter-owned durable compatibility catalogs; their production constructor returns before native PostgreSQL storage initialization. The upstream engines supply retrieval, not canonical authority. This is independent baseline replacement, not complete advanced native feature parity or a live Discord rollout.

## Implementation

- `server-go/modules/memory/backendstore`: private durable catalogs, process locks, atomic/fsynced commits, stable upsert identity, exact versions, history, CAS, replay conflict/refusal, scoped paging and export/import, retained hashed subject/session erasure controls.
- `external_store.go`: host-bound scope/authorship/admission, baseline context assembly, typed failures, scoped migration and required derived cleanup. No native record datasource is supplied.
- `process.go`: selected alternatives initialize their catalog and governed provider directly, verify cleanup before readiness, and publish content-free mutation observations through the existing audit bus. Native PostgreSQL setup/workers are bypassed.
- Existing C owner erasure coordinators forward verified subject/session erasure to the selected backend before acknowledgement. Erasure replay metadata must be retained when restoring old catalog files.
- Public baseline CRUD, supported-operation discovery and `backend_capabilities`, `backend_list`, `backend_export`, `backend_import` use existing command framing. Export/import requires verified human authority and an empty destination. Advanced unsupported operations never fall back to native SQL.

## Checks and evidence

`go test -race ./memory ./modules/memory ./modules/memory/backendstore ./modules/memory/cognee ./modules/memory/hillock ./modules/egress` passes locally. These tests include optional PostgreSQL/live tests that skip without fixture configuration; the real-provider runners below explicitly require executed live passes.

`TestDurableBackendConformance` and `TestDurableBackendConcurrentWritersAndPaging` cover restart, exact source/authorship, historical corrections, CAS/retry refusal, retirement/deletion retries, scope isolation, external catalog migration, concurrent writers/paging/cancellation and erase→restore→restart with retained control metadata. `TestExternalBackendHandlerWithoutNativeStorage` supplies no PostgreSQL/native store, verifies public chatbot CRUD/search, scoped public export/import, human authority enforcement, erasure and stale restore refusal.

Real Cognee 1.6.2: `scripts/validation/memory/run-cognee-contract.py` executed `TestCogneeLiveContract` successfully with an independent on-disk catalog reopened before adapter construction. Its real HTTP API uses deterministic fixture model/embedding endpoints. [Executed test](replaceable-memory-backends-2026-10-08/cognee-contract.log), [summary](replaceable-memory-backends-2026-10-08/cognee-summary.json).

Real Hillock: `scripts/validation/memory/run-hillock-contract.py` executed against pinned upstream revision `1edd166ead75b85a9ab95cd6ba4faf7011ad567c`, with an independent durable catalog and authenticated stateless sidecar. Both process rounds passed, including corrections, deletion, changed revisions, capacity/auth/validation and no retained cross-request corpus. [Executed tests](replaceable-memory-backends-2026-10-08/hillock-contract.log), [summary](replaceable-memory-backends-2026-10-08/hillock-summary.json).

C integration targets: `unit-test-module-json-call`, `unit-test-server-erasure-protocol`, and `unit-test-kb-http-routes` (with its required `AIMEE_TEST_RUNTIME_FIXTURE`) exercise thin framing/erasure forwarding. Boundary, egress, proposal link/audit/reconciliation checks and generated reference synchronization are checked separately.

## Remaining production gate

Native PostgreSQL ↔ external snapshot migration, rich lineage/validity/utility integration, advanced served views/claims/procedures/learning parity, representative corpus/latency qualification and live chatbot rollout remain pending. Current engine query snapshots have a 256-record limit and return capacity instead of silently truncating. The catalog has an explicit 64 MiB bound; RPC snapshots have a smaller wire bound. Files use host-private permissions and POSIX process locks. Local tests do not establish answer-quality superiority, upstream graph safety, a successful Docker build, remote CI status or deployment.

See [remaining work](../proposals/pending/external-memory-engine-scale-and-quality.md), [Hillock gaps and priorities](../../integrations/hillock/CAPABILITY_GAP_ASSESSMENT.md) and [specific upstream interfaces](../../integrations/hillock/FULL_SUPPORT_CONTRACT.md).
