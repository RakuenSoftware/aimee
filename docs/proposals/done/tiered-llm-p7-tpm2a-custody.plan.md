# P7-tpm2a plan: real TPM2 custody provider (seal barrier + live-key anchor)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Slice of P7 (hardened kb vault). Branch off `testing`. Implements the FIRST real external-anchor
custody provider (`tpm2`) against libtss2/ESAPI, validated on a software TPM2 (swtpm) in the
integration CT (260). Flips `kb_vault_live_keys_allowed()` from "never" to a genuine anchor-backed
"yes when unsealed". This is the reversal of the "P7 needs hardware" deferral: **swtpm exercises
the identical TPM2 2.0 API/semantics. It fully validates OUR custody code; physical anti-tamper
is a hardware-deployment property, not something the code proves.**

## Verified substrate

- The custody seam (slice 3b, `vault_internal.h`): `vault_custody_provider_t` = {name, ctx,
  `get_kek(ctx,kek[VAULT_KEK_LEN])`, `rotate(...)`, and OPTIONAL seal-barrier
  `is_sealed(ctx)` / `unseal(ctx,params,len)` / `seal(ctx)`}. A NULL seal slot = "always
  unsealed"; when sealed, `get_kek` MUST fail (-1); the kb accessor maps that to
  `VAULT_ERR_SEALED`. Bound via `vault_custody_set_provider(provider)`.
- `kb_vault_policy.c`: `kb_vault_policy_select("tpm2")` currently FAILS CLOSED ("not yet
  implemented"); `kb_vault_live_keys_allowed()` = real-anchor kind ∈ {tpm2,pkcs11,kms} AND
  unsealed → so the gate flips automatically the moment a tpm2 provider is bound + unsealed.
  `custody_is_real_anchor(TPM2)` already true.
- The mock provider (`vault_custody_mock.c`) is the shape to mirror: pthread-mutex-guarded ctx,
  boots SEALED, unseal derives from a secret, seal zeroizes, fail-closed. NEVER live-eligible.
- **libtss2-esys 4.1.3 is on CT260, NOT local/CI** → the provider is BUILD-GUARDED.
- swtpm on CT260 is a PROVEN TPM2 2.0 (startup/createprimary/create work); TCTI
  `swtpm:host=127.0.0.1,port=2321` (tpm2-tools) / the ESAPI mssim/swtpm TCTI for our code.

## Design decisions

1. **Build guard `WITH_TPM2`.** ONE file `vault_custody_tpm2.c`: `#ifdef WITH_TPM2` the real
   ESAPI implementation (links `tss2-esys` via `pkg-config`), `#else` a STUB whose
   `vault_custody_tpm2_provider()` returns a provider whose `get_kek`/`unseal` fail-closed with a
   clear "aimee built without TPM2 support (rebuild WITH_TPM2=1)", so `KB_CUSTODY_TPM2` stays a
   known, fail-closed value on every default build (CI, my box) and becomes REAL only on a
   `WITH_TPM2=1` build (CT260, or any host with libtss2). The default build + all existing gates
   are unaffected (no new link dep).
2. **KEK sealing model (use-at-rest → materialize-on-unseal).** A persistent PRIMARY in the OWNER
   hierarchy (`Esys_CreatePrimary` → `Esys_EvictControl` to a configured persistent handle, e.g.
   `0x81018001`; idempotent, reuse if the handle already holds a primary). The vault KEK is
   SEALED as a `keyedhash` (no-decrypt/no-sign, `sensitiveDataOrigin=0`, sensitive=the KEK)
   `Esys_Create`d under the primary with an **auth-value policy** (`PolicyAuthValue`): the sealed
   object's `authValue` = a KDF of an operator UNSEAL SECRET set at seal time. The sealed blob
   (`TPM2B_PUBLIC` + `TPM2B_PRIVATE`) is persisted to a configured path (`vault.tpm2.blob_path`,
   default `<config>/vault/tpm2-kek.blob`, mode 0600). The blob is USELESS without this TPM (it
   can only be loaded/unsealed under this TPM's primary), so on-disk storage is safe.
3. **Boots SEALED.** `is_sealed` = 1 until a successful `unseal`; `get_kek` fails while sealed.
   `unseal(params,len)`: `params` = the operator unseal secret → `Esys_Load` the blob under the
   primary → start a policy session → `PolicyAuthValue` → set the session's auth to the KDF'd
   secret → `Esys_Unseal` → the recovered KEK is copied into `ctx` (mlock'd if available) and
   `is_sealed`←0. A WRONG secret → the TPM refuses the unseal (policy/auth fail) → stays sealed,
   `unseal` returns -1. `seal(ctx)`: zeroize the cached KEK, `is_sealed`←1 (the blob stays on
   disk). `get_kek`: copy the cached KEK if unsealed, else -1.
4. **First-seal provisioning.** If no blob exists at init, the provider is UNPROVISIONED (sealed,
   get_kek fails, unseal fails "no sealed KEK"). A one-time `vault_custody_tpm2_provision(kek,
   unseal_secret)` (or an `aimee-kb vault tpm2-init`) creates the primary + seals a
   caller-supplied (or freshly random) KEK under the secret and writes the blob. (For P7-tpm2a
   the provision path + the seal/unseal/get_kek/seal barrier are the deliverable; ROTATE with the
   NV anti-rollback counter is P7-tpm2b, `rotate` here returns "unsupported on tpm2 (P7-tpm2b)"
   or does a simple re-seal, decided at review; lean: unsupported+documented, so tpm2b owns the
   anti-rollback semantics cleanly.)
5. **TCTI is configured, defaulting to the real device.** `vault.tpm2.tcti` (default
   `device:/dev/tpmrm0`); CT260/dev sets `swtpm:host=127.0.0.1,port=2321` (or the ESAPI
   `mssim`/`swtpm` TCTI string). Passed to `Esys_Initialize` via `Tss2_TctiLdr_Initialize`.
6. **Fail-closed everywhere.** ANY ESAPI error (init, primary, load, session, unseal) → the op
   fails (-1) and the provider stays sealed; NEVER a plaintext fallback. Sessions/transient
   objects are flushed (`Esys_FlushContext`) on every path incl. errors (no leaked handles; the
   swtpm smoke test's "out of memory for object contexts" is exactly this hazard).

## Scope (P7-tpm2a)

1. `src/modules/vault/vault_custody_tpm2.c` (+ `vault_custody_tpm2.h`): the `#ifdef WITH_TPM2`
   ESAPI provider (persistent primary, seal/unseal/get_kek/seal-barrier, TCTI config, blob
   persist, provision) + the `#else` fail-closed stub. Mirror `vault_custody_mock.c`'s ctx/mutex/
   zeroize discipline; reuse the existing KDF (HKDF/SHA256) for the secret→authValue.
2. `src/kb/kb_vault_policy.c`: `KB_CUSTODY_TPM2` → `vault_custody_set_provider(
   vault_custody_tpm2_provider())` (real or stub). The live-key gate needs NO change (already
   tpm2+unsealed → true).
3. **Makefile**: compile `vault_custody_tpm2.c` into the kb vault objects always (stub costs
   nothing); a `WITH_TPM2=1` switch adds `-DWITH_TPM2 $(shell pkg-config --cflags tss2-esys)` +
   `$(shell pkg-config --libs tss2-esys) -ltss2-tctildr` to the kb link. Config keys
   `vault.tpm2.blob_path` + `vault.tpm2.tcti`. Optional `aimee-kb vault tpm2-init` CLI (or a test
   harness that calls the provision fn directly. Decide at review; lean: a provision fn + the
   test drives it, CLI is a thin follow-on).
4. **Tests**:
   - Default build (no WITH_TPM2): a unit assertion that `kb_vault_policy_select("tpm2")` binds
     the stub and `kb_vault_live_keys_allowed()` stays FALSE / get_kek fails "not compiled". The
     merged code is safe + fail-closed without libtss2 (runs in CI/my box).
   - **CT260 swtpm integration** `scripts/p7_tpm2_swtpm_test.sh` (runs ONLY where WITH_TPM2 +
     swtpm exist; skips cleanly otherwise, like the PG gate): start swtpm, build kb WITH_TPM2=1,
     point the TCTI at swtpm, provision a KEK under a secret; assert (a) boots SEALED, get_kek
     fails, live_keys_allowed FALSE; (b) unseal(correct secret) → get_kek returns the KEK,
     live_keys_allowed TRUE; (c) unseal(WRONG secret) → refused, stays sealed; (d) seal → get_kek
     fails again, live_keys_allowed FALSE; (e) a NEW provider instance re-loads the on-disk blob
     and unseals (persistence); (f) the KEK never appears in the blob file (grep the raw blob for
     the known KEK bytes → absent, it's TPM-sealed).

## Explicitly deferred (P7-tpm2b and later)

`rotate` + the TPM2 NV monotonic-counter ANTI-ROLLBACK (P7 §9); WORM-audited key-use on
unseal/seal/rotate (P7 §6); PCR-policy binding (unseal gated on boot measurements) as an
alternative/addition to the auth-value policy; the pkcs11/kms providers; per-(team,provider) DEK
isolation; moving the kb CA key behind this vault (P7 §7); use-in-place DEK crypto (TPM does the
unwrap without releasing the KEK). P7-tpm2a is the seal-barrier custody core + the live-key gate
on a real anchor.

## Gate

- **Default build** (my box + CI, no libtss2): `make -j server` clean, all existing gates
  unaffected (the stub adds no dep); `make lint` green; the tpm2 stub keeps KB_CUSTODY_TPM2
  fail-closed. `make schema-sync-check` unaffected.
- **CT260** `make WITH_TPM2=1 -j kb` (or `server`) links against tss2-esys; the swtpm integration
  script passes (boots-sealed / unseal / wrong-secret-refused / seal / persistence / KEK-not-in-
  blob). THIS is the headline. A real TPM2 seal barrier + the live-key gate flipping on a real
  anchor.

## Non-goals (P7-tpm2a)

No rotate/anti-rollback (tpm2b), no WORM key-use audit, no PCR policy, no pkcs11/kms, no CA-key
move, no use-in-place DEK crypto, no real-hardware TPM (swtpm is the complete API/semantics
target). Pure `tpm2` seal-barrier custody provider, build-guarded, validated on swtpm, flipping
`kb_vault_live_keys_allowed` on a genuine external anchor.

## v2 refinements (roundtable-converged; TPM2/ESAPI correctness: the crux)

- **Unseal auth = `userWithAuth` object authValue, NOT a PolicyAuthValue session.** The sealed
  keyedhash has `userWithAuth=SET`, `authValue` = the operator UNSEAL SECRET. Unseal is
  `Esys_Load` → `Esys_TR_SetAuth(esys, sealedObj, &authValue)` → `Esys_Unseal(esys, sealedObj,
  <session>, ...)`. (The v1 "set the policy session's auth to the secret" was ESAPI-wrong. The
  authValue is set ON THE OBJECT via Esys_TR_SetAuth; the session enforces the auth. Using
  `userWithAuth` directly is simpler + correct and avoids a PolicyAuthValue-digest mismatch.
  PCR-policy binding stays a documented tpm2b/later option.)
- **Sealed keyedhash template pinned:** `type=KEYEDHASH`, scheme `TPM2_ALG_NULL` (no sign/no
  decrypt, a pure sealed-data object), `objectAttributes = fixedTPM | fixedParent |
  userWithAuth` (NO `sensitiveDataOrigin` — WE supply the KEK as `inSensitive.data`; NO
  `adminWithPolicy`), and **`noDA` CLEAR so TPM dictionary-attack protection is ON**. `authPolicy`
  empty. This forbids migrating the blob to another TPM/parent and enforces authValue-only use.
- **The unseal SESSION is SALTED + response-parameter-ENCRYPTED.** `Esys_Unseal` returns the KEK
  as a TPM response; an unsalted/unencrypted session would expose it in PLAINTEXT over the TCTI
  transport (the swtpm TCP socket / a hardware bus). So unseal uses an **HMAC session salted to a
  transient ECC key** (created in the NULL hierarchy or the primary) with
  `TPMA_SESSION_encrypt` set on the response → the unsealed KEK is transport-encrypted TPM→caller.
  (This is the load-bearing confidentiality control; the on-disk blob being sealed is NOT enough.)
- **The operator secret is a HIGH-ENTROPY credential, not a password.** ≥256-bit, used directly
  as the authValue (an HKDF domain-separation step is fine but is NOT password-hardening and none
  is needed because it is not a human password). DA protection stays ON (bounds any online
  guessing); because the secret is high-entropy, lockout is not an operational risk in practice.
Document the recovery (`tpm2_dictionarylockout` with the lockout auth) for completeness.
- **Persistent primary, VERIFIED idempotency, not blind reuse.** The primary is created from a
  FIXED, deterministic template (Owner hierarchy, ECC P-256 or RSA-2048, fixed attributes, empty
  unique) so its Name is deterministic. On init: `Esys_ReadPublic` the configured persistent
  handle; recompute the expected Name from the fixed template and REQUIRE a match. A mismatched/
  absent/attacker-provisioned object → FAIL CLOSED (never seal/unseal under an unknown parent).
  Provisioning creates the primary + evicts it only if the handle is empty.
- **Provisioning is STRICTLY CREATE-ONCE → no rollback surface in tpm2a.** Provision REFUSES if a
  blob already exists (re-provision requires an explicit destroy). With exactly ONE valid blob
  generation, restoring an "old" blob is a no-op (there is no older generation), so the NV
  monotonic-counter anti-rollback is genuinely only needed once ROTATE (multi-generation) lands
  (tpm2b). This closes the Q6 rollback concern for this slice.
- **Blob persistence is crash-safe + hostile-path-safe:** marshal `TPM2B_PUBLIC` + `TPM2B_PRIVATE`
  (via `Tss2_MU_*`) to a temp file `O_CREAT|O_EXCL|O_NOFOLLOW` 0600, `fsync`, atomic `rename`;
  load bounds-checks the file size + unmarshals defensively (reject truncated/oversized). The blob
  is confidentiality/integrity-protected by the TPM (useless without this TPM's primary), so on-
  disk storage is safe; a persistent-handle (in-TPM NV) alternative is a documented later option.
- **Full ESAPI/TCTI lifecycle:** `Esys_FlushContext` every transient object + session on EVERY
  path incl. errors; `Esys_TR_Close` persistent ESYS_TR references; `Esys_Free` every ESAPI-
  allocated output; finalize order = `Esys_Finalize` BEFORE `Tss2_TctiLdr_Finalize`. (The swtpm
  "out of memory for object contexts" error is precisely a leaked transient handle.)
- **TCTI is exact + linked.** swtpm's `--server type=tcp,port=2321 --ctrl type=tcp,port=2322` is
  the MSSIM protocol → the ESAPI TCTI string is `swtpm:host=127.0.0.1,port=2321` (tss2-tcti-swtpm)
  or `mssim:host=127.0.0.1,port=2321`; init via `Tss2_TctiLdr_Initialize`. The kb link adds
  `-ltss2-tctildr` (+ the tcti plugin is dlopen'd at runtime). The gate verifies the exact string
  the CT's tss2 accepts (do NOT assume mssim/swtpm are interchangeable, probe both, use the one
  that connects).

### Gate additions

- Test order FIXED so fail-closed is actually established: (a) boots SEALED (get_kek fails,
  live_keys FALSE); (b) **wrong-secret unseal → refused, STAYS sealed** (on the still-sealed
  provider, before any success); (c) correct-secret unseal → get_kek OK, live_keys TRUE; (d) seal
  → sealed again; (e) fresh provider instance re-loads the blob + unseals (persistence);
  (f) the KEK bytes never appear in the raw blob file; (g) re-provision while a blob exists →
  REFUSED (create-once); (h) a tampered/truncated blob → load fails closed.
