# MR-02: Authority-preserving memory mutations

- **State:** In progress; initial KB admission/versioning slice implemented
- **Priority:** P0: durable correctness
- **Owner:** Go memory mutation admission, with PostgreSQL durable guards
- **Depends on:** [MR-01](memory-reliability-01-unified-eligibility-and-validity.md) for shared authorization vocabulary; admission fixes can begin immediately
- **Delivery:** Three implementation slices

## Problem and intended result

The original same-key insert could overwrite active content in place, while edits
used a different history path. KB store/upsert, edit, supersede and legacy content
adapters now share Go admission and create versions for changed content. User
corrections preserve history; model writes cannot displace user or unknown-origin
content. Immutable kinds are protected on conflict, exact same-author retries
preserve identity and captured authorship, and concurrent same-key creators are
serialized. PostgreSQL tests cover rollback, protected writer/verb combinations,
private metadata preservation and an actual blocked concurrent writer.

This is a foundation slice. Review-required writes currently refuse without
creating a linked proposal. Personal versioning, explicit expected versions,
idempotency keys, durable guards/outbox/consumer replay and full retention policy
remain acceptance work.

Implement one mutation admission operation for create, propose, correct, supersede, reject, retire and explicitly authorized destructive deletion. Route compatibility entry points through it.

## Existing integration points

`server-go/modules/memory/mutations.go` contains `InsertEpistemic`, `UpdateAs` and `DeleteAs`. Preserve existing episode/experience immutability, instruction/policy replacement rules, rejection tombstones and fact changesets. Extend the current schema and audit mechanism instead of adding a parallel write service.

## Mutation contract

The authenticated host supplies `actor_ref`, `actor_class`, `operation`, `target_id`, `expected_version`, `content_digest`, `source_event_ref`, `scope` and an idempotency key. Authority does not come from text claiming to be a user correction.

Separate `origin_authority`, `revision_authority` and `review_authority`. A model may draft a proposal derived from a user statement; the resulting draft is still model-authored. User approval is a separate event that identifies the reviewed content digest. Copying source attribution must not copy author identity or an old confidence ceiling onto new prose.

| Attempt | Required result |
|---|---|
| Model corrects an authoritative user assertion | Preserve current assertion; create linked proposal unless explicit governing policy permits another transition |
| Model edits its own active hypothesis | Create a new version and supersession link; retain old version |
| Same-key insert conflicts with an immutable episode | Reject or record a distinct new episode; never overwrite |
| Same-key insert conflicts with instruction/policy | Use the reviewed replacement path |
| Authorized user correction | Create a new version with user correction provenance and explicit effective time |
| Replay of rejected material | Enforce applicable tombstone before activation |
| Retry with identical idempotency key and different bytes | Reject with `idempotency_conflict` |

Tombstone matching must have a documented canonical identity and scope. Exact-text tombstones and semantic rejection policies are different controls; do not claim exact hashing detects paraphrased reintroduction.

## Transactions and history

Resolve conflict identity under a transaction, compare the expected version, admit the transition, write the new version, append audit intent and invalidate derived dependants atomically where they share the durable owner. For other owners, commit an invalidation outbox entry in that same transaction, or use an equivalent durable change log with the mutation's commit position. Publication after commit alone is insufficient. Never delete the old row first and hope the replacement succeeds.

The producer retries delivery after restart until consumers have durably applied the event. Bind event identity to the source owner, record/version and change position. Consumers atomically apply invalidation and advance their replay position; duplicates and older events cannot restore freshness. Retain events until required consumers have acknowledged them, or require a full resynchronization after a retention gap. The mutation response confirms canonical commit; it does not claim that every derivative has caught up.

Advance the affected scoped collection generation in the mutation transaction, including inserts that have no existing dependants. This supplies [MR-12](memory-reliability-12-served-memory-views-and-claim-cards.md)'s query-cache invalidation contract; it does not require that view implementation to exist first.

A lagging consumer must obtain a current owner check before releasing affected derived content, or return unavailable. A stale local generation cannot certify its own freshness. [MR-04](memory-reliability-04-evidence-lineage-and-independent-support.md) applies this delivery contract to lineage, caches and erasure coverage.

User authority alone does not bypass the configured deletion/retention policy. A destructive operation must be explicit and auditable. Correction should be the normal path when historical evidence remains useful.

## Implementation slices

1. Add transition rules and concurrency/idempotency tests; make all same-key conflict handling call the same admission function.
2. Version model/user edits consistently and add new-author provenance plus confidence recomputation. Backfill only facts supported by existing records; legacy authorship remains `unknown` where necessary.
3. Add durable guards for critical invariants, transactional invalidation publication, consumer replay and compatibility-adapter parity. Expose conflict/review-required results to callers without silently retrying as a more privileged operation.

## Acceptance gates

- User assertion → model upsert and user assertion → model edit cannot silently become active model-authored truth bearing user provenance.
- Episode and policy protections hold for insert conflicts, updates, bulk import and maintenance writers.
- Concurrent corrections yield one admitted expected-version transition; the losing caller receives an actionable conflict.
- A crash cannot leave the old version retired with no valid replacement or duplicate a correction on retry.
- A crash after mutation commit but before publication still delivers the invalidation after restart. Duplicate or reordered delivery cannot mark an obsolete derivative fresh.
- Consumer restart and retention gaps preserve or rebuild replay progress; a lagging consumer cannot release revoked content using its old local generation.
- Rejected content, changed idempotency payloads and forged actor metadata fail consistently.
- Audit records identify previous/new versions, author/reviewer, effective time and the exact approved content.

## Rollout and rollback

Add schema fields before switching writers. Detect legacy same-key conflicts and make the migration explicit rather than silently choosing a winner. Keep a compatibility reader for old versions. Rollback must retain created history and guards; it cannot re-enable destructive model upserts.

[Program and common contracts](memory-reliability-00-program.md) · [Requirements coverage](memory-reliability-requirements-coverage.md)
