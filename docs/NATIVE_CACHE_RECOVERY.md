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
