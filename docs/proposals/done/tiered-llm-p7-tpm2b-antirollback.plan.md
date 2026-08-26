# P7-tpm2b plan: TPM2 NV monotonic-counter anti-rollback for the sealed KEK (P7 §9)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

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
  `file_rotate` is `.vault`-file-store-coupled, and the kb store is Postgres, so the full
  re-wrap is a kb-integration concern (NOT swtpm-standalone). This slice delivers the custody
  ANTI-ROLLBACK PRIMITIVE the kb rotate flow will call; the Postgres DEK re-wrap stays a
  documented kb-rotate integration.
- swtpm (CT260) supports `Esys_NV_DefineSpace` (TPMA_NV_COUNTER) + `Esys_NV_Increment` +
  `Esys_NV_Read`, a real, monotonic, TPM-backed counter (the same primitive on hardware).

## Design decisions

1. **A TPM2 NV MONOTONIC COUNTER as the anti-rollback authority.** Define ONE NV index (config
   `vault.tpm2.nv_index`, default e.g. `0x01500001`) with `TPMA_NV_COUNTER | TPMA_NV_AUTHREAD |
   TPMA_NV_AUTHWRITE | TPMA_NV_NO_DA | TPMA_NV_POLICY_DELETE?`, owner-authorized, created once at
   first provision (idempotent: if defined with the expected public, reuse; a mismatched
   definition → fail closed). A monotonic counter can ONLY increase (the TPM enforces it) and
   survives power cycles, the trust anchor for "which generation is current".
2. **The sealed blob BINDS its generation.** The sealed data becomes `KEK (VAULT_KEK_LEN) ||
   generation (8 bytes, the NV counter value at seal time)`. A fixed layout inside the
   TPM-sealed sensitive area (so the generation is TPM-integrity-protected, unforgeable without
   this TPM). The on-disk blob format gains a version byte (v2) so a tpm2a (v1, generation-less)
   blob is still loadable + treated as generation 0 (back-compat) OR is rejected (decide at
   review; lean: accept a v1 blob as "generation == current NV" on first tpm2b unseal to avoid
   bricking an existing tpm2a deployment, then it migrates to v2 on the next re-seal).
3. **Unseal VERIFIES the generation (rollback detection).** After `Esys_Unseal`, split the
   recovered data into KEK||gen; `Esys_NV_Read` the counter; if `gen != current_nv` → the blob is
   STALE (an older generation restored after a re-seal) → REFUSE (return -1, stay sealed,
   zeroize). Only a blob whose bound generation equals the live NV counter unseals. (Software
   compares the TPM-sealed generation to the TPM-backed NV counter. An attacker cannot forge the
   sealed generation, and cannot decrement the monotonic NV counter, so an offline old-blob
   restore is caught on the next unseal.)
4. **`vault_custody_tpm2_reseal(new_kek, secret)`, the generation-bumping re-seal.**
   `Esys_NV_Increment` the counter → new generation G'; seal `new_kek || G'` (same template,
   command-encrypted session as tpm2a provision); ATOMICALLY replace the blob (temp+fsync+rename).
   After this, the OLD blob's bound generation G < G' == NV → the old blob no longer unseals
   (anti-rollback). This is the primitive the kb rotate flow calls WITH the new KEK AFTER
   re-wrapping the Postgres DEKs (that re-wrap + atomicity is the kb-rotate integration, deferred).
5. **`tpm2_rotate` vtable:** performs the custody-KEK rotation (unseal-old → mint new KEK →
   reseal-with-bumped-generation) and reports the custody rotation; `out_principals`/`out_creds`
   remain the kb DEK-re-wrap counts (0 here; the Postgres re-wrap is the kb-rotate integration,
   documented; a naive KEK rotate WITHOUT re-wrap would strand DEKs, so tpm2_rotate here is the
   custody half + the kb layer owns the atomic re-wrap). Decide at review: whether tpm2_rotate
   exposes ONLY `vault_custody_tpm2_reseal` (called by the kb rotate orchestration) vs. a
   self-contained rotate, lean: expose reseal as the primitive; tpm2_rotate stays "unsupported
   standalone; use the kb vault-rotate flow" until the Postgres re-wrap lands, so we never strand
   DEKs.
6. **Fail-closed:** any NV define/read/increment error → the op fails, stays sealed; a generation
   mismatch → refuse; NV_Increment is done BEFORE the new blob is written (so a crash after
   increment but before write leaves the OLD blob un-unsealable = fail-closed-safe, requiring a
   re-provision/recovery, acceptable + documented; the alternative order risks a usable old blob
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
calls `vault_custody_tpm2_reseal`. A kb-store integration); WORM-audited key-use on
unseal/seal/reseal (P7 §6, needs the kb DB2 audit wiring); PolicyNV TPM-ENFORCED anti-rollback
(the TPM refuses to unseal a stale-generation object via a policy, stronger than the software
generation-compare. A hardening); pkcs11/kms providers; CA-key-behind-vault; use-in-place DEK
crypto. tpm2b is the NV-counter anti-rollback PRIMITIVE.

## Gate

- Default build (no libtss2): server+kb clean, stub fail-closed, existing gates unaffected; new
  files clang-format clean; module-boundary + kb-target-isolation + schema-sync green.
- CT260 `make WITH_TPM2=1 kb` + `scripts/p7_tpm2b_swtpm_test.sh`: the anti-rollback assertions
  (old-blob-refused after reseal, monotonic NV, v1 back-compat) pass on real swtpm. THIS is the
  headline. The tpm2a swtpm gate still passes (no regression).

## Non-goals (P7-tpm2b)

No kb DEK re-wrap, no WORM key-use audit, no pkcs11/kms, no CA-key move. TPM2 NV monotonic-counter
anti-rollback for the sealed KEK, generation-bound blob (TPM-ENFORCED via PolicyNV) + stale-blob
refusal + a generation-bumping reseal, validated on swtpm.

## v2 refinements (roundtable-converged; TPM-ENFORCED anti-rollback, not a software check)

- **PolicyNV is THE anti-rollback mechanism. The TPM refuses a stale generation, not our
  software.** (The panel converged hard: a software `gen == NV_Read` compare is bypassable by a
  code-exec attacker who patches the comparator / skips the read; only PolicyNV binds the refusal
  to the TPM.) The v2 sealed object uses **`userWithAuth` CLEAR + an `authPolicy`** =
  `Policy(PolicyNV(nvIndex, operandB = generation, offset 0, operation = TPM2_EO_EQ) THEN
  PolicyAuthValue)`. Because `operandB = generation` is folded into the policy DIGEST, EACH
  generation's object has a DISTINCT authPolicy bound to "NV counter == this generation". After a
  reseal increments the counter, an OLD blob's policy asserts `NV == old-gen` which the TPM
  evaluates as FALSE → `Esys_Unseal` FAILS at the TPM (TPM-enforced rollback refusal). The
  operator secret is still required (PolicyAuthValue in the same policy). A software `gen==NV`
  check is ALSO done post-unseal as cheap defence-in-depth, but PolicyNV is the load-bearing
  control.
  - Provision/reseal: compute the authPolicy via a TRIAL policy session (`TPM2_SE_TRIAL`,
    PolicyNV(gen) then PolicyAuthValue, `Esys_PolicyGetDigest`) → set it as the seal template's
    `authPolicy`; `Esys_Create` the KEK||gen object with that authPolicy + userWithAuth CLEAR.
  - Unseal: `Esys_StartAuthSession(TPM2_SE_POLICY)` (salted + response-encrypted as tpm2a) →
    `Esys_PolicyNV(session, nvIndex, nvIndex-auth, operandB=gen, 0, TPM2_EO_EQ)` →
    `Esys_TR_SetAuth(sealedObj, secret)` + `Esys_PolicyAuthValue(session)` →
    `Esys_Unseal(sealedObj, session)`. A stale gen → PolicyNV fails → Unseal refused.
- **NO v1 (tpm2a) back-compat, tpm2b requires a v2 (re-)provision.** (Accepting a
  generation-less v1 blob as "current" REOPENS the rollback hole. A restored v1 blob would be
  accepted forever.) tpm2a shipped hours ago with no production deployment, so dropping v1
  back-compat is clean: the tpm2b provision writes a v2 (PolicyNV, generation-bound) blob; a v1
  blob present → the provider treats it as "must re-provision to v2" (fail-closed, clear message),
  never silently accepts it.
- **NV counter, activation + dedicated auth + collision-safe define.**
  - A freshly `Esys_NV_DefineSpace`'d `TPMA_NV_COUNTER` is INACTIVE until its FIRST
    `Esys_NV_Increment`; provision does that first increment so `NV_Read` returns a usable value.
    (Test asserts the first increment succeeds + read works right after define.)
  - The NV index is authorized by its OWN `authValue` (a KDF of the operator secret), NOT bare
    owner-auth, so a mere TPM-owner-auth holder cannot bump the counter (bumping requires the
    same operator secret that gates unseal; an attacker without it cannot even DoS-advance it).
    Attributes: `TPMA_NV_COUNTER | TPMA_NV_AUTHREAD | TPMA_NV_AUTHWRITE | TPMA_NV_NO_DA` (drop the
    v1 "TPMA_NV_POLICY_DELETE?", irrelevant for a counter).
  - Define is idempotent with a VERIFIED public: if the index already exists with the EXPECTED
    public/attributes, reuse it; if it exists with a DIFFERENT public (a squatting/foreign index)
    → FAIL CLOSED (never use a foreign NV index). `vault.tpm2.nv_index` is configurable so an
    operator can pick a free index.
- **Increment-BEFORE-write ordering (confirmed fail-closed).** reseal does `NV_Increment` (→ G')
  THEN seals+atomically-writes the new blob bound to G'. A crash BETWEEN leaves the OLD blob bound
  to G < G' (its PolicyNV asserts NV==G, now false) → the old blob is un-unsealable = fail-closed
  (recovery = re-provision). Write-then-increment would be UNSAFE (a window where the new blob is
  bound to an already-bumped counter while the old blob still validates). The reseal is
  mutex-serialized.
- **`tpm2_rotate` fails LOUD, never silent.** It returns -1 + a clear errbuf ("tpm2 rotate must go
  through the kb vault-rotate flow which re-wraps DEKs then calls vault_custody_tpm2_reseal. A
  standalone custody KEK rotation would strand DEKs"); `vault_custody_tpm2_reseal(new_kek,secret)`
  is the exposed primitive; the atomic DEK re-wrap orchestration is the deferred kb integration.

### Gate additions (swtpm)

- (i) after provision, the NV counter's first increment succeeded + NV_Read returns the
  generation the blob is bound to;
- (j) **TPM-ENFORCED refusal**: after `reseal`, restoring the OLD blob → `Esys_Unseal` fails at
  the TPM via PolicyNV (not just our software check), verified by confirming the ESAPI return is
  the policy failure, and that even skipping the software gen-compare the unseal still fails;
- (k) a v1 (tpm2a) blob → tpm2b refuses with "re-provision to v2" (no silent accept);
- (l) NV monotonic: two reseals → G0<G1<G2, each prior blob refused; the counter never decrements;
- (m) NV increment REQUIRES the operator-secret-derived NV auth (a wrong/absent auth cannot bump).
