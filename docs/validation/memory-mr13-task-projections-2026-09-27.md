# MR-13 disposable task projections: functional closeout

MR-13 explicit task projections are functionally complete at `9c1242b1d`, image
`sha256:6a04c8e1eb838d0efe3be17297d2132b93775fcddc5907e997ef0fb53bc6b7e5`.
Both CT109 owners are healthy. The final live harness passed **31 checks** and
cleaned its exact synthetic fixtures. Production CT100 remains healthy on 0.4.5.

## Delivered behavior

The existing DB1 session-state owner holds a bounded, disposable projection for
its authenticated principal and active task. Expected-revision updates are atomic;
concurrent changes conflict. Task switching creates a new identity. Fresh Go
served-memory evidence and recorded execution events are checked on each release.
Changed dependencies withhold stale text; missing parents or changed execution
receipts block release. Hypotheses, planned actions and execution observations
retain distinct labels and do not become independent corroboration.

CLI, HTTP and MCP use one host bridge. Caller-supplied prepared evidence and
admission results are dropped, and plugins cannot invoke the host-only task-state
operation. SQL constraints reject authoritative task-state classes. The existing
runtime database role is unchanged; this does not claim a new least-privilege
role. Canonical mutations remain behind existing admission and review guards.

Zero budget omits the entire coherent rendering. Working text appears only in
that rendering, not again in response metadata. Admission metadata retains IDs
and digests, without copied draft text. Receipts bind exact rendered bytes,
revision, sources and replay availability; they are explicit-access receipts,
not claims of provider dispatch.

Promotion supports private corrections to an existing evidence target. Preview
binds the exact revision, selected claim, source versions, scope and reviewer.
Submission and human approval check current source versions. Repeated submission
reuses the same immutable correction proposal. Approved hypotheses retain their
label and partial lineage with unknown independent support. Shared promotion and
new canonical targets explicitly report unavailable. Automatic preload is off.

Expiry/discard clears derived text and records digest-only replay availability.
An ambiguous admission retains a digest for reconciliation without pinning task
text indefinitely. Input memories, execution receipts and admitted canonical
history remain governed by their existing owners.

## Validation and upgrade

- Full memory PostgreSQL race suite: 412.198 seconds; exported module build passed.
- Full isolated DB1 family race suite: 2.433 seconds. Final task-owner/envelope
  regression: 1.195 seconds; clock/promotion delta: 1.214 seconds.
- Native transport boundary test passed and is registered in the normal test run.
- All 77 lint checks ran. Failing integration checks were fixed and rerun; generated
  documents, test registration, module descriptors and validation records passed.
- Final live checks cover exact accounting, no metadata budget bypass, CAS races,
  CLI/MCP parity, stale previews, draft deduplication, explicit review, preserved
  hypothesis lineage, governed retirement, source/event invalidation, expiry,
  discard, task switching, foreign-principal denial and restart recovery.

Migration 41 adds bounded task state without rewriting prior migrations. Both
owners were quiesced and their exact stores snapshotted before migration. The
upgrade helper restores those stores before restarting old images on failure;
image-only rollback across schema versions is insufficient. Private dumps remain
on CT109; only hashes are in the evidence. Restoration was prepared, not fault-
injected during this acceptance run. Optional ranking/horizon policies stay off.

The first candidate failed because the real SQL process adapter cannot scan
`time.Time`, unlike direct PostgreSQL fixtures. Both clock reads now use integer
microseconds. Live probes also prompted removal of duplicated metadata text and
a valid error envelope for task mismatch. Fixture errors included a wrong review
route spelling and raw SQL correctly refused by the reviewed-record guard; the
final harness uses the governed delete/review path. An intermediate build ran
out of disk; inspected build caches and recreatable source snapshots were cleaned
before rebuilding. These failed/probe runs are not counted as final acceptance.

[Evidence manifest](memory-mr13-evidence-2026-09-27/manifest.json) binds the retained
checks, test logs, image/snapshot identities and reproducible acceptance helpers.
