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

Authenticated HTTP supersede now forwards the host's user authority, matching
store, and the Go owner independently verifies it before creating a correction
and capturing its author. Previously that route always requested model authority
and refused corrections to a user's own stored facts. Model calls and caller
supplied authority text still cannot replace authoritative content. A review
refusal maps to HTTP 409 rather than an upstream-failure status.

Shared model corrections now create linked review proposals and support exact-draft
approval or rejection as described below. Personal versioning, expected versions on remaining mutation verbs,
idempotency on remaining verbs, further durable guards/consumer replay and full retention policy
remain acceptance work.

The personal-store producer now captures governed row mutations in a durable,
content-free invalidation outbox with record revisions and a collection generation.
Generation reservation and publication are in the mutation transaction, in
commit order; ordinary read counters bypass capture. The Go host-only bounded
feed pins head/page to one SQL snapshot and requires canonical resynchronization
on bootstrap, owner change, a cursor beyond the head or retention gap. A non-owner fixture exercises
actual concurrent writers, rollback, outbox failure, replay and forbidden progress
mutation. Shared records and secondary scope tags now have a primary-scope
producer too: collection-bound cursors, old/new scope invalidation, independent
collection commit ordering and parent-visible tag access. Restricted-role replay
covers version replacement and rollback after a failed tag copy. This does not
complete personal content versioning, further governed child/dependency coverage,
consumer application/checkpoints or the release-freshness contract.

Implement one mutation admission operation for create, propose, correct, supersede, reject, retire and explicitly authorized destructive deletion. Route compatibility entry points through it.

### Expected-version shared corrections

Shared exact-ID `get` accepts `include_version: true`. Its `memory.version` object
contains `schema_version: 1`, the owner UUID, and decimal-string `record_id` and
`record_revision`, captured with the content in one SQL snapshot. Pass that object
as `expected_version` to shared `update` or `supersede`. The existing row lock protects the
comparison and replacement; stale revisions, changed owners and already replaced
rows return `conflict` with reason `expected_version_conflict`. Hidden and missing
rows retain `not_found`; knowing a version grants no authority. Ordinary callers
keep their existing query path. No additional database round trip is needed for
versioned reads or the locked comparison.

Generated search columns are excluded from both revision-trigger target columns
and BEFORE-row comparisons. PostgreSQL can otherwise fire those triggers on
counter updates, and the generated values are not available in NEW yet. The
regression uses generated columns plus a second BEFORE trigger, as in the shipping
schema; counter-only reads and unchanged governed writes retain their revision.

This opt-in contract currently supports shared exact-ID get, update and supersede.
Other operations and personal placement refuse the fields explicitly. It does
not supply automatic restore-owner rotation, historical
belief reconstruction, or final-release freshness proofs.

### Durable retries for shared corrections

Authenticated shared `update` and `supersede` accept an optional `idempotency_key` (16–128
printable ASCII characters, no spaces) together with `expected_version`. Reuse the
same key for a retry of the same owner-admitted request. Its digest includes the
content, confidence, target, expected version, effective authority, session and
scope; it excludes view formatting, trace IDs and connection identity. The host
may already have screened the incoming payload, so this does not claim a digest
of original HTTP bytes before host transformations.

A transaction-scoped key lock orders concurrent retries. The immutable,
actor-isolated receipt stores hashes and result references alongside the existing
`fact_graph_commits` audit. Replacement, copied scope tags, extraction actor/job,
WORM sealing, invalidation and receipt insertion commit together. Failure even at
the final receipt insert rolls all of them back. A disconnected uncommitted writer
leaves the key reusable; a committed writer needs no in-process retry state.
Ordinary requests do not pay for receipt lookup or key locking.

Success includes `mutation_receipt` with schema version, canonical commit ID,
exact decimal-string result version and `replayed`. Replays re-read current
eligibility and revision, without recapturing authorship, queuing extraction,
creating another canonical audit commit or publishing invalidation. A different
admitted payload returns HTTP 409 / `idempotency_conflict`. An erased, hidden,
retired, expired or revised result returns HTTP 409 /
`idempotent_result_unavailable`, without cached content or repeating the write.
The receipt is a canonical commit reference, not proof that derivatives caught up.

Both correction verbs use the same locked admission and retry mechanism. Update
inherits confidence from that row read, removing its previous separate confidence
lookup/lock. The digest binds the verb, so one key cannot switch between update
and supersede; existing schema-one supersede digests remain compatible.

The owner locks and admits the correction before opening its canonical audit
commit, then applies the admitted change under that same transaction and row
lock. A review or version refusal does not depend on the canonical audit writer
being available. Proposal creation is a distinct audited outcome; it does not open a canonical
correction commit or change the original serving revision.

MCP `memory_get` advertises the version field, and `mutate` forwards and advertises
the precondition and retry key for shared update/supersede. Keyed responses retain
the complete owner receipt instead of dropping it into a plain success string.
The Go owner omits the host audit request on replay, preventing duplicate host
mutation events. Legacy unkeyed MCP responses retain their existing text format.

This contract currently covers shared corrections only. Other verbs and personal
placement explicitly refuse the field. Remaining create/delete idempotency,
personal content versioning, retention/restore policy and consumer progress remain open.

### Linked model correction proposals

Schema 25 adds `memory_correction_proposals` outside the serving memory tables.
Model edits, supersede calls, same-key upserts and legacy content edits that need
review retain the original and return `review_required` with a `proposal`
reference. The reference binds the storage owner, target ID/revision and SHA-256
digest of the screened content and requested metadata. The draft is model-authored;
confidence is recomputed under the model ceiling (0.8, or 0.5 for L5). Background
convention, cognification and trace writers retain proposal outcomes without
extracting or counting those drafts as active facts.

The existing KB action API exposes `memory.correction_proposals` and
`memory.review_correction`. Lists return bounded references, without fetching
payload text. Supplying `proposal_id` inspects one draft. All reads use parent
visibility and the current owner identity; drafts never participate in recall.
Creation and retry replies also avoid loading the stored payload. Indexed exact
lookup, a recent-order index and a parent index support review and erasure.

A review requires verified authenticated user authority, `proposal_id`,
`payload_digest`, the proposal's `expected_version`, and `action` (`approve` or
`reject`). Body claims cannot grant user authority. A scoped service bearer
alone on direct KB HTTP does not supply human authority. The existing mTLS
host-caller transport separately verifies the enrolled Server certificate,
rotating bearer, service identity and caller assertion before Go sees the actor.
Approval locks the parent before the proposal, compares the current target
version and creates a new canonical version through the same Go writer.
It preserves model provenance
(`reviewed_model`), confidence ceilings and the model extraction actor. The
reviewer and exact draft digest are recorded separately in
`knowledge_review_decisions`, with the existing changeset/WORM audit. Requested
tier, use cases and mutable epistemic-kind changes are part of the reviewed
digest; episode/experience and instruction/policy transitions retain their
existing protection. If current screening would alter the reviewed text,
approval refuses instead of silently substituting another payload.

Proposal payloads and terminal decisions are immutable. Repeating the same
owner/target/revision/draft returns the same proposal, including across proposers
and after rejection; it cannot reopen a rejected draft. A changed target blocks
approval, while a still-visible stale draft can be explicitly rejected. Repeating
an approval returns the existing commit only while its result remains visible,
eligible and at the committed revision. Competing approvals create one successor;
a competing rejection cannot undo an already committed approval.

Keyed proposals use the existing actor-isolated mutation receipt namespace. The
receipt contains a proposal reference instead of canonical result content. Its
reserved zero result revision cannot match a canonical record, so older receipt
readers refuse it instead of falsely reporting a completed correction. A
changed request or authority under the same key conflicts. A proposal response
remains `review_required`; its retry reference does not imply an approved edit.
Parent erasure cascades to draft payloads, while content-free audit and retry
references prevent erased proposals from being recreated through old keys.
Late failures roll back the whole proposal or approval transaction, including
canonical versions, extraction work, decisions and invalidation. This workflow
is a shared-memory foundation, not completion of every MR-02 acceptance gate.

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
