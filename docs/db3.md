# db3 module contract

## Purpose and ownership

`db3` is the provider-neutral vector serving contract owned by DB2. Pgvector remains physically
inside DB2, shares DB2's PostgreSQL transactions and schema, and is the default DB3 behavior. An
external provider such as Qdrant can replace only portable serving operations after it is admitted,
ready, and explicitly selected. DB2 and its pgvector copy remain the canonical integrity and
recovery authority.

The logical contract owner will receive the next canonical principal when its executable router and
first served stage land. The current process-contract schema deliberately forbids reserving a
principal without a real process and stage, so this design audit does not create a fake deployment.
Each deployed provider has a distinct strict principal, executable identity, and runtime grant. No
provider descriptor or production grant exists until that provider ships.

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

## Current executable slice

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

The same source owns bounded version-one codecs for `db3.apply` and `db3.search`. A real authenticated
event-bus test admits DB2 plus two distinct provider observers, delivers one committed operation and
its duplicate to both, proves each provider records only one effect, routes a search only to the
selected server, and rejects a second serving principal. These are executable pre-activation
conformance seams; production outbox and pgvector callbacks connect only after the C DB2 closure is
standalone, so no second database owner or partial runtime grant is introduced here.

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

The current C conformance tests use two authenticated provider principals and prove that writes reach
both observers, duplicate operations are idempotent, one search reaches only the selected server, a
second server cannot own the route, malformed vectors and scores fail closed, unauthorized candidates
are removed, and explicit fallback is observable. Activation additionally requires the same fixtures
against real DB2 rows and the committed outbox. C-versus-Go replay runs the no-provider,
selected-provider, unavailable, and fallback cases before the C implementation can retire.
