#ifndef AIMEE_KB_VAULT_PROTECTED_USE_H
#define AIMEE_KB_VAULT_PROTECTED_USE_H

#include "kb_vault_key_use.h"
#include "org_vault_key_use.h"

/* Internal synchronous decrypt/use boundary. Plaintext never leaves callback scope. */
kb_vault_key_use_status_t kb_vault_protected_use(uint64_t expected_local_epoch,
                                                 const char *principal, const char *agent,
                                                 const char *cred,
                                                 const db2_vault_key_use_envelope_t *envelope,
                                                 kb_vault_key_use_fn callback, void *callback_ctx);

/* Use an envelope whose authenticated-data domain is defined by its owner. */
kb_vault_key_use_status_t
kb_vault_protected_use_with_aad(uint64_t expected_local_epoch,
                                const db2_vault_key_use_envelope_t *envelope, const uint8_t *aad,
                                size_t aad_len, kb_vault_key_use_fn callback, void *callback_ctx);

#endif
