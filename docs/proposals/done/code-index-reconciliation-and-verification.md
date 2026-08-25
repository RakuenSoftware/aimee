# Code-index reconciliation and verification

Status: done

## Problem and boundary

The canonical code index is good at adding and replacing facts, but an index is
trustworthy only when it can also prove what it owns, retract facts whose source
has disappeared, and distinguish a durable snapshot from the live workspace.
Batch delivery makes this harder: a missing final batch, a retry, or an older
scan finishing late must never turn a partial manifest into authoritative
absence.

This proposal covers the canonical source-file index, its scan protocol, the
operator verification surface, and the adapter tests that keep repository
automation deterministic. It does not change memory retrieval, branch-overlay
semantics, or the meaning of a project's lifecycle generation.

## Decision

### 1. Reconcile from a sealed complete manifest

A scan is a level-triggered statement of the files currently owned by one
project generation. Local scans build that complete manifest while walking the
workspace. Remote scans use a `begin` / `stage` / `seal` session:

- `begin` records the project generation and current index revision;
- `stage` records source payloads and manifest membership idempotently in the
  private session, without changing query-visible facts;
- `seal` is accepted only for the same project generation and baseline
  revision, and only when the distinct staged-file count matches the sender's
  expected count;
- a successful seal atomically publishes changed facts, retracts indexed files
  absent from the manifest, and advances a separate monotonic index revision;
- an interrupted or stale session leaves the prior durable snapshot visible; it
  cannot publish partial changes, publish absence, or roll the revision backwards.

The existing lifecycle generation continues to mean detach/re-add identity.
Index revision is a different ABA guard and must not be inferred from timestamps.
Duplicate and out-of-order stage requests are harmless because manifest
membership is keyed by `(scan_id, path)`.

### 2. Add a read-only verification command

`aimee index verify <project> <root> [--deep]` reports drift and never repairs
it. Shallow verification compares the indexed manifest with the current
indexable path set. Deep verification additionally hashes source bytes and
compares them with stored content hashes. Repair remains the explicit
`aimee index scan` operation.

Verification has bounded output: summary counts plus a capped list of example
paths. It returns a non-zero command status for detected drift or an unavailable
workspace, making it useful in CI without making reads mutate state.

### 3. Separate durable-index and live-workspace state

Results that discuss freshness use two independent concepts:

- `index_state`: whether the row belongs to the current durable project
  generation, accompanied by `index_revision` where available;
- `workspace_state`: `matched`, `modified`, `missing`, or `unavailable`;
- `verification`: `manifest`, `content_hash`, or `none`.

`current` must never imply that the backing file still exists. A service that
cannot see a client's filesystem reports `workspace_state=unavailable` instead
of guessing.

### 4. Pin reconciliation invariants with failure-oriented tests

Tests must cover deletion, same-size/same-mtime content changes, duplicate and
out-of-order batches, an interrupted scan without a seal, stale seals after a
newer scan, and deterministic ordering of reported drift. These are protocol
invariants rather than examples of one happy-path implementation.

### 5. Treat repository adapters as a compatibility matrix

The hook/config installer is tested as a matrix of the harnesses it claims to
support. Golden assertions preserve unrelated user configuration, require the
managed entry exactly once, and prove a second installation is byte-for-byte
idempotent. A new adapter is not considered supported until it appears in that
matrix.

## Non-goals

- Verification does not automatically scan, delete, or repair.
- A partial remote upload is not a complete snapshot.
- Basename similarity does not transfer ownership between paths or projects.
- Index revision does not replace lifecycle generation or branch-local overlays.
- The initial protocol does not retain historical source bodies as queryable
  snapshots; it protects publication ordering and exact current ownership.
- Derived optional projections that already converge asynchronously remain
  asynchronous.

## Owners and dependencies

The canonical-index module owns revisions, sessions, manifest membership,
retraction, and verification. The knowledge-service HTTP layer owns protocol
validation. The thin client owns complete-manifest enumeration and session
sequencing. Repository configuration owns adapter installation; tests own the
declared compatibility matrix.

The implementation depends on existing project generations, cascading file
foreign keys, source-content hashes, path exclusion rules, and the purge fence.

## Failure model

- A process failure before seal leaves no authoritative deletion.
- A retry may repeat any stage batch.
- A stale seal is rejected when either generation or baseline revision changed.
- A count mismatch rejects the seal and preserves the previous ownership set.
- Purge fences are checked at the retraction commit point.
- Files that disappear while a local scan is reading are omitted only if the
  scan still reaches a valid seal; hard read/index failures abort reconciliation.
- Verification reports `unavailable` for an inaccessible root and does not
  reinterpret that as every indexed file being missing.

## Compatibility and migration

The schema migration is additive. Existing projects receive revision zero on
first use. The legacy one-request `files` payload remains an incremental write
for compatibility and is never treated as a complete manifest. New thin clients
use phased requests; old clients therefore remain conservative rather than
accidentally deleting data. Session rows are operational metadata and become
eligible for garbage collection after publication or explicit abort.

Human-readable output gains explicit state fields while existing JSON fields are
preserved. Adapter installation continues to merge, rather than replace, user
configuration.

## Bounded implementation slices

1. Add revision/session/manifest schema and canonical-index APIs.
2. Reconcile local scans only after a complete successful traversal.
3. Add phased remote begin/stage/seal while retaining the legacy incremental
   payload.
4. Add shallow/deep verification and explicit state output.
5. Add reconciliation invariant tests and the adapter compatibility matrix.

## Checks

- DB2 schema parity and generated-schema checks pass.
- Canonical-index tests prove retraction, interruption safety, ABA rejection,
  retry idempotency, and deep hash drift detection.
- Client and HTTP route tests pin phased request/response compatibility.
- Adapter golden tests pass twice against pre-populated configuration.
- Relevant production and thin-client targets compile.
