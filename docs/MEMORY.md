# Memory implementation boundary

Aimee retains its generic memory SDK and host/module transport. The bundled memory executable is an explicit unavailable adapter: it never attaches, opens storage, advertises stages, or falls back to a built-in datastore. Install an independent memory implementation and select its exact executable using the existing role manifest and executable grants.

The private aims-db product and Aimee memory frontend are independent modules. Availability depends on the selected implementation and its actually declared contracts. Similar stage numbers do not imply legacy ABI1/2/4 compatibility. The public adapter does not implement memory CRUD, recall, ontology policy, memory maintenance, evaluation, or provider source fences.

The old Go implementation and its evaluation/test assertions are preserved as the immutable private aims-db experiments/legacy-aimee-go baseline, with separate consumer sidecars. That baseline is experimental comparison material and is never a default or runtime fallback. Historical decisions remain in [migration history](validation/memory-migration-history.md).

Public CI separately tests the unavailable adapter, generic SDK, canonical DB1 producers/access/erasure/replay, and PostgreSQL provider. These tests do not certify the installed external memory implementation. Its real process, datastore, provider-send and recovery qualification must accompany deployment. Old MR18 and transitional serving-owner certification are explicitly unavailable after extraction; their frozen manifests remain unchanged.
