#ifndef DEC_VAULT_CUSTODY_MOCK_H
#define DEC_VAULT_CUSTODY_MOCK_H 1

#include <stdint.h>
#include <pthread.h>
#include "vault_crypto.h"   /* VAULT_KEK_LEN */
#include "vault_internal.h" /* vault_custody_provider_t */

/* vault_custody_mock: a TEST/DEV-ONLY anchor custody provider that exercises the
 * P7 §3 seal/unseal barrier without real hardware (tpm2/pkcs11/kms are deferred).
 *
 * It models an external anchor's Decrypt: the provider boots SEALED (get_kek
 * fails) and only after unseal(secret) does it derive a KEK it will hand out.
 * seal() zeroizes that derived KEK and re-seals. It NEVER rotates.
 *
 * SECURITY: this provider can NEVER hold a live key. kb_vault_live_keys_allowed()
 * requires a REAL anchor (tpm2/pkcs11/kms) AND unsealed — `mock` is excluded by
 * construction, closing "a hardened kb bypasses §3 with vault.custody=mock". */

typedef struct
{
   pthread_mutex_t mu; /* guards sealed/kek_ready/kek so concurrent get_kek/seal/
                        * unseal cannot race (e.g. copy a KEK mid-seal). */
   int sealed;         /* 1 = sealed (get_kek fails); starts 1 */
   int kek_ready;      /* 1 once a KEK has been derived by unseal */
   uint8_t kek[VAULT_KEK_LEN];
} vault_custody_mock_ctx_t;

/* The bind-ready singleton mock provider. Its ctx starts SEALED. Bind it with
 * vault_custody_set_provider(vault_custody_mock_provider()). */
const vault_custody_provider_t *vault_custody_mock_provider(void);

/* Re-seal + zeroize the singleton's derived KEK (test reset / clean re-bind). */
void vault_custody_mock_reset(void);

#endif /* DEC_VAULT_CUSTODY_MOCK_H */
