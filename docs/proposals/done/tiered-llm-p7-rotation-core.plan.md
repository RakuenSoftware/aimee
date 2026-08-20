# P7 rotation core: anchor-authoritative credential versions

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Status: complete

## Scope

Land the persistence and custody primitives required by P7 §8 without adding an
operator HTTP route, vendor callbacks, or the steady-state egress use path. This
slice makes an org-vault `N+1` stage and post-anchor-CAS finalization durable and
crash-resumable for the KMS + signed-HWM custody path. The next bounded slice
composes these primitives into provision/probe/activate/revoke/retire.
TPM PolicyNV remains the already-delivered whole-KEK anti-rollback barrier; a
later operational wrapper may call `vault_custody_tpm2_reseal` only as part of a
whole-vault maintenance window, never as a per-principal rewrap.

## Storage contract

- Add a nullable `hwm_attestation BYTEA` to immutable `org_vault_secret` rows.
  Legacy/file-mode rows remain readable only outside hardened live-key mode.
- Add `org_vault_rotation`, uniquely keyed by `(principal,agent,cred)` while
  active, with stable `key_id`, `from_version`, `to_version`, state, compromise
  flag, timestamps, bounded last error, and the final signed attestation.
- States are `provision`, `staged`, `probed`, `activating`, `activated`, `revoked`,
  `retired`, plus terminal `failed`. The committed `activating` claim closes the
  race between a late failure transition and the external CAS. Transitions are monotonic compare-and-set operations
  under the slot advisory lock and append the existing DB2 WORM chain in the
  same transaction.
- Add SECURITY DEFINER functions to start/read/transition a rotation, stage an
  immutable `N+1` envelope without moving `org_vault_current`, and finalize the
  pointer plus HWM attestation after anchor activation. Revoke direct runtime
  table access and grant only these functions.

## Custody contract

- Add public fail-closed `vault_hwm_read` and `vault_hwm_cas` facades over the
  active custody provider. A provider missing either callback is unsupported;
  no Postgres-only fallback exists.
- The provider remains responsible for cryptographic verification of every
  returned token. KMS continues using the Ed25519 domain/key/version verifier.
- HWM calls are serialized per process. A failed/short/malformed/conflicting
  response never advances durable state.

## Core orchestration contract

- Add a kb-only core that starts a durable rotation from a verified signed HWM
  `N`, stages an already-encrypted immutable `N+1` envelope without moving the
  current pointer, invokes `hwm_cas(key_id,N,N+1)`, and finalizes the returned
  token plus pointer.
- Recovery reads the durable row and signed HWM: if the anchor reports `N+1`,
  finalize DB activation; if it reports `N`, leave the staged row inert for the
  driver to retry activation; any other version fails closed.
- This core accepts ciphertext envelope fields, never a vendor plaintext. The
  next slice owns provision/probe/revoke callbacks, in-place staged-secret use,
  compromise sealing, and the steady-state egress HWM check.

## Concurrency and failure rules

- One active rotation per slot and per stable HWM key; a key ID is permanently
  bound to one credential slot. Concurrent starters converge or get conflict.
- The rotation uses the existing `orgvault:` slot-lock namespace. Ordinary put,
  delete, and re-wrap writers take the same lock and reject a non-retired
  rotation, so they cannot invalidate a staged `from_version`. A failed attempt
  remains blocking until explicit operator remediation; it is not silently
  replaced over the immutable `N+1` row.
- Every core transition is idempotent. Replaying a completed stage/finalize
  returns its durable result without duplicating a version or WORM record.
- No external network call runs inside a Postgres transaction.
- The unavoidable crash window after external HWM CAS but before DB finalization
  is fail-closed (temporarily unavailable), then self-heals on resume; it never
  serves `N` after the anchor reports `N+1`.
- All version arithmetic rejects overflow and requires `to_version=N+1`.

## Validation

- Unit: missing HWM callbacks, signed read/CAS success, malformed/forged token,
  stale expected version, overflow, and idempotent recovery classification.
- Real PG17: immutable stage without pointer movement, tenant isolation,
  monotonic transitions, WORM atomicity, concurrent start/CAS, and rollback of
  failed transitions.
- CT260: signed mock-KMS helper plus real PG17; inject crashes before CAS and
  after CAS, resume both, prove the pointer never moves before anchor CAS, and
  verify secret-free logs/database rows.
