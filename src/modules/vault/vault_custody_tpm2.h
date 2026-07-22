#ifndef DEC_VAULT_CUSTODY_TPM2_H
#define DEC_VAULT_CUSTODY_TPM2_H 1

#include <stddef.h>
#include <stdint.h>
#include "vault_crypto.h"   /* VAULT_KEK_LEN */
#include "vault_internal.h" /* vault_custody_provider_t */
#include "vault_reseal_receipt.h"

/* vault_custody_tpm2: the FIRST real external-anchor custody provider (P7-tpm2a).
 * It seals the vault's server KEK to a TPM 2.0 under a persistent OWNER-hierarchy
 * primary, so the KEK only materializes after an out-of-band unseal against the
 * TPM. This is the anchor that flips kb_vault_live_keys_allowed() to true (once
 * unsealed) — see kb_vault_policy.c.
 *
 * BUILD-GUARDED. The provider has two implementations in vault_custody_tpm2.c:
 *   - WITH_TPM2 (libtss2/ESAPI): the real seal barrier, validated on swtpm.
 *   - default (no libtss2): a fail-closed STUB — vault_custody_tpm2_provider()
 *     returns a provider that boots SEALED, is_sealed()==1 forever, and
 *     get_kek/unseal fail with "aimee built without TPM2 support". This keeps
 *     KB_CUSTODY_TPM2 a known, fail-closed value on every default build (CI, dev
 *     hosts) while adding no libtss2 link dependency.
 *
 * Config: vault.tpm2.blob_path (sealed-blob file; default
 * <config>/vault/tpm2-kek.blob) and vault.tpm2.tcti (tss2 TCTI string; default
 * "device:/dev/tpmrm0"). Read lazily on first use by the WITH_TPM2 build. */

/* The bind-ready singleton tpm2 provider. Bind it with
 * vault_custody_set_provider(vault_custody_tpm2_provider()). Boots SEALED. */
const vault_custody_provider_t *vault_custody_tpm2_provider(void);

/* One-time CREATE-ONCE provisioning: seal `kek` under the operator `secret` and
 * persist the sealed blob at vault.tpm2.blob_path. REFUSES (-1) if a blob already
 * exists (no rollback surface in tpm2a; re-provision requires an explicit
 * destroy). `secret` is a high-entropy operator credential (NUL-terminated).
 * Returns 0 on success, -1 on any failure (incl. the default stub build, which
 * cannot seal). errbuf (optional) receives a human-readable reason. */
int vault_custody_tpm2_provision(const uint8_t kek[VAULT_KEK_LEN], const char *secret, char *errbuf,
                                 size_t errlen);

/* P7-tpm2b anti-rollback re-seal. Bumps the TPM2 NV monotonic counter to a new
 * generation G', computes a PolicyNV(NV==G') authPolicy, seals `new_kek || be64(G')`
 * under it (userWithAuth CLEAR, object authValue = the operator secret), and
 * ATOMICALLY replaces the on-disk blob (temp+fsync+rename). INCREMENT-BEFORE-WRITE:
 * a crash between the increment and the write leaves the OLD blob bound to a now-stale
 * generation whose PolicyNV the TPM refuses (fail-closed; recovery = re-provision).
 * Mutex-serialized. `secret` is the operator credential gating BOTH the NV counter
 * (its authValue is secret-derived) and the sealed object. Returns 0 on success, -1
 * on any failure (incl. the default stub build, which cannot seal).
 *
 * This is the custody-half PRIMITIVE the kb vault-rotate flow calls AFTER re-wrapping
 * the DEKs under `new_kek`; a standalone custody KEK rotation would strand DEKs, so
 * the tpm2 `rotate` vtable slot itself fails LOUD and defers to this flow. */
int vault_custody_tpm2_reseal(const uint8_t new_kek[VAULT_KEK_LEN], const char *secret);

/* P7-reseal-a: disabled-by-default prepared reseal foundation. The receipt is an
 * in-memory view of the canonical on-disk bundle; it is never written as this C
 * struct. These helpers do not re-wrap Postgres DEKs and therefore are not a
 * complete/operator-safe whole-vault rotation API. */
typedef enum
{
   VAULT_TPM2_RESEAL_ABSENT = 0,
   VAULT_TPM2_RESEAL_PREPARED,
   VAULT_TPM2_RESEAL_NV_ADVANCED,
   VAULT_TPM2_RESEAL_INSTALLED,
   VAULT_TPM2_RESEAL_CLEANED,
   VAULT_TPM2_RESEAL_CONFLICT,
   VAULT_TPM2_RESEAL_CORRUPT,
} vault_tpm2_reseal_status_t;

typedef enum
{
   VAULT_TPM2_RESEAL_OK = 0,
   VAULT_TPM2_RESEAL_ERR = -1,
   VAULT_TPM2_RESEAL_NOT_BUILT = -2,
   VAULT_TPM2_RESEAL_BUSY = -3,
   VAULT_TPM2_RESEAL_INTEGRITY = -4,
} vault_tpm2_reseal_result_t;

typedef enum
{
   VAULT_TPM2_CLEANUP_TERMINAL_COMPLETED = 1,
} vault_tpm2_cleanup_authorization_t;

int vault_custody_tpm2_reseal_prepare(const uint8_t operation_id[16],
                                      uint64_t expected_old_generation,
                                      const uint8_t new_kek[VAULT_KEK_LEN], const char *secret,
                                      vault_tpm2_reseal_receipt_t *out);
int vault_custody_tpm2_reseal_discover(const uint8_t operation_id[16],
                                       uint64_t expected_old_generation, const char *secret,
                                       vault_tpm2_reseal_receipt_t *receipt,
                                       vault_tpm2_reseal_status_t *status);
int vault_custody_tpm2_reseal_recover_kek(const vault_tpm2_reseal_receipt_t *receipt,
                                          const char *secret, uint8_t new_kek[VAULT_KEK_LEN]);
int vault_custody_tpm2_reseal_status(const vault_tpm2_reseal_receipt_t *receipt, const char *secret,
                                     vault_tpm2_reseal_status_t *out);
int vault_custody_tpm2_reseal_commit(const vault_tpm2_reseal_receipt_t *receipt, const char *secret,
                                     vault_tpm2_reseal_status_t *out);
int vault_custody_tpm2_reseal_abort(const vault_tpm2_reseal_receipt_t *receipt, const char *secret);
int vault_custody_tpm2_reseal_cleanup(const vault_tpm2_reseal_receipt_t *receipt,
                                      const char *secret,
                                      vault_tpm2_cleanup_authorization_t authorization);

/* P7-tpm2b introspection: read the current NV monotonic-counter generation (the
 * anti-rollback authority — the value a freshly (re)sealed blob is bound to). Requires
 * the operator `secret` (the NV index is AUTHREAD-gated by a secret-derived authValue,
 * so a wrong/absent secret returns -1). Read-only; does not alter TPM state. Returns 0
 * and *out_gen on success, -1 on any failure (incl. the default stub build). */
int vault_custody_tpm2_nv_generation(const char *secret, uint64_t *out_gen);

/* Re-seal + zeroize the singleton's cached KEK and drop its lazy-init state so a
 * fresh instance re-loads the on-disk blob (test reset / clean re-bind). */
void vault_custody_tpm2_reset(void);

#endif /* DEC_VAULT_CUSTODY_TPM2_H */
