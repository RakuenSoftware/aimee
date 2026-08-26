# P7 whole-vault TPM2 reseal orchestration

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

> **Archived complete (2026-07-26).** The audit found the scoped deliverables shipped,
> superseded by the current implementation, or fully represented by completed child slices.

- **State:** DONE. Delivered scope archived 2026-07-26.
- **Depends on:** P7 TPM2 PolicyNV anti-rollback, signed-HWM rotation, and
  steady-state key-use admission.

## Delivery split

The design review found this protocol too broad to review safely as one change.
Delivery is therefore fail-closed and ordered:

1. `P7-reseal-a`: canonical TPM receipt and prepared artifact
   `prepare/status/commit/cleanup`, provider-scoped process lock, and exhaustive
   swtpm crash tests. No database or production caller uses the new API.
2. `P7-reseal-b`: primary control-row barrier, complete protected-entry-point
   inventory, privilege enforcement, and admission/mutation race tests. No TPM
   generation changes.
3. `P7-reseal-c`: bounded inventory/staging/promotion schema and transaction tests
   against a mock custody provider. No TPM generation changes.
4. `P7-reseal-d`: end-to-end start/resume/reconciliation, WORM checkpoints, real
   TPM/PG17 kill matrix, and only then operator enablement.

The legacy one-shot TPM reseal remains explicitly unsafe for whole-vault use and
cannot bypass an in-progress prepared operation. Each slice has its own reviewed
plan, adversarial branch review, target validation, and merge.

## Scope

Deliver crash-safe, resumable rotation of the single TPM-pinned kb vault root:
place a primary-backed maintenance barrier, prepare a new TPM generation, stage
new wraps for every retained Postgres DEK and every principal verifier, commit the
TPM generation, atomically promote the staged database material, verify it under
the newly unsealed KEK, and append immutable intent/outcome records.

This is deliberately limited to a deployment proven to have one active kb runtime
and one pinned TPM. KMS and PKCS#11 fleet root rotation require provider-native
activation receipts and cross-node cache convergence and remain a separate slice.
The existing per-principal `vault_store_rekey` helper remains available for its
current callers but is not used for whole-vault rotation.

## Non-negotiable invariants

- No plaintext KEK, DEK, or credential is persisted. All sensitive locals are
  cleansed on every exit; database staging contains only wrapped DEKs/verifiers.
- Every retained row in `org_vault_secret`, including historical and staged
  credential versions, is re-wrapped. The current-pointer view and the fixed
  `VP_MAX_SLOTS` plan are not used.
- A durable primary barrier rejects new key-use admissions and all vault mutations
  before custody preparation begins. The local exclusive use guard prevents an
  already-admitted use from crossing the seal epoch.
- A database transaction is never held open across a TPM call.
- Before NV advances, abort is safe. After NV advances, the operation can only
  resume forward or remain sealed as `recovery_required`; old wraps are never
  reactivated.
- A matching operation ID, generation, and artifact digest, not NV equality alone,
is required before database promotion.
- Intent and terminal state have deterministic, append-only WORM outbox records;
  external WORM delivery is idempotent and cannot turn an uncommitted transition
  into a reported success.

## TPM2 prepared-reseal protocol

Replace orchestration's use of the single-shot
`vault_custody_tpm2_reseal()` with an inspectable, idempotent protocol while
retaining the old function as a compatibility wrapper:

```c
typedef enum {
  VAULT_TPM2_RESEAL_ABSENT,
  VAULT_TPM2_RESEAL_PREPARED,
  VAULT_TPM2_RESEAL_NV_ADVANCED,
  VAULT_TPM2_RESEAL_INSTALLED,
  VAULT_TPM2_RESEAL_CONFLICT,
  VAULT_TPM2_RESEAL_CORRUPT
} vault_tpm2_reseal_status_t;

typedef struct {
  uint8_t operation_id[16];
  uint64_t old_generation, new_generation;
  uint8_t artifact_digest[32], installed_digest[32];
} vault_tpm2_reseal_receipt_t;
```

Add `prepare`, `recover_kek`, `commit`, `status`, and `cleanup` calls keyed by the
operation ID and receipt. `prepare`, with NV at G, durably writes:

1. a recovery capsule holding the new KEK under PolicyNV(G);
2. a future active blob holding the same KEK and G+1 under PolicyNV(G+1); and
3. a manifest binding operation ID, G/G+1, and SHA-256 digests.

Each artifact is written through an `O_NOFOLLOW|O_EXCL` 0600 temporary file,
fsynced, renamed, and followed by a parent-directory fsync. The manifest is made
visible last. Existing final artifacts must either byte-match the receipt or
classify as conflict/corrupt. Paths are derived from the configured active blob
path; operation IDs are fixed hex and never accepted as arbitrary paths.

`commit` verifies the complete manifest before incrementing NV. If NV is G it
increments once; if NV is G+1 it resumes. It then atomically installs the prepared
future blob, fsyncs the parent directory, verifies the installed digest and
generation, and is idempotent for the same receipt. Startup/status can distinguish
and repair the critical crash after increment but before rename. NV > G+1, a
different active artifact, or missing/corrupt prepared material after increment is
`recovery_required` and never success. `recover_kek` opens the recovery capsule
only while NV is G. `cleanup` removes continuation artifacts only after durable DB
completion. All helper failures clear the cached old KEK and leave the provider
sealed.

## Primary schema and barrier

Add a singleton `kb_vault_control` row with `sealed`, monotonic `seal_epoch`,
`maintenance_kind`, `maintenance_id`, and monotonic `fencing_token`. Add one
`kb_vault_rewrap` operation row with request/operation IDs, actor, state, fencing
token, G/G+1, serialized custody receipt, inventory/stage counts and digests, and
failure classification. Permit only:

`preparing -> custody_prepared -> wraps_staged -> reseal_committing -> resealed -> promoted -> completed`

and pre-increment `aborted`, or fail-closed `recovery_required`. A partial unique
index allows one nonterminal operation.

Add staging tables keyed by `(operation_id, principal, agent, cred, version)` for
new wrapped DEKs plus the digest of each source wrap, and by
`(operation_id, principal)` for new wrapped KEK checks. Add an append-only
`kb_vault_rewrap_worm` outbox with deterministic event IDs. Revoke direct runtime
table access; expose only narrowly scoped SECURITY DEFINER functions with a pinned
`search_path` and explicit runtime grants.

Every SECURITY DEFINER function that admits key use or mutates vault secrets,
current pointers, KEK checks, or per-credential rotation state must lock/read the
control row and reject while whole-vault maintenance is active. The orchestrator
takes it `FOR UPDATE` for transitions. Existing slot locks remain in place but do
not replace this cross-instance barrier.

## Transaction and recovery protocol

1. Acquire the local exclusive maintenance guard and current KEK. T1 locks the
   singleton, proves no active maintenance/rotation, increments seal epoch and
   fencing token, installs the sealed maintenance barrier, inserts `preparing`, and
   appends intent in one commit.
2. Outside SQL, derive a fresh new KEK and call TPM `prepare`. T2 records the exact
   receipt as `custody_prepared` under the same fencing token.
3. T3 opens one transaction and streams all `org_vault_secret` rows in stable key
   order. It validates every principal's old KEK check, unwraps and immediately
   re-wraps each DEK, cleanses it, and inserts staged rows and new verifier wraps.
   Count and canonical SHA-256 inventory digests must agree before committing
   `wraps_staged`. Any corrupt/missing wrap rolls the entire transaction back.
4. T4 durably records `reseal_committing`; outside SQL, idempotent TPM `commit`
   advances/repairs the blob. T5 accepts only verified `INSTALLED` for the recorded
   receipt and records `resealed` plus outbox event.
5. T6 is SERIALIZABLE. It locks the singleton and operation, re-inventories all
   source rows, compares source digests, updates every source wrap and verifier
   from staging, checks affected counts, and commits `promoted` atomically.
6. Under the still-held local guard, unseal the installed blob and verify every
   retained wrap/verifier under the new KEK. T7 records `completed`, clears the
   maintenance kind while leaving the vault sealed, advances the fence, and appends
   success. Only then clean continuation artifacts and release the local guard.

Recovery first loads the database operation then obtains TPM `status` and follows
the same idempotent transition. `preparing` without artifacts may retry prepare;
prepared at G may recover the identical new KEK and resume staging; staged at G may
commit; G+1 with a valid future artifact installs it; installed resumes promotion;
promoted resumes verification/completion. Any receipt mismatch or missing artifact
after G+1 records `recovery_required` and retains both barriers.

Expose a typed start/resume/status API. A new request ID starts exactly once; a
repeat resolves the existing operation. Return committed, safe-retry, busy,
sealed/recovery-required, integrity, or unsupported deployment distinctly.

## Local seal guard

Add an orchestration-only writer guard alongside `vault_use_begin/end`. It obtains
the current KEK into caller-owned protected memory, holds `g_use_lock` exclusively
for the whole protocol, advances the local epoch as the primary barrier is
installed, and never implicitly unseals at completion. Cancellation is disabled
while the guard owns sensitive material and restored after cleanse/release.

The durable admission function returns the primary `seal_epoch`; use-in-place
requires it to match the synchronized local epoch before acquiring custody. This
closes the race where an admission commits just before T1 but waits locally until
after the root changes.

## Validation gates

- Unit/default build: stub APIs fail closed; receipt encoding/digest checks,
  idempotent transitions, exact replay, corrupt inventory, wrong KEK, cleanup,
  typed errors, guard epoch/cancellation, and sensitive-buffer canaries.
- ASAN/UBSAN: helper artifact parsing and all orchestration success/failure paths;
  no leak/use-after-cleanse and no callback after a barrier transition.
- Real PG17 on CT103: in-place schema upgrade and full P1 gate; all historical rows
  and more than 512 rows rotate; mutations/admissions race the barrier; WORM and
  state atomicity; source digest mismatch; fencing loss; forced disconnect during
  promotion; snapshot rollback remains sealed against G+1.
- CT260 real PG17 + libtss2 + swtpm: provision at G, rotate to G+1, restart both kb
  and swtpm, unseal and verify every retained version. Kill the driver after each
  durable boundary, especially immediately after NV increment and before active
  blob rename; restart using only PG, swtpm state, and artifact directory and prove
  same-operation completion or typed sealed recovery. Exercise wrong secret,
  external NV advance, corrupt/missing manifest/capsule/future blob, repeated
  prepare/commit/status/cleanup, symlink attacks, permissions, and old-blob replay.
- Scan database, files, logs, and crash artifacts for raw KEK/DEK canaries.

## Deferred

- KMS/PKCS#11 fleet root activation and cross-node cache convergence.
- Automatic operator scheduling/UI and destructive re-provision tooling.
- Per-credential compromise cancellation; it consumes this barrier but remains its
  own bounded admission-policy slice.
