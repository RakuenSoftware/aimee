# db3 module contract

This page carries routing, admission, fallback, and revalidation.
[`docs/modules/db3.md`](modules/db3.md) is the shorter module contract and the entry point from the
[module index](modules/README.md).

## Purpose and ownership

`db3` is the provider-neutral vector serving contract owned by DB2. Pgvector remains physically
inside DB2, shares DB2's PostgreSQL transactions and schema, and serves portable reads only while no
ready external DB3 provider is deployed. An admitted, ready external provider such as Qdrant becomes
the portable-read default automatically. DB2 and its pgvector copy remain the canonical integrity,
transaction-coupled read, and recovery authority.

DB3 provider events are protocol events, not module stages. The canonical registry at
`src/modules/protocol-contracts.json` reserves the high half of the 32-bit event-kind space for
provider-neutral protocols: bit 31 is set, bits 30 through 16 hold a nonzero protocol ID, and the
low 16 bits hold a nonzero event ID. DB3 owns protocol ID 3, so version-one events occupy
`0x80030001` through `0x80030005`. Module stages remain in the low half and cannot collide even as
the selected provider principal changes. Protocol ID zero is invalid, and future protocols must be
added to the sorted registry before their catalog can generate artifacts.
Module authors have at most 255 stages in a principal's 256-kind carve. With that maximum stage,
the largest low-half principal reference is 8,388,591 (`0x7fffef`); process-contract validation
rejects either a larger carve or any stage event that reaches `0x80000000`.

The logical DB3 router receives a canonical principal only when its Go executable and first served
route-control stage land. The process-contract schema deliberately forbids reserving a principal
without a real process and stage. Each deployed provider has a distinct strict principal,
executable identity, and runtime grant. No provider descriptor or production grant exists until
that provider ships.

## Routing contract

Committed vector mutations are idempotent notifications from the DB2 outbox. Every admitted
write observer receives the same operation and reports its own watermark. A duplicate operation or
generation must be recognized without duplicating an effect. The provider capability generation is
the shared corpus epoch; the operation ID is the monotonically replayed delivery watermark. One
failed or lagging observer never
changes which provider serves reads.

Portable reads are request/reply, never observer fanout. Exactly one admitted and ready external
principal holds the search route. Deployment admission selects the lowest eligible principal, so
multiple providers have a deterministic default independent of capability arrival order; all remain
write observers. Control may explicitly override that choice, and `clear` restores the deployed
default. With no ready external provider, DB2 uses its internal pgvector implementation. Reads that
depend on transaction-local or snapshot-consistent PostgreSQL state never enter this router and
remain on DB2/pgvector. If an explicitly selected provider is unavailable, DB2 fails closed unless
the route explicitly enables fallback; fallback records the selected principal, failure, and
`explicit_fallback` provenance. Automatic selection never silently opts into fallback.

DB3 returns bounded opaque point IDs and finite scores. DB2 rehydrates every candidate and repeats
tenant, project, lifecycle, quarantine, and classification checks. Provider responses never confer
authority and never contain SQL, table names, connection data, provider query JSON, or source rows.

## Provider frames and the compatible apply-v2 extension

Every multibyte integer and float is little-endian. Each payload starts with its own magic, wire
version, and fixed header length; unknown flags, unknown closed-enum values, nonzero reserved bytes,
length mismatches, nonfinite numbers, zero identities, and values above the catalog limits are
malformed. Provider identity is never a payload field.

| Payload | Required semantics |
| --- | --- |
| `capabilities` | Generation; ready flag; search/apply operation bits; cosine/L2/dot metric bits; exact-filter bit; maximum dimension, batch, and top-K. Ready evidence requires a nonzero generation. Search requires a metric and nonzero dimension/top-K; apply requires a nonzero batch limit. |
| `apply` v1 | Nonzero operation and corpus generation, positive opaque point ID, upsert/delete/tombstone kind, bounded collection, and a finite vector only for upsert. V1 remains decodable for compatibility but cannot represent filter-correct external projections. |
| `apply` v2 | The v1 fields plus one to 16 canonically sorted exact labels. Keys are unique lowercase identifiers of at most 31 bytes; values are printable ASCII of at most 255 bytes; the encoded label section is at most 4096 bytes. Deletes and tombstones carry no labels. DB2 uses v2 for every external upsert. |
| `apply_chunk` | Operation ID, total encoded apply length, exact offset, and a nonempty chunk. Offset zero restarts an incomplete retry; every subsequent chunk must be contiguous and retain the same total. |
| `applied` | Operation and corpus generation, durable provider watermark, lag, and `ok`, `retryable`, `rejected`, or `internal`. An `ok` watermark cannot precede the acknowledged operation ID. |
| `search` request | Request ID, exact required generation, workspace/project scope, record type, bounded top-K, and finite vector. At least one scope is present. Request/reply fragmentation and cancellation use the bus directly. |
| `search` success | Matching request ID and exact generation, then at most requested top-K unique positive point IDs with finite scores. |
| `search` failure | Matching request ID and `invalid_request`, `unavailable`, `retryable`, or `internal`; provider prose and backend details never cross the boundary. |
| `route` request | `query`, `select`, or `clear`, a request ID, and, only for select, the authenticated provider principal, observed capability generation, and explicit-fallback bit. `clear` removes an explicit override and restores the deterministic deployed-provider default. |
| `route` reply | Typed result (`ok`, `not_found`, `not_ready`, `generation_conflict`, or `invalid`) plus the current selected principal, provider generation, and fallback bit. A failed selection leaves the previous route intact. |

The non-apply frames remain version one. Apply v2 reuses the apply event kind and magic but has its
own version and longer fixed header; decoders accept both versions and encoders retain byte-identical
v1 output when no labels are present. A new closed bit or operation needs a catalog update and
conformance fixtures; reusing a bit, changing an existing field, or weakening validation requires a
new wire version and a dual-version compatibility window. Provider implementations must pass the shared
codec, malformed-frame, duplicate-apply, multi-observer, selected-search, cancellation, and
revalidation suite before receiving a production grant.

## Current executable slice

The provider contract, durable projection ledger, and DB2-side router now have executable Go
implementations. The public
`server-go/db3` package contains every version-one provider frame plus a provider runner. The
DB2-owned policy in `server-go/modules/db2` keeps an authenticated capability registry, accepts an
explicit compare-and-select route request, sends search to one selected principal, performs
pgvector fallback only when that route permits it, and revalidates every result. The external search
seam is implemented by a raw event-bus endpoint; it never calls provider code or opens a provider
connection.

This does not activate an external provider in production. There is still no provider descriptor or
grant, and the C DB2 process is still the activation oracle until its standalone closure and
real-row search adapters land. The cross-process conformance harness supplies strict test-only grants
to a C host and runs the DB2 router plus two independent Go providers over the real shared-memory
bus. Production activation remains ordered behind the C DB2 ownership cutover and database-effect
parity gates.

`src/modules/db2/db3_route.c` is the descriptor-owned C reference router for the first portable
operation, memory candidate search. It accepts explicit workspace, project, record type, generation,
bounded vector, and top-K fields; TLS scope hints do not cross the boundary. Injected callbacks bind
the internal pgvector implementation, the authenticated selected provider, and DB2's authoritative
candidate check without importing a provider or private KB header.

The route has four tested outcomes: pgvector when no ready external principal is deployed,
external-only serving through the deterministic deployed default or an explicit override, typed
failure when that provider is unavailable or malformed and fallback is disabled, and
`explicit_fallback` with the original external error retained when an override enables fallback.
Candidate IDs must be positive and unique, generations and request IDs must match, vectors and scores
must be finite, and every result passes the injected DB2 authorization check.

The catalog at `src/modules/db2/eventcontract/db3.json` generates the public C constants and Go
package identity. The shared `db3-wire-v1.json` fixture pins the byte-compatible v1 payloads while
the Go mutation suite pins canonical apply-v2 labels and malformed encodings. The nine event
payload shapes are:
capabilities, apply, apply chunk, applied acknowledgement, search request, search success, typed
search failure, route request, and route reply. A direct apply frame is retained when it fits one
inline slot. Larger committed operations use an ordered, bounded notification chunk envelope,
because the bus reserves `F_MORE` for request/reply streams. Reassembly is keyed by operation ID and
rejects gaps, overlaps, total-length changes, oversized bodies, and point-ID mismatch.

Capabilities never self-assert identity: principal, attachment handle, and sequence come from the
host-stamped bus frame. A new attachment handle may restart its sequence, while duplicate or stale
evidence from one handle is rejected. Ready search providers declare their generation, supported
operation, metric, and exact-filter bits, and dimension/top-K limits. Selection requires cosine
search and exact scope filtering.

Ready admission refreshes the deterministic deployed default unless
control has installed an override. Explicit route selection is a compare-and-select against the
observed generation, which remains bound until another successful select or clear; a later capability
generation makes the explicit selection unavailable rather than silently advancing it.

An apply-capable principal is durable after admission; an attachment loss does not discard its
delivery obligations. Admission installs durable per-projection cursors under the same PostgreSQL
transaction lock used by live vector triggers, then a Go worker advances at most 128 rows in each
short transaction. Live writes also target backfilling principals, so releasing the lock between
chunks cannot lose a mutation; a later snapshot duplicate carries the current row and remains
idempotent. It may advertise `ready=false` while applying that snapshot, but `ready=true` is rejected
from routing until every backfill delivery is acknowledged.

Retirement is an explicit durable control operation, not an inference from a lost heartbeat.
Removing an
automatic default advances to the next eligible principal; losing an explicitly selected provider
does not silently clear the route, so searches fail closed or take its explicit pgvector fallback.

`db3_projection` is the single reviewed catalog for relation, collection, vector-column, and exact
label mappings. The same rows install live triggers and drive resumable snapshot cursors, preventing
capture and backfill coverage from drifting apart. Multiple vector columns on one relational row
are independent collection consistency domains: their zero-padded trigger order is deterministic,
but no portable search joins those collections or assumes cross-collection atomic visibility.

`db3_provider`, `db3_backfill`, `db3_outbox`, and `db3_delivery` are separate from the existing
`vector_index_ops` pgvector retry table. AFTER ROW triggers cover each relation behind the 32
reviewed committed-mutation APIs, including row-wise effects of project/bulk deletes. The trigger
insert shares the pgvector transaction, so an outbox contract failure rolls the vector write back.
TRUNCATE is rejected because PostgreSQL exposes no transition rows from which to construct portable
point deletes. The Go dispatcher claims committed rows with `FOR UPDATE SKIP LOCKED`, broadcasts
them, records the publish timestamp, releases its lease, and schedules an unacknowledged operation
for replay after five seconds. Its per-principal snapshot worker retries transient driver,
connection, rollback, resource, failover, operator-intervention, and system failures with bounded
exponential backoff; cancellation stops the worker, terminal SQL errors stop retrying, and
`LastBackfillError` retains the most recent failure for operator diagnostics. Only an authenticated
per-principal `applied` frame completes a
delivery. Retryable/internal results remain pending, rejected results remain quarantined, and one
provider's acknowledgement cannot complete another provider's obligation.

The pre-activation rebuild procedure is intentionally schema-owner-only: retire every external
provider, remove the relation's DB3 capture/reject triggers in the maintenance transaction, perform
the destructive rebuild, reapply the schema to reinstall catalog-driven triggers, advance the
corpus generation, and re-admit providers for a full snapshot. Ordinary `TRUNCATE` remains rejected;
there is no application-level switch that can silently discard projection obligations.

The authenticated event-bus tests admit DB2 plus two distinct provider observers, deliver one
committed operation and its duplicate to both, prove each provider records only one effect, route a
fragmented search only to the selected server, reject source-principal substitution, filter an
unauthorized candidate, and make readiness loss produce observable explicit fallback. These are
executable pre-activation conformance seams; production outbox and pgvector callbacks connect only
after the C DB2 closure is standalone, so no second database owner or partial runtime grant is
introduced here.

## Exhaustive portability audit

`src/modules/db2/eventcontract/vector-portability.json` classifies all 76 current `pgvec_*`
declarations from the generated DB2 ledger. The five closed outcomes are:

- `portable-search`: 14 bounded candidate searches that can move behind DB3 once DB2 revalidation is
  wired; memory candidate search is the first vertical slice.
- `committed-mutation`: 32 upsert/delete surfaces whose logical effects can fan out only after the
  canonical DB2 transaction commits.
- `provider-control`: 15 collection, index, health, tuning, and telemetry operations that each
  provider implements locally and exposes only as bounded readiness/capability evidence.
- `db2-authority`: 12 schema, recovery, canonical lookup, verification, or process-local operations
  that remain inside DB2 even when an external provider is selected.
- `portable-analytics`: 3 path-returning or pair-analysis APIs deferred until a bounded opaque-ID
  contract lets DB2 revalidate their candidates.

The audit names the provider-specific C declarations for traceability, but those names never become
wire operations. CI compares the audit with the generated declaration ledger, rejects missing,
extra, duplicated, or reordered symbols, and pins the exact source-symbol fingerprint. Any new
pgvector declaration therefore requires an explicit reviewed disposition.

The update workflow is intentionally reviewed rather than automatic: regenerate
`declarations-v1.json`, run `python3 -I -S scripts/check_db3_portability.py
--print-source-fingerprint`, classify every added or renamed symbol in exactly one sorted group,
replace `source_symbols_sha256` with the printed digest, then run the checker and its mutation suite.
The checker continues to fail until both the classification and fingerprint agree with the ledger.

## Activation sequence and tests

The DB3 contract does not activate ahead of DB2. The existing C DB2 implementation first moves
behind its module boundary and passes build/link, caller, replay, tenant, schema, concurrency,
cancellation, durability, and pgvector parity gates. The tested DB3 C routing seam then connects to
the committed outbox, the selected search grant, internal pgvector, and authoritative DB2 hydration.
Only after the C process is the sole DB2 owner does the pure-Go DB2 provider reproduce the same
wire and database-effect fixtures. The final provider switch changes runtime selection, not callers
or contracts.

The runtime conformance suite exercises the existing C reference router, every generated/handwritten
Go codec, the Go selection policy under the race detector, the Go provider runner, notification
chunk reassembly, request/reply fragmentation and cancellation, and a real C host with two Go
providers. Activation additionally requires the same fixtures against real DB2 rows and the
committed outbox. C-versus-Go replay still runs the no-provider, selected-provider, unavailable, and
fallback cases before the C implementation can retire.
