# Shared conditional deletion and durable retry identity

The Go shared-memory owner now accepts `expected_version` on delete/forget and
an authenticated `idempotency_key` with that precondition. Existing behavior is
preserved: model authority retires eligible model-authored records; independently
verified user authority may physically delete through the existing deletion
contract. Body claims do not grant that authority. Model retirement continues to
respect episode/experience immutability and instruction/policy revocation rules.
This does not complete the broader configured retention/erasure policy.

The existing actor/owner/key receipt namespace serializes duplicates. Admission
locks the canonical row and checks scope, revision and authority before opening
an audit commit. The mutation, WORM changeset, invalidation and receipt commit
together. Destructive changesets are explicitly irreversible. A late receipt
failure rolls back the entire operation; an uncommitted disconnect leaves the
key available, while a committed retry returns the original commit.

Shared deletion receipts use schema version 2 and bind `target_version` plus an
`outcome` of `retired` or `destroyed`. Retirement includes its resulting `version`.
Destruction has no current canonical version, so that field is omitted. Existing
schema-one correction and private-retirement receipts retain their format and
hashes. No receipt caches memory content or certifies downstream consumers.

Schema 27 extends the existing shared receipt table; existing rows retain the
`correction` discriminator. A durable guard requires the exact target/result
revision and the canonical audit outcome. Go seals the changeset before inserting
the receipt. Replay checks the receipt's
current owner and scope and the actual canonical outcome. Its bounded privileged
verifier accepts only the caller's own key and returns a boolean: a restored ID
hidden by RLS cannot be mistaken for successful destruction. Changed requests
conflict; reactivation, scope/revision changes or erasure cannot cause another
retirement. Owner rotation and complete restore/retention semantics remain open.

Unkeyed calls retain their original path. Conditional calls add one locked
admission read; keyed calls add receipt lookup and verification. Lookup uses the
existing receipt primary key and canonical record ID. No latency improvement or
whole-request P95 claim is made here.

Local validation passes:

- Required PostgreSQL memory suite under the runtime role, including receipt
  failure rollback, stale/hidden targets, cross-verb reuse, authority refusal,
  exact schema-two outcomes, forged-receipt rejection, unkeyed preconditions,
  MCP replay audit suppression and hidden-ID restoration.
- Actual concurrent connections for both authority modes, committed response
  loss, uncommitted disconnection and replay after reconnect; targeted race tests.
- Full schema 26 → 27 → 27 upgrade on a disposable database using the previous
  revision's real schema, preserving every old correction-receipt field and the
  canonical record. Verifier ACLs remain restricted after reapplication.
  [Upgrade source identities and results](memory-shared-deletion-2026-09-21/schema-upgrade.json).
- Native build and memory-route tests, independent Go memory export/build, all
  77 repository lint checks and all 17 S1 contract checks.

The live fixture adds real HTTP/MCP deletion and retirement, failure injection,
KB restart, reactivation/erasure and hidden-ID restoration. Fresh-image T2/T3
acceptance is pending. Creation retries, remaining mutation preconditions,
consumer/checkpoint/retention behavior and the full MR-02 acceptance inventory
remain open. The C bus is unchanged; schema bootstrap remains with the existing
storage owner. Whole-DB2 retirement stays deferred.
