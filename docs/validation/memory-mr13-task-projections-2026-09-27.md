# MR-13 task projection candidate

The first candidate upgraded successfully but failed live task access because
the process SQL adapter does not support scanning into `time.Time`. Both task
expiry and promotion admission now read database-clock integer microseconds.
Direct PostgreSQL fixture success was insufficient to catch this boundary;
corrected live acceptance remains pending.

The existing DB1 session-state owner holds one bounded, disposable projection
for its authenticated principal and active task. Atomic expected-revision updates
conflict rather than overwrite concurrent changes. Task switches require a new
identity. Fresh Go served-memory evidence and recorded execution events are
rechecked on every release; stale or unauthorized inputs withhold the projection.
Working hypotheses, plans and observed execution results retain distinct labels.

Explicit CLI, HTTP and MCP access share the same host bridge. Caller-supplied
prepared evidence and admission results are dropped. Plugin principals cannot
invoke the host-only DB1 operation. SQL constraints reject authoritative task
state. No task-local canonical memory table or memory-validity implementation
was introduced. A zero budget omits the entire coherent rendering; receipts bind
exact output bytes, revision, source versions and replay availability.

Promotion currently supports private corrections to an existing evidence target.
It previews the exact revision, selected claim, sources, scope and reviewer, then
submits an immutable draft through the existing correction-review owner. Both
submission and human approval recheck source versions. Approved hypotheses keep
their label and have partial lineage with unknown independent support. Shared
promotion and creation of a new canonical target explicitly report unavailable.
No projection is automatically promoted or preloaded.

Expiry/discard removes derived text, retaining digest-only replay metadata and
any ambiguous admission digest for reconciliation. Input memories, execution
receipts and already admitted canonical history remain under their owners.

Validation so far: full memory race suite (412.198 seconds) and exported build;
full isolated DB1 family race suite (2.433 seconds); final task schema/concurrency
checks (1.156 seconds); recovery delta (1.172 seconds); native transport boundary
test. The native test verifies fresh evidence replacement, authenticated identity,
internal-operation exclusion and genuine promotion-result finalization.

Migration 41 adds the bounded task-state column without rewriting earlier
migrations. Canary upgrade quiesces both owners and snapshots their exact stores
before migrating. Failure restores those stores before restarting previous images.
Snapshots stay private on CT109; only hashes are retained as evidence. Image-only
rollback across this schema change is not supported.

[Evidence manifest](memory-mr13-evidence-2026-09-27/manifest.json).

Live probes on the clock-corrected candidate validated explicit promotion review,
private hypothesis lineage, concurrent revisions, retirement invalidation and
execution-event changes. They are not final acceptance: follow-up fixes remove
working/draft text from response metadata and give task mismatch a valid HTTP
error envelope. The initial review probe used the wrong route spelling; a later
raw-SQL retirement fixture correctly failed the reviewed-record authority guard.
The corrected fixture uses governed delete/review instead. A build exhausted
canary disk space; inspected reclaimable build caches and recreatable committed
source snapshots were removed before the successful rebuild.
