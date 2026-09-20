# Personal memory invalidation producer

PR: [#2990](https://github.com/RakuenSoftware/aimee/pull/2990).

Personal memory changes now commit a content-free invalidation event, record
revision and collection generation in the same transaction as the canonical
write. Generation locking orders committed events; rollback restores all three.
The bounded Go feed requires a canonical snapshot after bootstrap, an owner
change, a rewound database or a retention gap. Consumers cannot treat a feed
cursor as proof that they applied the event.

Revision assignment runs before writes and event capture runs after writes.
This matters for `INSERT ... ON CONFLICT`: PostgreSQL invokes `BEFORE INSERT`
for candidates discarded by conflict handling. The regression failed before
the fix because an identical retry advanced the generation from 5 to 7 instead
of 6. With the fix, an identical retry preserves identity and progress; changed
content emits one update for the existing identity. Counter-only reads bypass
both triggers, avoiding content serialization and generation locking.

## Local validation

- Full memory and Aimee-family race suites passed. Memory PostgreSQL fixtures
  were enabled against PostgreSQL 17 with pgvector.
- The shipping migration ran in an isolated schema with a non-owner runtime
  role. Tests cover immutable identity, revision forgery, protected journal
  privileges, temporary-table shadowing, rollback, outbox failure, producer
  restart, concurrent writer blocking and commit order, bounded replay and
  retention gaps. The canonical Go `Put` path covers identical and changed
  upserts. Catalog checks cover both triggers' governed-column lists.
- `CGO_ENABLED=0 go build ./modules/memory`, schema synchronization, source
  registration and memory ownership checks passed.

## Fresh shipping-image validation

Built `Dockerfile.server` with `WITH_VSCODE=0` from commit
`e967113a21b785daa5e142f1dc7ea94af84d30f5` in owned CT 9498 on `.253`.
Application image:
`sha256:46f0ee6b33ed63033d137cf6ebb8405307e241d779b1da2b9239299affcdc56d`.
The same revision supplied the test harness. The runner created fresh identities,
storage and a Compose project, using PostgreSQL image `aimee-postgres:pr2983`
and real Bekko-a25m embedder
`sha256:b03199bee881bf632f7194f472de7bc370e66d16b7215bb6aa506fb2b1510209`.

All **102 verdicts passed**: 61 personal placement, 15 semantic recall,
21 exploratory and 5 topology checks. HTTP store and retirement commit their
events; an identical HTTP upsert preserves identity and progress. HTTP/CLI/MCP
reads leave the journal unchanged. Injected outbox failure rolls back the HTTP
mutation. Application and PostgreSQL restarts preserve the owner and journal.
Exploration covers concurrent operations, exact large IDs and supervised memory
owner recovery. After the deliberate 60-second owner interruption, the next
successful recovery check completed in 2.23 seconds.

[Content-free verdict receipt](memory-personal-invalidation-2026-09-20/fresh-t3.json).
The owned project was retained for diagnosis; its containers were stopped after
validation. These checks establish correctness, not a comparative latency result.

## Scope

This is the personal producer foundation for MR-02. Personal content versioning,
shared-KB generations, expected-version/idempotency admission, durable consumer
application/checkpoints and final release checks remain pending. No latency
improvement is claimed. Memory remains Go; the C bus is unchanged.
