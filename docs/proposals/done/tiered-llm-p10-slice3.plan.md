> **DEFERRED (2026-07-20):** This CA-key-in-vault slice is premature. P7 §3 forbids a
> file-custody kb from holding the CA private key (file custody = keyless dev; the CA key
> requires an external anchor). The anchor/seal foundation must land first; this plan is
> revisited as the CA-key-behind-anchor slice afterward. See tiered-llm-p10-slice3b-anchor.plan.md.

# P10/P7 slice 3 implementation plan — kb CA key behind the vault (P7 §7)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Slice 3 of P10+P7. Branch off `testing` (P1, P3a, P10 slice 1, P10/P7 slice 2 merged).
Goal: stop persisting the kb enrollment CA **private** key as plaintext PKCS#8
(`kb/pki.c` `ca-key.pem`, mode 0600) — store it as a vault credential
(`org:pki:ca-key`), decrypt only in-memory at sign time, cleanse after. The CA key is
"the single highest-risk at-rest secret in kb today" (P7 §7). First real consumer of the
slice-2 kb vault.

## Verified ground truth

- CA key today (`src/kb/pki.c`): `kb_pki_ca_save` (:524) writes `ca.pem` (0644, public
  cert) + `ca-key.pem` (0600, **plaintext** PKCS#8 via `pem_from_key` :50, no cipher).
  `kb_pki_ca_load` (:567) reads both. `kb_pki_ca_load_or_create` (:585) loads or
  generates+saves. `kb_pki_ca_t` holds `cert_pem[]` + `key_pem[]` in memory.
- Sign-time (`src/kb/enroll.c`): three call sites — CA fingerprint/bootstrap
  (`kb_pki_ca_load_or_create` :418), client-cert issue (`kb_pki_ca_load` :456 →
  `kb_pki_issue_client_cert` :469), CSR sign (`kb_pki_ca_load` :501 →
  `kb_pki_sign_csr` :513). Each already does `OPENSSL_cleanse(&ca, sizeof(ca))` after
  use. `ca_dir = <data_dir>/kb-ca`.
- Vault (slice 2): `vault_pg_backend` bound at `kb/kb_main.c:779` after `db2_init`.
  Facade `vault_store_*` + custody `vault_server_kek()` (file custody). Slot key
  `org:pki` is NOT `team:<n>` → platform-scoped (team_id NULL) → admin-only RLS.

## Design decisions

1. **kb_vault accessor** — new `src/kb/kb_vault.{c,h}` (kb-only; uses the pg backend +
   file custody). Thin orchestration over the vault facade:
   - `int kb_vault_available(void)` — the **dev gate**: true iff the vault backend is
     bound AND custody is `file` (single-instance dev, P7 §3). A hardened custody
     (`kms`/`tpm2`/… — not implemented until the anchor slice) makes this return false so
     the CA-in-vault path **fails closed** rather than storing a live key under a bare
     file root.
   - `int kb_vault_put(principal, agent, cred, secret)` / `kb_vault_get(principal, agent,
     cred, out, cap)` — obtain the KEK via `vault_server_kek()` (custody), ensure the
     salt + verifier (`vault_store_get_or_create_salt` + `vault_store_unlock_check`,
     first call establishes), then `vault_store_set` / `vault_store_get`. Cleanse the KEK
     after each op. Returns fail-closed on any error.
   - **Principal→tenant enforcement:** slice 3 only stores the platform-scoped CA key
     (`org:pki`), so no tenant check is needed yet; a `team:<n>` principal put/get (P2b)
     will assert the active tenant context matches — stubbed with a TODO + a hard reject
     of a `team:` principal in slice 3 (the CA path never uses one).
2. **CA-key slot:** `principal="org:pki", agent="ca", cred="key"` — AAD
   `org:pki|ca|key|<version>`, platform-scoped (team_id NULL). The **cert** (`ca.pem`,
   public) stays a file; only the **private key** moves into the vault.
3. **pki.c changes (minimal, behavior-preserving for the sign path):**
   - `kb_pki_ca_save`: write `ca.pem` to the file as today; write `key_pem` to the vault
     via `kb_vault_put("org:pki","ca","key", key_pem)` instead of `ca-key.pem`. If
     `kb_vault_available()` is false → fail closed (no plaintext fallback on a box that
     asked for hardening; on a dev box the vault IS available).
   - `kb_pki_ca_load`: read `ca.pem` from the file; read `key_pem` from the vault via
     `kb_vault_get`. Populates the same `kb_pki_ca_t` → the sign functions
     (`key_from_pem(ca->key_pem)`) are UNCHANGED, and the existing `OPENSSL_cleanse(&ca)`
     at each call site already scrubs the in-memory key after use.
   - **Migration:** in `kb_pki_ca_load`/`load_or_create`, if the vault has no CA key but a
     legacy plaintext `ca-key.pem` exists, migrate it into the vault (`kb_vault_put`) and
     `unlink` the plaintext file (P7 §7: stop persisting plaintext). Log the migration.
4. **Bootstrap ordering:** the CA path must not run before the vault bind
   (`kb_main.c:779`). The enroll.c sign/issue sites run on serving requests (vault bound).
   If a startup CA-fingerprint log runs earlier, make CA access **lazy** (first enrollment
   request) or move it after the bind. The implementation verifies no CA load precedes the
   vault bind (grep the startup path); if one does, reorder or lazy-init.
5. **Envelope crypto** unchanged — `vault_crypto` (random DEK, AES-KW wrap under the
   custody KEK, AES-256-GCM with AAD). The CA key ciphertext lands in `org_vault_secret`
   (platform-scoped, admin-only RLS, ciphertext-at-rest). A DB dump yields no usable key.

## Scope (slice 3)

1. `src/kb/kb_vault.{c,h}` — the accessor above; wire its object into the kb link
   (`KB_SRCS`) — it's a `kb/` source, kb-only, allowed by target-isolation.
2. `src/kb/pki.c` — CA key save/load via the vault + legacy-plaintext migration + the
   dev gate. `ca-key.pem` no longer written on new/migrated deployments.
3. Bootstrap-ordering fix if needed.
4. Tests: a real-PG-gated integration test (`test_kb_ca_vault` or extend an enroll test)
   proving: generate CA → key stored in the vault (no plaintext `ca-key.pem` on disk) →
   reload from the vault → **enrollment still issues a valid client cert / signs a CSR**
   end-to-end; legacy-plaintext migration removes the file; the dev gate fails closed
   under a simulated hardened custody. Existing `test_pki` / enroll tests still pass.

## Explicitly deferred (later slices)

External-anchor custody + seal/unseal (§2-3) — until then the CA-in-vault is dev-mode
only; use-in-place (§4) — the CA key is still `key_from_pem`'d in process at sign time
(scoped-access, not memory isolation); mlock (§5); WORM-audited key use (§6); rotation +
anti-rollback (§8). Org vendor keys are P2b.

## Gate

- `make -j kb` + `make -j server` link clean; `make lint` (kb-target-isolation,
  module-boundary) + `make schema-sync-check` green (no schema change this slice — reuses
  slice-2 `org_vault_*`).
- Existing `test_pki` + enroll unit tests pass unchanged; new CA-in-vault integration test
  passes on real PG17 (CT103); a forced-plaintext-scan asserts no CA private-key bytes in
  `ca-key.pem` (absent) after a fresh init.
- Enrollment end-to-end (issue client cert, sign CSR) works with the key sourced from the
  vault.

## Non-goals (slice 3)

Not hardened (file custody dev-only; seal/anchor/use-in-place/WORM/rotation later); no
schema change; the CA cert stays a public file; no change to the enrollment protocol.
