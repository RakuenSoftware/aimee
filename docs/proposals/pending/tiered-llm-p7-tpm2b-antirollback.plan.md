# P7-tpm2b plan — TPM2 NV monotonic-counter anti-rollback for the sealed KEK (P7 §9)

Slice of P7. Branch off `testing`. Adds the P7 §9 ANTI-ROLLBACK core to the tpm2 custody
provider (P7-tpm2a, merged): a TPM2 NV **monotonic counter** binds a GENERATION into the sealed
blob, so restoring an OLD sealed-KEK blob after a re-seal is DETECTED and REFUSED. Validated on
swtpm (CT260). tpm2a shipped a strictly create-once provider (one blob, no rollback surface);
tpm2b enables multi-generation re-seal WITH rollback protection.

## Verified substrate

- P7-tpm2a (`vault_custody_tpm2.c`, WITH_TPM2-guarded, swtpm-validated): persistent Owner primary
  (verified idempotency), KEK sealed as a keyedhash under `userWithAuth` (high-entropy operator
  secret), salted + parameter-encrypted sessions BOTH directions, create-once provision,
  crash-safe blob (marshaled TPM2B_PUBLIC+PRIVATE, temp+fsync+rename, O_NOFOLLOW, bounded
  unmarshal), full ESAPI lifecycle hygiene. `is_sealed`/`unseal`/`seal`/`get_kek` work.
- The custody `rotate` vtable slot drives a VAULT-WIDE DEK re-wrap; the file provider's
  `file_rotate` is `.vault`-file-store-coupled, and the kb store is Postgres — so the full
  re-wrap is a kb-integration concern (NOT swtpm-standalone). This slice delivers the custody
  ANTI-ROLLBACK PRIMITIVE the kb rotate flow will call; the Postgres DEK re-wrap stays a
  documented kb-rotate integration.
- swtpm (CT260) supports `Esys_NV_DefineSpace` (TPMA_NV_COUNTER) + `Esys_NV_Increment` +
  `Esys_NV_Read` — a real, monotonic, TPM-backed counter (the same primitive on hardware).

## Design decisions

1. **A TPM2 NV MONOTONIC COUNTER as the anti-rollback authority.** Define ONE NV index (config
   `vault.tpm2.nv_index`, default e.g. `0x01500001`) with `TPMA_NV_COUNTER | TPMA_NV_AUTHREAD |
   TPMA_NV_AUTHWRITE | TPMA_NV_NO_DA | TPMA_NV_POLICY_DELETE?`, owner-authorized, created once at
   first provision (idempotent: if defined with the expected public, reuse; a mismatched
   definition → fail closed). A monotonic counter can ONLY increase (the TPM enforces it) and
   survives power cycles — the trust anchor for "which generation is current".
2. **The sealed blob BINDS its generation.** The sealed data becomes `KEK (VAULT_KEK_LEN) ||
   generation (8 bytes, the NV counter value at seal time)` — a fixed layout inside the
   TPM-sealed sensitive area (so the generation is TPM-integrity-protected, unforgeable without
   this TPM). The on-disk blob format gains a version byte (v2) so a tpm2a (v1, generation-less)
   blob is still loadable + treated as generation 0 (back-compat) OR is rejected (decide at
   review; lean: accept a v1 blob as "generation == current NV" on first tpm2b unseal to avoid
   bricking an existing tpm2a deployment, then it migrates to v2 on the next re-seal).
3. **Unseal VERIFIES the generation (rollback detection).** After `Esys_Unseal`, split the
   recovered data into KEK||gen; `Esys_NV_Read` the counter; if `gen != current_nv` → the blob is
   STALE (an older generation restored after a re-seal) → REFUSE (return -1, stay sealed,
   zeroize). Only a blob whose bound generation equals the live NV counter unseals. (Software
   compares the TPM-sealed generation to the TPM-backed NV counter — an attacker cannot forge the
   sealed generation, and cannot decrement the monotonic NV counter, so an offline old-blob
   restore is caught on the next unseal.)
4. **`vault_custody_tpm2_reseal(new_kek, secret)` — the generation-bumping re-seal.**
   `Esys_NV_Increment` the counter → new generation G'; seal `new_kek || G'` (same template,
   command-encrypted session as tpm2a provision); ATOMICALLY replace the blob (temp+fsync+rename).
   After this, the OLD blob's bound generation G < G' == NV → the old blob no longer unseals
   (anti-rollback). This is the primitive the kb rotate flow calls WITH the new KEK AFTER
   re-wrapping the Postgres DEKs (that re-wrap + atomicity is the kb-rotate integration, deferred).
5. **`tpm2_rotate` vtable:** performs the custody-KEK rotation (unseal-old → mint new KEK →
   reseal-with-bumped-generation) and reports the custody rotation; `out_principals`/`out_creds`
   remain the kb DEK-re-wrap counts (0 here — the Postgres re-wrap is the kb-rotate integration,
   documented; a naive KEK rotate WITHOUT re-wrap would strand DEKs, so tpm2_rotate here is the
   custody half + the kb layer owns the atomic re-wrap). Decide at review: whether tpm2_rotate
   exposes ONLY `vault_custody_tpm2_reseal` (called by the kb rotate orchestration) vs. a
   self-contained rotate — lean: expose reseal as the primitive; tpm2_rotate stays "unsupported
   standalone; use the kb vault-rotate flow" until the Postgres re-wrap lands, so we never strand
   DEKs.
6. **Fail-closed:** any NV define/read/increment error → the op fails, stays sealed; a generation
   mismatch → refuse; NV_Increment is done BEFORE the new blob is written (so a crash after
   increment but before write leaves the OLD blob un-unsealable = fail-closed-safe, requiring a
   re-provision/recovery — acceptable + documented; the alternative order risks a usable old blob
   at a bumped counter). Full ESAPI/NV handle hygiene.

## Scope (P7-tpm2b)

1. `vault_custody_tpm2.c` (WITH_TPM2): NV counter define/read/increment (idempotent define,
   verified public); the v2 blob layout (KEK||generation) + back-compat v1 read; unseal
   generation-verify; `vault_custody_tpm2_reseal(new_kek, secret)`; wire `tpm2_rotate` to the
   reseal primitive (custody half). Config `vault.tpm2.nv_index`. Stub path: reseal → fail-closed
   "not compiled with TPM2".
2. **Tests**: default-build unit unchanged (stub fail-closed). **CT260 swtpm gate**
   `scripts/p7_tpm2b_swtpm_test.sh` (skips clean without swtpm/WITH_TPM2): provision (gen G0) →
   unseal OK; `reseal(new_kek)` → NV incremented to G1, the NEW blob unseals to new_kek; **RESTORE
   the OLD blob → unseal REFUSED (generation mismatch = rollback detected)**; a second reseal →
   G2, old-G1 blob refused; NV counter is MONOTONIC (reseal only increments; cannot be decremented
   even by restoring an old blob); a v1 (tpm2a) blob loads as generation-current (back-compat).

## Explicitly deferred

The kb-Postgres VAULT-WIDE DEK RE-WRAP under the new KEK (the atomic rotate orchestration that
calls `vault_custody_tpm2_reseal` — a kb-store integration); WORM-audited key-use on
unseal/seal/reseal (P7 §6, needs the kb DB2 audit wiring); PolicyNV TPM-ENFORCED anti-rollback
(the TPM refuses to unseal a stale-generation object via a policy, stronger than the software
generation-compare — a hardening); pkcs11/kms providers; CA-key-behind-vault; use-in-place DEK
crypto. tpm2b is the NV-counter anti-rollback PRIMITIVE.

## Gate

- Default build (no libtss2): server+kb clean, stub fail-closed, existing gates unaffected; new
  files clang-format clean; module-boundary + kb-target-isolation + schema-sync green.
- CT260 `make WITH_TPM2=1 kb` + `scripts/p7_tpm2b_swtpm_test.sh`: the anti-rollback assertions
  (old-blob-refused after reseal, monotonic NV, v1 back-compat) pass on real swtpm. THIS is the
  headline. The tpm2a swtpm gate still passes (no regression).

## Non-goals (P7-tpm2b)

No kb DEK re-wrap, no WORM key-use audit, no PolicyNV, no pkcs11/kms, no CA-key move. Pure TPM2 NV
monotonic-counter anti-rollback for the sealed KEK — generation-bound blob + stale-blob refusal +
a generation-bumping reseal — validated on swtpm.
