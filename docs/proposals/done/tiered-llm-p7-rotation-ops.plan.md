# P7 rotation operations: fenced vendor workflow

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** completed (2026-07-20).
- **Evidence:** local build/lint/unit and GCC ASAN+UBSAN; real PG17.10 schema,
  RLS, 12-worker fencing, expiry takeover, and stale-winner gates on CT103;
  signed mock-KMS HWM plus lost-response mock-vendor recovery on CT260.

## Scope

Compose the delivered anchor-authoritative rotation core into a vendor-neutral,
crash-resumable provision → stage → probe → activate → revoke → retire driver.
This slice adds no operator HTTP route, scheduler, or concrete cloud SDK. A mock
vendor on CT260 verifies the complete workflow and its crash matrix. Steady-state
egress HWM enforcement, the compromise admission barrier, and whole-vault TPM
reseal maintenance remain separate slices because each changes a different live
use or maintenance boundary. Until the admission barrier lands, this driver
rejects `compromise=true` rotations; the new workflow therefore cannot create an
unprotected compromise-mode activation. With no route or scheduler, the driver
also has no production caller in this slice.

## Durable operation contract

- Extend `org_vault_rotation` with bounded, non-secret `old_vendor_ref`,
  `new_vendor_ref`, `revoke_receipt`, `failure_phase`, and a fenced work claim
  (`claim_owner`, `claim_token`, `claim_until`). Vendor references identify
  credentials but never contain credential material.
- Add SECURITY DEFINER claim/release/checkpoint/remediate functions. A claim is
  acquired atomically on the primary for one expected state, increments the
  fencing token, and may be stolen only after its database-time expiry. Every
  state/result commit from an external call must present the current token. The
  claim tuple and state CAS are persisted on the same rotation row transaction.
  The current owner heartbeats at no more than one third of the lease TTL; a
  provider callback that cannot be heartbeat safely is unsupported. Stale workers may cleanse their local
  result but cannot mutate the rotation; overlapping calls after a crashed or
  paused owner are safe only through the provider idempotency contract below.
- Derive stable per-step operation keys from the durable rotation ID and step,
  never from a process-local retry count. Provider callbacks must reconcile a
  repeated provision after an uncertain result: either return the same
  credential or revoke the orphan before returning a replacement. A provider
  that cannot provide vendor-side duplicate suppression/reconciliation is
  unsupported for automatic rotation. Returning a different credential without
  positive orphan-revocation evidence fails closed and is never staged.
- Keep every vendor call and every HWM call outside PostgreSQL transactions.
  Claims and result checkpoints use short verified-principal tenant scopes.
- Every new definer function pins `search_path=public`, is revoked from PUBLIC,
  is executable only by `aimee_kb_runtime`, re-checks the tenant actor/team, and
  appends the existing DB2 WORM chain in the same transaction as its state CAS.
  Direct runtime table access remains revoked.

## Transition table

| State | Claimed external action | Success | Definite failure | Uncertain result |
|---|---|---|---|---|
| `provision` | idempotent provision/reconcile | atomic envelope + ref checkpoint to `staged` | `failed(provision)` | remain `provision`, reconcile by operation key |
| `staged` | audited staged-key probe | `probed` | `failed(probe)` | remain `staged`, repeat idempotent probe |
| `probed` | none; core claims activation | `activating` | n/a | retry core claim |
| `activating` | signed HWM read/CAS | `activated` | n/a | resume from verified anchor |
| `activated` | idempotent revoke/query-status | `revoked` with receipt | remain `activated` | remain `activated`, reconcile by operation key |
| `revoked` | no external call | `retired` | n/a | retry local transition |
| `failed` | explicit reconcile + anchor check | audited `retired` and inert-row purge | remain `failed` | remain `failed` |

Claims never create another active rotation; the existing unique active-slot
constraint remains authoritative. `failed → retired` exists only inside the
dedicated remediation function, not the generic transition function.

## Secret lifecycle and callbacks

- Add a small provider vtable in `kb_vault_rotation_ops.[ch]`: provision,
  probe, revoke, and reconcile. It is process configuration only; all
  authoritative progress remains in PostgreSQL. Registration is startup-only,
  requires the complete vtable, and is serialized against use.
- Provision writes the one-time plaintext only into a bounded secure buffer.
  A vault-owned callback boundary accepts that buffer and returns only an
  envelope; orchestration never receives a KEK or DEK. The buffer is at most
  4096 bytes and uses a page-aligned arena whose `mlock` and `MADV_DONTDUMP`
  protections are mandatory: allocation fails closed if either protection
  cannot be established, and `OPENSSL_cleanse` runs before `munlock`/free on
  every exit. The atomic stage checkpoint persists ciphertext and
  `new_vendor_ref` together. `old_vendor_ref` is captured by the same provider's
  pre-provision reconciliation and persisted before provision is claimed; a
  missing old reference blocks activation. There is no plaintext-returning public accessor.
- Probe reads exactly the inert `to_version` ciphertext envelope through a new
  rotation-scoped DB function. A vault-owned staged-use primitive decrypts only
  inside the protected arena, invokes the provider callback there, and returns
  only success/failure. A secret-free, idempotent WORM probe-use admission is
  durably appended before decrypt/use; then the arena is cleansed before the
  fenced `probed` checkpoint.
  Ordinary vault reads continue to resolve only `org_vault_current`.
- Revoke receives only the durable old vendor reference and stable operation
  key. The same callback accepts either old or new references so remediation can
  revoke an abandoned candidate. The provider must confirm the credential is
  unusable (not merely that a request was accepted/already queued) and return a
  bounded non-secret receipt; success atomically checkpoints `revoked` plus that
  receipt. Retire is a local fenced transition only when the receipt is present.
- Activation continues through `kb_vault_rotation_activate_or_resume`; the
  external anchor remains the sole activation commit point.

## Failure and remediation

- A definite pre-activation provider failure records `failed` and its phase.
  An uncertain side effect remains in its forward state for reconciliation; it
  is never guessed successful or marked failed merely because a lease expired.
- Explicit remediation is allowed only while the verified anchor still reports
  `from_version`. A `failed` row has no legal activation transition, so normal
  workers cannot advance its anchor during remediation. Remediation first obtains
  positive reconcile/revoke evidence, re-reads the anchor, then an
  audited SQL function removes only the inert staged `to_version` row and retires
  the failed attempt. It never deletes the rotation history/WORM rows, a current
  row, or an anchor-attested version; the staged row must have NULL HWM
  attestation. A failed provision with no vendor reference is remediable only
  after reconciliation positively reports that no credential exists for the
  operation key. Any mismatch or unavailable anchor remains blocking.
- An `activating` row is never abandoned. If the anchor reports `to_version`,
  recovery finalizes; if it reports `from_version`, recovery retries CAS. Any
  other anchor version fails closed for operator investigation.
- Failed/uncertain revoke remains blocking and resumable; old credential bytes
  are retained as ciphertext, and retirement never claims cryptographic erasure.

## Validation

- Unit: lease acquisition/expiry/fencing, stale-result rejection, deterministic
  operation keys, provision reconciliation, staged-only probe, callback failure,
  full cleansing, idempotent revoke/retire, compromise-start rejection, a
  different-secret replay rejected without positive orphan evidence, and
  remediation preconditions.
- Real PG17: two workers race every claim; only the winning token checkpoints.
  Prove no external callback runs in a transaction, stale owners cannot advance,
  remediation cannot delete current/attested rows, and every transition appends
  one secret-free WORM record atomically. Inject WORM admission failure and prove
  probe never decrypts or calls the vendor.
- CT260: run real PG17 plus signed mock KMS HWM and a mock vendor. Inject crashes
  before and after each external side effect, including a lost one-time provision
  response and revoke response, then resume to one active N+1 credential, one
  revoked N credential, and a retired durable rotation without plaintext in DB
  rows, logs, or artifacts.

## Explicitly deferred

- Concrete AWS/IAM, Bedrock, PKCS#11, or cloud-KMS vendor-management adapters.
- HTTP/CLI routes, scheduling, cadence policy, and approval UX.
- Steady-state egress verification of the current row against fresh signed HWM.
- Compromise-mode per-slot admission blocking and in-flight capability fencing.
- Whole-vault KEK rotation and `vault_custody_tpm2_reseal` maintenance windows.
