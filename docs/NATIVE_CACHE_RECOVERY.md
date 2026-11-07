# Native cache recovery owner contract

The new DB1 operations are custody primitives. No production caller currently
uses them to recover a native cache. Their existence and the store fixtures do
not prove that an SDK, descendants, or host tool effects are quiescent.

Sessions operation 38, `session_native_owner_claim`, is HostOnly and atomically
registers a qualified resource owner while claiming its derived native cache.
Sessions operation 39, `session_native_reconcile`, is HostOnly and atomically
reconciles that exact owner after an independent host process-tree owner proves
quiescence. Browser/model callers cannot invoke either operation.

Both accept the actual principal plus strict bounded JSON:

```json
{
  "version": 1,
  "session_id": "canonical-session",
  "turn_id": "exact-source-turn",
  "harness": "codex",
  "scope_sha256": "64 lower-case hex characters",
  "context_sha256": "64 lower-case hex characters",
  "binding_sha256": "64 lower-case hex characters",
  "owner_id": "32 lower-case hex characters",
  "claim_id": "32 lower-case hex characters",
  "containment_sha256": "64 lower-case hex characters",
  "expected_token": "exact prior cache token"
}
```

The cache key uses the existing SHA256 of `harness:scope:context`; neither a
caller-selected table key nor a current model/profile substitution can identify
the old claim. Registration accepts an absent, complete or invalid exact prior
token, requires an actual same-owner running source, and stores the pending
claim and complete owner binding in one transaction. Reconciliation accepts only
that owner's exact pending/finishing token. Legacy claims without a host owner
record are refused. An owner record is not a process-exit receipt.

Reconciliation locks the current session owner, rejects foreign source actors
and malformed/duplicate lifecycles, and preserves an already committed terminal.
An unfinished admitted turn receives one deterministic `cancelled` event with an
interruption explanation; it never receives invented completion, verification
proofs or a test grade. The event/outbox enqueue, recovered-turn writer fence,
cache invalidation and owner receipt commit together. Retries return the same
terminal reference. A stale call cannot invalidate a replacement owner's claim.

Ordinary and verified append reject new events in a recovered turn. Exact retries
of existing immutable events remain idempotent. The fence survives connection
restart and partial event retention; a deleted terminal cannot become a live
recovery receipt. Whole-session erasure cascades the minimal owner/fence metadata.
This closes a source-write race; it does not undo or re-execute host tool effects.

## Optional qualified containment integration

1. Probe an operator-delegated Linux cgroup-v2/supervisor capability without
   changing successful ordinary launches when unavailable. Merely owning a
   writable delegated cgroup is insufficient if the SDK can relocate itself.
   Qualification must prevent boundary escape (including writable ancestor
   controls, inherited descriptors and host `/proc/.../root` aliases), or retain
   ordinary fail-closed recovery. A private namespace or a separately privileged
   supervisor must be qualified for the actual deployment.
2. Create a random exclusive boundary before forking. Retain parent/leaf
   descriptors and record exact filesystem type, device/inode, random name,
   host/boot identity and launch-custody version. Persist its complete bounded
   descriptor in the pre-spawn cache lease marker. The containment digest binds
   those facts; a digest or caller boolean alone is never liveness evidence.
3. The actual forked child blocks before any SDK exec. Its parent places that
   exact still-owned child into the retained boundary, verifies membership and
   only then releases the barrier. Parent death/pipe EOF cannot release an
   uncontained SDK. All descendants must inherit enforced containment, including
   `setsid` descendants. No model/provider argument selects the boundary.
4. After effective SDK/profile attestation, the native cache producer uses
   operation 38 instead of its ordinary pending CAS only for a qualified owner.
   Persist exact scope/context/binding/owner/claim/containment facts before SDK
   thread/start/resume is released. The plain nonqualified path keeps its existing
   semantics. A crash before registration remains refused, never guessed safe.
5. On authenticated lazy restart, acquire the same physical cache-directory
   lease. Validate the old complete manifest against host/boot and exact held
   boundary identity. A live lease, missing/corrupt/foreign descriptor, replaced
   boundary or unsupported namespace posture refuses recovery. Do not derive
   safety from a daemon PID, listener state or direct-child waitpid.
6. Kill only that exact retained boundary through its owner and wait boundedly
   for machine-observed `cgroup.events populated=0` (or an equivalent supervisor
   receipt). Failure/timeout retains the marker and claim. Exclude boundary
   reuse; do not remove it before durable reconciliation commits. Host effects
   launched outside that boundary need their own existing owner/quiescence
   handling; this contract never claims they succeeded or retries them.
7. With actual quiescence retained, operation 39 performs source reconciliation
   and exact claim invalidation. Only after commit may the owner clear its exact
   marker/remove its exact empty boundary. A DB failure leaves retryable custody
   metadata. The next admitted request uses canonical full-history adoption,
   never an uncertain partial native handle. No SDK conversation tree is wiped.

Required qualification includes a real owning daemon/SDK surrogate, SIGKILL at
each preclaim/pending/checkpoint/native-bind/complete boundary, a still-writing
escaped-session descendant, live-owner refusal, replaced/foreign descriptors,
source-actor and exact-claim refusal, whole-tree empty proof, and retry after DB
failure. Store fixtures cover atomic owner registration/reconciliation, existing
terminal preservation, real concurrent append serialization, durable writer
fences, rollback, stale owner refusal and erasure. They deliberately do not
represent independent SDK execution or qualify the optional containment layer.


## Pre-exec reservation and shared cache epochs

HostOnly operations 40–44 extend the initial custody contract. They are store
primitives; only a qualified host containment owner can supply actual empty-tree
evidence. A JSON descriptor or successful DB call alone proves no quiescence.

* `session_native_reserve` (40) stores an exact pre-exec owner, running source,
  complete containment JSON string and its SHA256 before SDK configuration/cache
  access. It creates no pending native cache claim.
* `session_native_reservation_reconcile` (41) cancels an unfinished source or
  preserves its actual terminal, fences the abandoned writer and rotates the
  entire physical cache directory's epoch in one transaction. A registered
  pending/finishing claim is invalidated only by its exact actual token. A
  preclaim owner is reconciled without inventing a pending claim. Same-owner
  retries after commit return the same terminal and epoch.
* `session_native_epoch` (42) returns the current owner-checked directory epoch.
  Initial `0` preserves existing native handles. Every production launch must
  read it before selecting SDK HOME or cache scope, including ordinary launches.
* `session_native_reservation_lookup` (43) returns the exact durable owner and
  full descriptor for authenticated recovery. Local marker metadata is not a
  substitute for source ownership or kernel containment qualification.
* `session_native_reservation_finish` (44) records ordinary terminal cleanup
  after actual quiescence and an exact complete/invalid claim. It neither
  rotates the epoch nor adds a recovery writer fence. A crash after finish but
  before marker unlink can be retried without invalidating valid checkpoints.

Qualified operation38 requests include `epoch` and must match an active exact
reservation; their recovery uses41 instead of legacy39. Epoch-aware native HOME
and canonical checkpoint scope use the shared domain-separated hash
`SHA256("aimee-native-cache-epoch-v1:" + epoch + ":" + original_binding_or_scope)`.
Epoch0 leaves both unchanged. The physical lease remains on the original
binding, serializing all derived contexts. Epoch changes preserve old files and
checkpoints while preventing uncertain shared native handles from being resumed
by a different context.

Real PostgreSQL fixtures cover preclaim cancellation without fake claims,
shared epoch isolation, actual qualified pending/finishing registration,
transaction rollback at both epoch and receipt writes, normal finish without
cache invalidation, commit-before-unlink retry, corrupt descriptors, source
ownership and whole-session erasure. These fixtures deliberately use metadata
surrogates and do not assert SDK or kernel execution qualification. The final
production launch/recovery ordering is owned by the native execution adapter;
missing reservations, legacy or unqualified markers remain fail-closed unless
an independently qualified host pre-exec protocol proves the exact boundary.


Qualified launch ordering is DB-first: acquire the original physical directory
flock with an unpublished in-memory owner nonce; create a qualified boundary;
commit40 with the exact full descriptor before publishing a local marker or
accessing SDK HOME/config. Then atomically publish the marker and launch through
the retained pre-exec barrier. A local marker is SDK-writable and must never be
promoted into authority by creating a missing DB reservation. Before any fresh
admission, lookup43 without owner_id checks the directory's exact active durable
reservation even when no marker exists. That closes commit40-before-marker
crashes. Before40 no SDK can execute; an empty boundary alone conveys no cache
claim. Lookup43 also supplies the actual registered cache token, validated
against its exact owner, rather than trusting the marker's pre-registration token.
