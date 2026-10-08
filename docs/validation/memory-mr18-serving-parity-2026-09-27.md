# MR-18 serving parity matrix

This matrix describes the PR #2990 candidate after MR-17 (`53d49b031`) and the
MR-18 unit-semantic source-version repair. The [final release report](memory-mr18-release-2026-09-27.md) pins executable
`1fac4f35c`, fresh CLI/MCP/HTTP view, action and clean-retry execution and the
173-Go/40-Python frozen gate. The linked earlier process runs retain their original
image identities; they are not measurements of a later image. Source review and
current automated execution are separate evidence classes.

The owner is Go memory in both placements. Server owns private/user memory in
DB1; KB owns shared global/workspace/project memory in DB2. Native hosts own
credentials, resource/tool execution, transport and provider handoff. No native
memory implementation or fallback is permitted. PostgreSQL runtime roles are
non-owner, without BYPASSRLS; administrative schema bootstrap is separate.

| Surface | Owner/store and scope | Modes, dense/graph and index state | Activation/workspace and budgets | Receipt boundary | Evidence and unsupported behavior |
|---|---|---|---|---|---|
| CLI `memory` | Explicit `store=user` or `store=kb`; verified caller scope | Current reads, explicit retained-version/history commands; KB lexical + versioned whole/unit semantic + eligible graph | Host workspace/project; explicit scope is a restriction; owner limits | Diagnostic preview; provider receipt only when dispatched | MR-01/02 CLI/live fixtures; current Go owner tests and C-bus conformance. Private graph and unsupported historical query modes explicitly refused. |
| MCP memory tools | Same Go owner; host supplies principal/audience, numeric IDs do not choose store | Current retrieval, served views/claim cards, explicit history; same KB lanes | Forwarded activation/project/workspace; owner clamps final projection | Preview distinct from admitted/sent | MR-01–14 live MCP evidence; exact shared-file integration check preserves unrelated LSP behavior. `memory_hygiene` queues proposals; never executes canonical cleanup. |
| HTTP Server/KB | Authenticated Server private store or scoped KB route; no cross-store fallback | Same command table/owner; explicit unsupported modes fail | HTTP limits plus owner limits, host-owned session binding | HTTP success does not prove provider send | MR-01/02 exact-set and scope fixtures; MR-12–17 process evidence; native KB route suite. Service credentials are not user authority. |
| Existing C event bus / Go module client | Host grants principal/stage; placement-specific Go process | Versioned data/command/runtime contracts; same owner decisions | Deadlines/cancellation propagated; bounds checked before decoding/dispatch | Module result only; no fabricated downstream receipt | Current real C-host/Go-process conformance; Server/KB placement isolation, unavailable/restart behavior, CGO=0 build. Unsupported contract versions reject. |
| Ingress/context assembly | Go selects private and shared evidence under host audience | Typed projections, coverage/recovery, current versions and lineage; readiness reported | Activation allowlist/workspace forwarded; byte/token ceilings include protected rendered sections | Plan/projection commitment; final pack may remove evidence | MR-03–06/11–13 fixtures and current `ingress_assembly` tests. Unknown/incomplete lineage never counts as independent corroboration. |
| Final provider dispatch | Native host hands exact final request to owner guard | Revalidates current source versions, scope, policy/index identity | Exact provider-shaped bytes; changed payload requires fresh admission | Durable admission → handoff → acknowledgement, unresolved gaps stay unknown | MR-06/08/16/17 controlled transports/restarts; wire-fence and receipt tests. Commitment alone is not replay proof. |
| Scheduled maintenance/index workers | Go owner with narrowly scoped worker authority | Future-valid indexing allowed; current release still checks time; same-dimension identity drift rejected | Bounded batches/backlog; exact source revision and generation | Durable change journal/checkpoint; publication alone does not prove erasure | MR-02/11/14 fixtures and current lifecycle tests. Hygiene findings require version-bound human review; no autonomous canonical edits. |
| Compatibility APIs | Existing external C ABI/HTTP adapters marshal to Go | Documented retained interfaces; no parallel policy implementation | Advertised supported fields forwarded; owner rejects unsupported semantics | Compatibility response, not provider receipt | Frozen native ownership ledger, negative C/cgo fixtures and MR-01 serving inventory. Full DB2 retirement remains a separate proposal. |
| Actions and clean retries | DB1 durable root journal + current Go memory evidence check + native exact-resource adapter | Supported action adapters; failed retry starts fresh memory plan after corrected input | Shared parent/child root reservations; exact provider bytes and operator byte-cost estimate | Effect-confirmed only for verified adapters; unknown mutation blocks conflicting retry | MR-16 42 and MR-17 20 deployed checks. Retry token-cap enforcement is unsupported and refused; in-flight cancellation/physical TTL erasure are not claimed. |

The [MR-01 detailed route inventory](../proposals/done/memory-reliability-01-serving-inventory.md)
expands the grouped read surfaces. [MR-16](memory-mr16-actions-2026-09-27.md) and
[MR-17](memory-mr17-retries-2026-09-27.md) document exact adapter and retry limits.
The immutable native inventory and reviewed external-owner ledger remain in
`tests/baselines/modules/`; this matrix does not replace those inventories.
