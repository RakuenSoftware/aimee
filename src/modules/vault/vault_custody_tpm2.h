#ifndef DEC_VAULT_CUSTODY_TPM2_H
#define DEC_VAULT_CUSTODY_TPM2_H 1

#include <stddef.h>
#include <stdint.h>
#include "vault_crypto.h"   /* VAULT_KEK_LEN */
#include "vault_internal.h" /* vault_custody_provider_t */

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

/* Re-seal + zeroize the singleton's cached KEK and drop its lazy-init state so a
 * fresh instance re-loads the on-disk blob (test reset / clean re-bind). */
void vault_custody_tpm2_reset(void);

#endif /* DEC_VAULT_CUSTODY_TPM2_H */
