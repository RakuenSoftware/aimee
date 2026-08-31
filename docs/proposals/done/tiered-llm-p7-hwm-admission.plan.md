# P7 steady-state signed-HWM key-use admission

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** delivered and merged-ready implementation slice.
- **Depends on:** P7 signed-HWM rotation core and fenced rotation operations.

## Scope

Add the anchor-authoritative, fail-closed key-use primitive that later live egress
drivers must call. The primitive resolves one exact credential version from a fresh
signed custody high-water read, atomically admits that version with a WORM event on
the Postgres primary, verifies both the fresh and stored attestations, decrypts only
inside a protected arena, invokes a bounded use callback, and cleanses every local
copy. It never returns the plaintext or the KEK.

This slice deliberately does not expose a route or claim that Bedrock egress is fully
wired. The current Bedrock dispatch seam still accepts raw AWS credentials; P2b must
replace that signature with this primitive before the live route can ship. Likewise,
compromise-mode admission fencing and whole-vault TPM reseal remain later slices.

## Binding and migration contract

- The stable anchor `key_id` remains permanently bound to one
  `(principal, agent, cred)` slot by the durable rotation history and its existing
  uniqueness/conflict checks. Admission rejects a missing or ambiguous binding.
- Only a current version with a non-empty activation attestation is usable. Existing
  version-1 rows with no completed signed-HWM rotation are not silently grandfathered,
  auto-signed, or accepted from the mutable Postgres pointer. They fail with a typed
  `unattested` result until an operator performs a normal signed-HWM rotation.
- No schema migration invents anchor truth. Rollout is additive and preserves old
  ciphertext, rotation history, and WORM rows.
- The per-key HWM provider is mandatory. Today the signed KMS-HWM helper supplies it;
  TPM2 and PKCS#11 custody continue to fail closed for live use until they implement
  their own per-key signed read/CAS/verify seam. The TPM root-generation PolicyNV
  counter is not misrepresented as a per-credential HWM.

## Custody and DB contracts

- Extend the custody HWM seam with `hwm_verify(key_id, version, attestation)`. Add
  `vault_hwm_verify` to the shared facade. `hwm_read`, `hwm_cas`, and `hwm_verify`
  must all exist for attested key use; unsupported, unavailable, malformed, and
  invalid-signature cases fail closed. The KMS provider uses the existing Ed25519
  domain-bound verifier. Other providers remain unsupported rather than falling back.
- Add a DB2 envelope type and a read-only `db2_vault_key_use_candidate` wrapper. It
  resolves the exact anchor version, stable binding, envelope, and stored attestation
  but performs no admission. C verifies the fresh and stored signatures before any
  event can claim that use was authorized.
- Add immutable `org_vault_key_use_intent`, keyed by
  `(team_id,authenticated_origin,use_id)`, binding the use to key ID, slot, version,
  canonical request SHA-256, provider, model, and operation. Runtime has no direct
  access; SECURITY DEFINER functions expose no update/delete operation and ordinary
  team deletion is restricted while retained intents exist. This durable intent is
  not a plaintext or credential store.
- Add `db2_vault_key_use_admit` over a new `org_vault_key_use_admit` SECURITY
  DEFINER function. It receives all verified bindings plus the exact stored
  attestation bytes. In one short primary transaction it takes per-use and slot
  advisory/row locks, re-checks tenant authorization and stable key binding, rejects
  an `activating` rotation, requires `org_vault_current.version = anchor_version`,
  byte-compares the exact row's stored attestation, inserts the immutable intent,
  appends one secret-free `vault.key_use` WORM admission, and only then returns the
  exact envelope. Intent and WORM append commit atomically; WORM failure returns no
  envelope and leaves no intent.
- A conflicting origin/use ID with any different binding fails as integrity misuse. An
  exact replay returns a typed replay result and no envelope, so the plaintext
  callback cannot execute twice under one admission. A crash after admission but
  before callback therefore remains safely non-replayable; later P2b dispatch
  reconciliation decides the request outcome rather than silently using the key
  again.
- The function is revoked from PUBLIC, granted only to `aimee_kb_runtime`, pins
  `search_path=public`, and leaves direct runtime table access revoked. The SQLite
  schema mirrors only additive storage shape; this security boundary is PG-only.

## C use-in-place contract

Replace the unused/misleading `kb_vault_key_use` KEK callback with an explicit
attested-slot API:

1. Reject invalid identity/slot/use inputs and a sealed or non-live-key profile.
   Require a 64-lowercase-hex canonical request SHA-256 plus bounded provider,
   model, operation, authenticated origin, and use ID; replay metadata cannot be changed.
2. Read the fresh anchor HWM outside a DB transaction and verify its signature.
3. Read the exact candidate envelope and verify its stored activation attestation
   for the same `(key_id, version)`.
4. Call the atomic DB admission with the verified token and all immutable bindings.
   Stop on a replay or conflict; only a newly committed admission returns an envelope.
5. Allocate a page-aligned `mlock` + `MADV_DONTDUMP` arena; protection failure is
   retryable and the plaintext callback is never invoked.
6. Obtain the custody KEK into the arena, unwrap the row DEK, decrypt with the
   domain-separated, length-prefixed v2 slot AAD (with a delimiter-free v1 read
   fallback), and invoke the callback with only the bounded plaintext credential bytes.
7. `OPENSSL_cleanse` the entire arena and envelope on every exit before
   `munlock`/`munmap`.

The credential plaintext is capped at 4096 bytes. The KEK, DEK, and plaintext exist
only in the locked arena. The callback is synchronous, receives a borrowed
read-only pointer valid only for its invocation, and must not retain it or perform a
non-local exit; this is an internal provider contract, not a public extension seam.

Add a local use/seal serialization guard and monotonic local seal generation to the
shared custody facade. The call snapshots the generation before admission, then takes
the guard immediately before KEK acquisition and proceeds only if the generation is
unchanged and the provider is still unsealed. It holds the guard through callback
cleanup. `vault_seal` takes the exclusive side, increments the generation before
sealing/cache flush, and never rolls it back on unseal. Thus a use that already entered
the protected boundary may finish, while an admission waiting across seal, even if an
unseal follows, is denied. Fleet-wide Postgres seal epochs remain a separate
multi-instance slice and are not claimed here.

Disable pthread cancellation from protected-boundary entry through arena cleanup and
restore the caller's cancellation state afterward. Fatal process termination remains
outside an in-process cleanup guarantee; `MADV_DONTDUMP` and the no-retention callback
contract bound that residual risk.

Return an exact enum: `OK`, `RETRY`, `SEALED`, `UNATTESTED`, `INTEGRITY`, `REPLAY`,
and `CALLBACK_FAILED`. Provider/DB/protection unavailability before a committed
intent maps to `RETRY`; a committed exact replay maps to `REPLAY`; malformed
envelopes, signature/binding/digest conflicts, and impossible SQL results map to
`INTEGRITY`; profile/seal denial maps to `SEALED`. No integrity denial is reported as
success or ordinary vendor failure. A WORM admission records an authorized attempted
use; callback outcome events remain separate and are deferred.

If the DB connection is lost after the primary may have committed, return `RETRY` with
no envelope and no callback. The retry resolves the immutable intent: an actual commit
returns `REPLAY`, while a rolled-back transaction may create one fresh admission. This
is fail-closed under an indeterminate commit.

Normal rotation may leave a previously admitted version in flight. The DB admission
rejects the `activating` crash window, so an anchor advance cannot admit a stale new
use before finalize moves the pointer. The stronger rule that blocks all old-version
uses during compromise rotation is explicitly deferred to the compromise barrier.

## Validation

- Unit: fresh/stored valid attestations admit exactly one callback; exact replay
  invokes no callback and conflicting replay metadata is integrity failure; anchor rollback,
  pointer rollback, missing/ambiguous binding, NULL stored token, invalid fresh or
  stored signature, anchor outage, activating state, WORM failure, sealed profile,
  protected-arena failure, envelope tamper, and callback failure all fail with the
  specified type and never leak/call unexpectedly. Assert the callback receives the
  credential rather than the KEK and all test canaries are cleansed.
- Unit the arena as a checked struct containing the full 4096-byte plaintext region,
  KEK, DEK, and cryptographic scratch, rounded up to whole locked pages; reject every
  length before copying. Exercise cancellation deferral and seal-generation change
  between admission and boundary entry.
- Real PG17 on CT103: RLS/definer/grant checks, exact-version envelope selection,
  immutable intent binding including cross-team inverse-history conflicts,
  concurrent same-use-ID admission with one new winner and
  replay-only losers, WORM-failure atomicity,
  activating-state refusal, and a restored old current pointer rejected against a
  newer supplied anchor version. Re-run the full P1 gate and in-place schema upgrade.
- CT260: real PG17 plus the signed mock-KMS helper. Provision/rotate to an attested
  current row, perform use through the new callback, restore the DB pointer to N while
  the helper remains at N+1, and prove the callback is not invoked. Corrupt the stored
  signature and prove refusal. Run GCC ASAN/UBSAN and scan artifacts/logs/DB rows for
  the plaintext canary.

## Explicitly deferred

- P2b live `/v1/llm/egress` and removal of raw credential parameters from the
  Bedrock dispatch boundary.
- Compromise-driven per-slot seal/capability fencing and cancellation policy.
- TPM2/PKCS#11 per-key HWM implementations, whole-vault TPM reseal maintenance,
  scheduled rotation, outcome audit events, and off-host WORM witnessing.
