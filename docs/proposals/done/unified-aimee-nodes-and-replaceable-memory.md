# Replaceable Aimee memory

- **State:** done.
- **Archive notice — 2026-10-08:** The implementation-independent Store contract and native/Cognee integration are implemented and verified. Hillock HDC retrieval is added in this work; unified nodes were explicitly excluded from the corrected scope. Implementation completion is limited to that scope. Historical planning and earlier checkpoints below are retained as evidence, not an active backlog.
- **Remaining work:** [external-memory-engine-scale-and-quality.md](../pending/external-memory-engine-scale-and-quality.md).


Status: corrected memory-contract scope implemented and verified on 2026-10-04. This replaces the previous unified-node/provider-binding proposal. The previous text is archived with the removed implementation. Node unification and parent connections require a separate proposal; they are not prerequisites for changing memory.

## Goal clarification — 2026-10-08

The user requires interchangeable durable native/Cognee/Hillock memory backends. The implementation archived here extracted the contract and replaced retrieval; its external adapters still delegate canonical records to native Store. This is a completed foundation, not completion of the broader replacement goal. Durable backend ownership, required lifecycle/history interfaces and a native-independent conformance gate remain [pending](../pending/external-memory-engine-scale-and-quality.md). The earlier corrected scope below is historical and does not override this clarification.

## Decision

Extract the current memory API into an implementation-independent contract. Memory implementations adhere to that contract and are hosted through the existing module infrastructure. Native memory remains the default. Cognee is the first alternative retrieval implementation.

Aimee retains the existing admission, authorization, canonical record identity, audit, version checks, lifecycle and cancellation behavior. These remain existing owners and contracts; this change does not add another authority process, binding process, credential router, deletion ledger or audit worker. There remains one personal runtime owner, with shared knowledge supported by the existing scopes.

## Contract and application

`server-go/memory` contains the existing scopes, wire identifiers, request/response types, client framing and a `Store` interface derived from the current `DataStore`: Get, Search, Put and Delete. `Record` preserves integer identity, scope, content, kind, tier, confidence and optional version/provenance fields. Structured admission refusal remains distinct from missing capability and unavailable backend.

`ClientStore` applies Store over the existing module bus API. `NativeStore` adapts the native implementation; `ContractDataStore` lets any contract implementation serve the existing baseline data API. `WithMemoryBackend` injects a backend factory in the existing memory handler. Providers import the contract rather than native SQL or module implementation types. Existing extended operations remain separate native contracts; implementing Store does not claim to implement them.

The optional `DerivedStore.Forget` operation removes derived retrieval state without deleting canonical records. Existing admitted versioned and ordinary deletion paths invoke it before committing retirement/destruction. Its failure must prevent acknowledging a successful canonical mutation. This does not establish distributed atomicity: a failed canonical commit may require reindexing derived state.

## Cognee

The Cognee adapter uses the generic Store for authorized canonical records and the host's existing governed egress transport for Cognee. It reconciles revision-specific datasets for the requested scope, synchronously cognifies them, and queries CHUNKS with retrieval-only context. Dataset names bind node, audience, record ID and revision/content. Returned IDs resolve to canonical records and are revalidated before release; remote generated text is never accepted as a canonical record.

Selection uses `AIMEE_MEMORY_BACKEND=cognee`, `AIMEE_MEMORY_BACKEND_URL` (or `AIMEE_COGNEE_URL`), and `AIMEE_MEMORY_BACKEND_AUTH=bearer|none`. The existing Vault credential is `AIMEE_MEMORY_BACKEND_TOKEN`. Native is selected by an empty value, `native` or `aimee-native`. Unknown configuration fails startup rather than falling back. Egress restricts calls to the configured origin and required Cognee routes.

This first adapter bounds a scope snapshot to 256 eligible records and 1 MiB response bodies. Exceeding the bound returns an explicit capacity error, never a truncated corpus disguised as complete retrieval. Cognee indexing latency and API compatibility must be validated on the deployed Cognee release. Larger corpora need a paginated contract and indexing through existing job facilities.

## Validation and delivery criteria

- Existing native behavior, wire identities and module admission remain compatible.
- Independent contract consumers compile without importing native memory.
- Cognee contract tests verify routing, dataset reconciliation, provenance, scope isolation, stale-result refusal, cancellation and failed deletion retry.
- The existing PostgreSQL owner/replay checks and native builds pass.
- Descriptor/export checks include the independent contract and adapter without adding modules.
- A real Cognee deployment verifies the supported API version, readiness and score semantics before this adapter is described as deployment-ready.
- Subject-wide erasure and restore now use `DerivedResetter.ResetDerived` through the existing owner workflow. Private/shared completion is blocked until verified cleanup. Startup resets this node's managed derived namespace before readiness, and provider request serialization prevents old indexing from recreating erased copies. Failed/unknown cleanup retries use the existing request ID and journal; other nodes and canonical retained records are preserved. Real Cognee, native protocol, race and PostgreSQL replay tests verify these paths.

No deployment, data migration, source-storage cutover or supervisor redesign is authorized or required by this change.
