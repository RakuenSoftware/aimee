# Shared lifecycle retry receipts — 2026-09-23

This PR #2990 slice extends the [conditional lifecycle
operations](memory-lifecycle-versions-2026-09-23.md) with durable retries. It is
partial MR02 evidence, not certification of MR01–MR18 or the frozen 123 clauses.

## Contract and implementation

Authenticated shared reject/restore accept a 16–128 character printable ASCII
`idempotency_key` with `expected_version`. The existing owner/caller/key namespace
and immutable receipt table hold schema-two outcome receipts. Their digest binds
the verb, effective authority, scope, observed version and rejection reason;
existing correction digest formats are unchanged. Restore derives its actor from
authenticated context and retains its existing tombstone admission policy.

A transaction advisory lock orders same-key calls; the canonical row lock orders
different keys and competing mutations. The canonical row, rejection tombstone,
audit seal, invalidation and receipt commit together. Any failure rolls the request
back. A fresh-key rejection of an already rejected matching record binds an
explicit no-op audit to the existing revision without rewriting the tombstone.

A retry confirms the stored result's scoped row, revision and lifecycle/tombstone
state. It returns the original commit with `replayed: true` and performs no
canonical mutation or audit/invalidation write. Reusing a key for a different
admitted request returns `idempotency_conflict`; an unavailable result returns
`idempotent_result_unavailable`. A receipt cannot reapply rejection after restore
or restore a later revision.

Shared schema 32 and its matching C schema constant add a receipt guard against
fabricated lifecycle outcomes. The existing operation constraint is widened in
place so schema reapplication accepts committed lifecycle receipts.

## Validation

The isolated PostgreSQL instance used the published 0.4.5 PostgreSQL image and
the shipping schema, independently of production data and volumes.

- Full Go memory package suite passed (57.4 seconds).
- The final restricted-role and concurrent retry cases passed with the Go race
  detector (95.1 seconds).
- Native server-memory and KB HTTP route suites passed with the rebuilt Go
  runtime fixture.
- Restricted-runtime-role replay covers late receipt failure and atomic rollback,
  same-key replay without duplicate audit/invalidation, changed reason and scope,
  caller isolation, no-op rejection, later lifecycle changes and forged receipts.
- Real simultaneous requests wait on the key lock. Both committed response loss
  and uncommitted writer disconnect are exercised for reject and restore, with a
  fresh connection confirming the durable result.
- Shipping schema 31 → 32 → 32 preserves an existing owner, canonical record and
  correction receipt. The runtime fixture also reapplies the receipt migration
  twice with earlier receipt types and lifecycle receipts present.
- Ownership, C boundary and module-bus boundary checks pass. The C bus stays C;
  receipt and lifecycle decisions remain in the Go memory owner.

Test credentials are private local artifacts and are not committed. The tests
prove database-backed reconnect behavior, not a newly deployed PR binary or a
whole-machine crash campaign.

## Released 0.4.5 validation

CT100 on 192.168.1.253 remains pinned to the published 0.4.5 application,
PostgreSQL and embedder images. Repeated checks through the paired local thin
client report `state: ok` and healthy capture; all three production containers
remain healthy. A fresh thin-client private-memory write/read/delete/not-found
smoke also passed; its temporary record was retired. Upgrade rollback material and the initial restart/write smoke
are recorded in the preceding lifecycle validation document. This slice changes
the draft PR source; it does not replace the released production application.
