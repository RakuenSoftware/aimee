# P7-reseal-d1 recovery, guard, and completion foundations

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** delivered and validated on PostgreSQL 17 plus swtpm (CT260), with
  default and ASAN/UBSAN builds; only fail-closed startup epoch synchronization
  is wired in production, with no recovery/rotation caller or operator enablement.
- **Depends on:** P7-reseal-a prepared TPM2 artifacts, P7-reseal-b primary
  barrier, and P7-reseal-c bounded staging/promotion.

## Scope and enablement boundary

Close the primitive gaps that make a crash-resumable production reconciler
possible without yet adding that reconciler. Add receipt discovery and prepared
KEK recovery to the TPM2 custody seam, add a process-local exclusive maintenance
guard with explicit primary/local epoch synchronization, require durable key-use
admission to match the local epoch exactly, and add owner-only `completed` and
fail-closed quarantine transitions to the Postgres state machine.

This slice adds no HTTP route, management RPC, CLI, scheduler, recovery worker,
external WORM drain, or automatic TPM operation. Its TPM recovery and database
transition seams remain uncalled by production orchestration; only the narrow
runtime-safe startup status seam initializes the local epoch barrier. The default
non-TPM build remains fail-closed. Successful completion leaves both the provider
and primary vault sealed; operational unseal belongs to P7-reseal-d3.

## Receipt discovery and prepared-KEK recovery

Add these build-guarded APIs beside the prepared-reseal helpers:

```c
int vault_custody_tpm2_reseal_discover(
    const uint8_t operation_id[16], uint64_t expected_old_generation,
    const char *secret, vault_tpm2_reseal_receipt_t *receipt,
    vault_tpm2_reseal_status_t *status);

int vault_custody_tpm2_reseal_recover_kek(
    const vault_tpm2_reseal_receipt_t *receipt, const char *secret,
    uint8_t new_kek[VAULT_KEK_LEN]);
```

`discover` closes the crash after the prepared bundle becomes durable but before
T2 records its receipt in Postgres. It holds both the in-process TPM mutex and the
existing provider-root interprocess `flock` continuously across path derivation,
all artifact opens/reads, manifest and component validation, authenticated NV
read, classification, and output publication. It derives only the canonical
fixed operation path, loads the manifest made visible last, validates every
canonical field and digest, requires the operation ID and exact `G/G+1`, and
returns the same receipt/status that the existing `prepare` plus `status` path
would return.
It never searches arbitrary directories, accepts a caller path, repairs an
artifact, changes NV, installs a blob, or generates a replacement KEK. Absent,
conflict, and corrupt are distinct statuses; a wrong secret is a typed error and
does not collapse into absence.

`recover_kek` holds the same two locks for one verification window spanning
manifest/component reads, exact receipt comparison, authenticated NV read,
capsule PolicyNV unseal, and final digest comparison. It is legal only for an
exact, fully verified `PREPARED` bundle while NV remains at G. The verified
manifest binds the capsule and its policy generation to the receipt's operation
ID, G/G+1, component digests, and `new_kek_digest`; recovery then requires the
32-byte plaintext hash to match that digest. It returns plaintext only into a
caller-provided protected buffer. Any other status, receipt/artifact mismatch,
NV drift, wrong secret, or parse error fails and cleanses the entire output. It
never uses a temporary active-blob path, populates the provider cache, changes
provider sealed state, or returns material in an error/log.

Output rules are total: functions zero caller outputs before validating input;
`discover` returns OK+ABSENT with a zero receipt only when no canonical bundle
exists, OK plus an exact nonzero receipt for PREPARED/NV_ADVANCED/INSTALLED, and
typed BUSY/INTEGRITY/ERR with zero receipt for lock failure, conflict/corruption,
wrong secret, or invalid input. `recover_kek` returns OK with a nonzero KEK only
for exact PREPARED-at-G; every other return leaves it all zero. Both default stubs
return `VAULT_TPM2_RESEAL_NOT_BUILT`, set status to ABSENT, and cleanse outputs.
Both real helpers inherit the prepared protocol's no-symlink/fixed-path rules,
bounded parsing, and artifact permission checks. Receipt, bundle, TPM auth/private
material, digest, and transient plaintext buffers are cleansed on every exit.

## Process-local maintenance guard and epoch authority

Add an opaque maintenance guard owned by `vault_server_key`, not the TPM module:

```c
typedef struct vault_maintenance_guard vault_maintenance_guard_t;
typedef int (*vault_maintenance_kek_fn)(const uint8_t kek[VAULT_KEK_LEN],
                                       void *ctx);

int vault_maintenance_guard_begin(vault_maintenance_guard_t **guard);
int vault_maintenance_guard_sync_primary_epoch(vault_maintenance_guard_t *guard,
                                               uint64_t primary_epoch);
int vault_maintenance_guard_with_active_kek(vault_maintenance_guard_t *guard,
                                            vault_maintenance_kek_fn callback,
                                            void *ctx);
int vault_maintenance_guard_unseal(vault_maintenance_guard_t *guard,
                                   const void *params, size_t len);
int vault_maintenance_guard_seal(vault_maintenance_guard_t *guard);
int vault_maintenance_guard_end(vault_maintenance_guard_t **guard);
int vault_primary_epoch_initialize(uint64_t primary_epoch);
```

`begin` disables POSIX thread cancellation, obtains `g_use_lock` exclusively for
the entire future orchestration protocol, drains every existing admitted reader,
and records the owning thread and exact prior cancellation state. It does not
implicitly fetch or export a KEK. A same-thread recursive begin returns typed
`BUSY` instead of deadlocking; a different thread blocks normally. Guard identity
is registered separately from its allocation so an alias, stale, forged, or
wrong-thread pointer is rejected without dereferencing freed/untrusted memory.
Failure and owner `end` cleanse and unmap guard material, release the writer lock
exactly once, then restore the prior cancellation state. Wrong-thread calls leave
the live guard and lock untouched. At-fork handlers permanently invalidate the
child image, wipe fixed-address file/principal/TPM caches, and close the child's
copy of any prepared-operation flock without touching inherited pthread or ESYS
objects. Every custody API then fails before locking; the child must `exec` before
using custody again.

Only guard-owned functions may touch custody while the writer lock is held; they
call lock-assuming internals and never recursively enter public
`vault_unseal`/`vault_seal`. `with_active_kek` places the KEK in a guard-owned,
`mlock`ed, `MADV_DONTDUMP`, and `MADV_WIPEONFORK` arena, invokes a synchronous
callback while the exclusive lock remains held, then cleanses it before return.
Callback-active state rejects guard end/seal/unseal/sync recursion, and TLS use
ownership rejects public seal/use recursion rather than deadlocking or releasing
the writer lock. A callback-side fork observes a kernel-wiped child arena and its
return path fails without touching inherited locks. The pointer cannot
escape by contract and no raw-KEK getter is added. Guard `seal` clears every KEK
cache and advances the process-local transition generation even if provider seal
reports failure. Owner `end` also performs this fail-closed seal/cache-clear
before unlocking; it returns a typed error if seal failed but never opens ordinary
use after an incompletely terminated maintenance session.

Keep two distinct authorities under `g_use_lock`: `g_use_epoch` is the
process-local transition generation, while `g_primary_seal_epoch` plus an
initialized bit is the last explicitly synchronized Postgres barrier epoch.
They are never assigned to each other. `vault_primary_epoch_initialize` is
startup-only: it takes the exclusive lock, requires no guard/use active, accepts
a positive signed-64 epoch once or as an exact replay, and rejects a different
second initialization. The selected PKCS#11/KMS provider may already be unsealed
by its startup login; the caller first forces a seal whenever durable control
says sealed. A narrow runtime-safe status function holds the primary shared
advisory lock in an explicit startup transaction until local initialization has
completed, closing the status-to-initialize race. Initialization cannot unseal or
obtain a KEK.
Provider rebind, seal, fork, and test reset advance the local generation and
invalidate primary synchronization.

`guard_sync_primary_epoch` is owner-only and occurs only after the corresponding
primary transaction committed. An exact same-epoch resume is idempotent and does
not advance `g_use_epoch`; a greater primary epoch installs the new value and
advances the local generation exactly once; zero, signed-64 overflow, regression,
or local-generation exhaustion fails closed.

Change `vault_use_begin` to accept both the pre-admission local-generation
snapshot and the durable admitted primary `seal_epoch`. After acquiring its
shared lock it atomically requires the local snapshot to equal `g_use_epoch`,
primary synchronization to be initialized, and the durable epoch to exactly
equal `g_primary_seal_epoch` before any custody access. A zero, overflowed, lower,
higher, or unsynchronized epoch fails and cleanses output. `kb_vault_key_use`
passes `admitted.seal_epoch` into this locked check; no comparison outside the
lock is treated as authoritative. This closes the admission-at-T1 race where a
durable admission from one primary epoch waits until after a local root
transition.

## Owner-only completion and quarantine schema

Extend the operation state set with terminal `completed` and extend the local
immutable outbox event-kind set with deterministic `completed`. This transactional
outbox is not claimed to be an off-database WORM sink; delivery and acknowledgement
remain D3. `completed` requires every receipt, inventory, and stage field already
fixed by `promoted`; `completed` is excluded from the one-active-operation partial
index and has no outgoing edge, including no quarantine edge.

Add `org_vault_rewrap_complete(operation,fence,receipt_digest,inventory_digest,
stage_digest)`. It takes the exclusive control gate then the operation row,
requires exact `promoted` state/fence and byte-equal stored digests, and supports
exact replay after the control fence advances. The operation retains the consumed
fence and complete terminal payload; replay also requires the single deterministic
outbox row to match exactly, so it does not depend on the advanced live control
fence. In one transaction it records `completed`, appends the completed outbox
checkpoint, clears only
`maintenance_kind/maintenance_id`, advances the control fence with exhaustion
checking, and deliberately leaves `kb_vault_control.sealed=true`. It does not
delete staging or authorize unseal. P7-reseal-d2 calls it only after guarded
post-promotion cryptographic verification; P7-reseal-d3 separately authorizes
prepared-artifact cleanup and operational unseal.

Generalize the existing `org_vault_rewrap_recovery_required` transition into a
quarantine seam legal from every active state (`preparing` through `promoted`).
This is required when the TPM is already G+1 or installed but Postgres is still
at an early state. It takes the same lock order, exact fence, and lowercase
failure-class token; preserves whatever receipt/inventory/stage evidence is
already present; atomically records the immutable recovery-required outbox event;
advances the fence; and retains both `sealed=true` and the maintenance identity.
It never clears staging, rolls back wraps, or permits a later automatic
transition. Add `failure_from_state` so the terminal operation itself durably
binds the exact source phase, not merely its outbox detail. Recovery-required
allows exactly three evidence shapes: `preparing` provenance has no receipt or
digests and zero counts; `custody_prepared` provenance has the complete receipt
pair but no inventory/stage digests and zero counts; `wraps_staged`,
`reseal_committing`, `resealed`, or `promoted` provenance has the complete receipt
pair plus both inventory/stage digests and counts. Half-pairs, fabricated later
evidence, and evidence loss fail the table constraint. Exact terminal replay
matches operation, consumed fence, failure class, source phase, preserved
evidence, and deterministic outbox row before consulting the advanced live fence.
Earlier transition replays use `failure_from_state` plus the applicable
resealed/completed checkpoint to prove the requested transition committed
before quarantine; full digests alone are not accepted as provenance.

All new functions remain owner/migration-orchestrator only: pinned search path,
schema-qualified objects, RLS with no table policies, and explicit PUBLIC/runtime
revokes in both schema and grant reapplication. SQLite mirrors columns/states only
and exposes no authoritative transition behavior. Creation-time default EXECUTE
is revoked for every exact overload; tests check effective privileges through
inherited memberships, and the owning role is non-login. D2 adds one narrow
in-process orchestration authority rather than giving runtime schema-owner
credentials. SQLite drops/recreates the active-operation index to exclude
completed.

Schema reapplication backfills pre-d1 `recovery_required` provenance from the
exact immutable P7-reseal-c WORM detail before validating the stronger constraint.
The SQLite shim migration adds the mirror column to existing support databases;
neither shim path gains authoritative transition behavior.

## Validation gates

- Default build and unit tests: non-TPM stubs fail closed and cleanse outputs;
  guard ownership, cancellation restoration, cache cleansing, epoch monotonicity,
  exact admission/local-epoch equality, wrong-thread/double-end/fork behavior,
  reader-before-writer drain and writer-before-reader blocking with condition
  variables (not timing guesses), and sensitive-buffer canaries. Test initially
  enabled and disabled cancellation, same-thread BUSY, wrong-thread operations,
  callback failure, provider seal/get failure, exact sync replay, regression,
  signed-64/local-generation exhaustion, provider rebind, and unsynchronized use.
- Real PostgreSQL 17 on CT260: fresh and twice-reapplied schema/grants; transition
  matrix for completion and quarantine from every active state and every exact
  partial-evidence shape; earlier-transition replay provenance; exact replay,
  stale fence, digest mismatch, fence exhaustion, concurrent completion versus
  quarantine (exactly one terminal winner), outbox UPDATE/DELETE/TRUNCATE trigger
  denial, direct and inherited effective privilege denial for every exact overload,
  and full P1/P7 gate.
- Real swtpm on CT260 with the WITH_TPM2 build: prepare, restart the helper state,
  discover the byte-identical receipt, recover the exact KEK at G, and prove wrong
  secret/op/generation/receipt, corrupt/missing manifest/capsule/future artifact,
  symlink, permissions, NV advance, installed, and cleaned states never return
  plaintext. Run each command in a fresh process, restart swtpm after prepare, and
  recover using only swtpm state, the active blob, and prepared bundle. Repeated
  discovery/recovery at G is read-only and deterministic; after commit advances
  NV to G+1, the same recovery call fails with a zero output while the installed
  active KEK unseals and old-blob replay fails.
- ASAN/UBSAN on discovery, recovery, guard success/failure, and concurrent use;
  include cancellation/thread stress and concurrent second-process artifact-lock
  attempts; scan database, artifacts, logs, and process outputs for raw KEK
  canaries.
- Source/link inventory proves there is still no production recovery/rotation
  caller, route, scheduler, worker, or operator capability beyond the fail-closed
  startup epoch synchronization.

## Deferred to P7-reseal-d2/d3

P7-reseal-d2 owns canonical receipt encoding in Postgres wrappers, the in-process
DB-state x TPM-status reconciler, bounded real re-wrap orchestration, guarded
post-promotion verification, and exhaustive fake-provider state-machine tests.

P7-reseal-d3 owns local authenticated operator start/resume/status and explicit
unseal, protected secret input, startup fail-closed detection, idempotent external
WORM delivery with sink acknowledgement, the PG17/swtpm kill-after-every-boundary
matrix, artifact hardening not already required by D1, and the only enablement
switch. Remote rotation, scheduling, multi-node TPM operation, KMS, and PKCS#11
activation remain out of scope.
