# Independent durable Cognee/Hillock baseline

- **State:** done — baseline scope, 2026-10-08.
- **Evidence:** [validation](../../validation/replaceable-memory-backends-2026-10-08.md).
- **Remaining:** [extended parity, native migration and scale qualification](../pending/external-memory-engine-scale-and-quality.md).

Selecting Cognee/Hillock now constructs an adapter-owned durable compatibility catalog without opening native PostgreSQL memory storage. Each engine/placement has an isolated catalog. Native remains the existing default backend.

Implemented: canonical CRUD/upsert, scoped current search and stable-ID enumeration, authorship retention, conditional revisions and historical reads, idempotency conflict/replay refusal, locked atomic persistence/restart, scoped portable export/import between external catalogs, physical subject erasure with retained restore controls, provider cleanup before acknowledgement/readiness, public chatbot CRUD commands and supported-operation discovery. Content-free mutation observations use the existing audit bus.

This completes independent baseline storage, not upstream-owned Hillock graph storage or full native feature parity. Rich claim/validity/utility integration, native migration, advanced served views/learning/procedures, scale and live chatbot rollout remain in pending.

Fresh guest acceptance exposed public export invocation and post-erasure admission gaps despite implemented catalog primitives. Those remaining shipping behaviors stay in the [pending replacement proposal](../pending/external-memory-engine-scale-and-quality.md); see [actual .253 evidence](../../validation/memory-backends-lxc-253-2026-10-08.md). This done item covers the implemented storage foundation only.
