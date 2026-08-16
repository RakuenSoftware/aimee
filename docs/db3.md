# db3 module contract

## Purpose and ownership

`db3` is the provider-neutral vector serving contract owned by DB2. Pgvector remains physically
inside DB2, shares DB2's PostgreSQL transactions and schema, and is the default DB3 behavior. An
external provider such as Qdrant can replace only portable serving operations after it is admitted,
ready, and explicitly selected. DB2 and its pgvector copy remain the canonical integrity and
recovery authority.

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
generation must be recognized without duplicating an effect. One failed or lagging observer never
changes which provider serves reads.

Portable reads are request/reply, never observer fanout. Exactly one admitted and ready external
principal may hold the selected search route. Installing several providers does not imply score
comparison, fusion, federation, or multiple reads. With no selected external provider, DB2 uses its
internal pgvector implementation. If a selected provider is unavailable, DB2 fails closed unless
the route explicitly enables fallback; fallback records the selected principal, failure, and
`explicit_fallback` provenance.

DB3 returns bounded opaque point IDs and finite scores. DB2 rehydrates every candidate and repeats
tenant, project, lifecycle, quarantine, and classification checks. Provider responses never confer
authority and never contain SQL, table names, connection data, provider query JSON, or source rows.

## Version-one provider frames

Every multibyte integer and float is little-endian. Each payload starts with its own magic, wire
version, and fixed header length; unknown flags, unknown closed-enum values, nonzero reserved bytes,
length mismatches, nonfinite numbers, zero identities, and values above the catalog limits are
malformed. Provider identity is never a payload field.

| Payload | Required semantics |
| --- | --- |
| `capabilities` | Generation; ready flag; search/apply operation bits; cosine/L2/dot metric bits; exact-filter bit; maximum dimension, batch, and top-K. Ready evidence requires a nonzero generation. Search requires a metric and nonzero dimension/top-K; apply requires a nonzero batch limit. |
| `apply` | Nonzero operation and generation, positive opaque point ID, upsert/delete/tombstone kind, bounded collection, and a finite vector only for upsert. The operation is emitted only after DB2's canonical transaction commits. |
| `apply_chunk` | Operation ID, total encoded apply length, exact offset, and a nonempty chunk. Offset zero restarts an incomplete retry; every subsequent chunk must be contiguous and retain the same total. |
| `applied` | Operation and generation, durable provider watermark, lag, and `ok`, `retryable`, `rejected`, or `internal`. An `ok` watermark cannot precede the acknowledged generation. |
| `search` request | Request ID, exact required generation, workspace/project scope, record type, bounded top-K, and finite vector. At least one scope is present. Request/reply fragmentation and cancellation use the bus directly. |
| `search` success | Matching request ID and exact generation, then at most requested top-K unique positive point IDs with finite scores. |
| `search` failure | Matching request ID and `invalid_request`, `unavailable`, `retryable`, or `internal`; provider prose and backend details never cross the boundary. |
| `route` request | `query`, `select`, or `clear`, a request ID, and, only for select, the authenticated provider principal, observed capability generation, and explicit-fallback bit. |
| `route` reply | Typed result (`ok`, `not_found`, `not_ready`, `generation_conflict`, or `invalid`) plus the current selected principal, provider generation, and fallback bit. A failed selection leaves the previous route intact. |

Version one is additive only. A new closed bit or operation needs a catalog update and conformance
fixtures; reusing a bit, changing an existing field, or weakening validation requires a new wire
version and a dual-version compatibility window. Provider implementations must pass the shared
codec, malformed-frame, duplicate-apply, multi-observer, selected-search, cancellation, and
revalidation suite before receiving a production grant.

## Current executable slice

The provider contract and DB2-side router now have an executable Go implementation. The public
`server-go/db3` package contains every version-one provider frame plus a provider runner. The
DB2-owned policy in `server-go/modules/db2` keeps an authenticated capability registry, accepts an
explicit compare-and-select route request, sends search to one selected principal, performs
pgvector fallback only when that route permits it, and revalidates every result. The external search
seam is implemented by a raw event-bus endpoint; it never calls provider code or opens a provider
connection.

This does not activate an external provider in production. There is still no provider descriptor or
grant, and the C DB2 process is still the activation oracle until its standalone closure and
outbox/real-row adapters land. The cross-process conformance harness supplies strict test-only grants
to a C host and runs the DB2 router plus two independent Go providers over the real shared-memory
bus. Production activation remains ordered behind the C DB2 ownership cutover and database-effect
parity gates.

`src/modules/db2/db3_route.c` is the descriptor-owned C reference router for the first portable
operation, memory candidate search. It accepts explicit workspace, project, record type, generation,
bounded vector, and top-K fields; TLS scope hints do not cross the boundary. Injected callbacks bind
the internal pgvector implementation, the authenticated selected provider, and DB2's authoritative
candidate check without importing a provider or private KB header.

The route has four tested outcomes: default pgvector when no external principal is selected,
external-only serving when the selected principal is ready, typed failure when it is unavailable or
malformed and fallback is disabled, and `explicit_fallback` with the original external error retained
when fallback is enabled. Candidate IDs must be positive and unique, generations and request IDs must
match, vectors and scores must be finite, and every result passes the injected DB2 authorization check.

The catalog at `src/modules/db2/eventcontract/db3.json` generates the public C constants and Go
package identity. The shared `db3-wire-v1.json` fixture now pins all nine payload shapes:
capabilities, apply, apply chunk, applied acknowledgement, search request, search success, typed
search failure, route request, and route reply. A direct apply frame is retained when it fits one
inline slot. Larger committed operations use an ordered, bounded notification chunk envelope,
because the bus reserves `F_MORE` for request/reply streams. Reassembly is keyed by operation ID and
rejects gaps, overlaps, total-length changes, oversized bodies, and point-ID mismatch.

Capabilities never self-assert identity: principal, attachment handle, and sequence come from the
host-stamped bus frame. A new attachment handle may restart its sequence, while duplicate or stale
evidence from one handle is rejected. Ready search providers declare their generation, supported
operation, metric, and exact-filter bits, and dimension/top-K limits. Selection requires cosine
search and exact scope filtering. Route selection is a compare-and-select against that observed
generation, which remains bound until another successful select or clear; a later capability
generation makes the old selection unavailable rather than silently advancing it. Removing or
losing a selected provider does not silently clear the route; searches fail closed or take the
route's explicit pgvector fallback.

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
