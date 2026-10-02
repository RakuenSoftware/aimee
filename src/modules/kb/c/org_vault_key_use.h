#ifndef AIMEE_KB_STORE_ORG_VAULT_KEY_USE_H
#define AIMEE_KB_STORE_ORG_VAULT_KEY_USE_H

#include <stddef.h>
#include <stdint.h>

#define KB_STORE_VAULT_KEY_USE_CIPHERTEXT_MAX 4096
#define KB_STORE_VAULT_KEY_USE_ATTEST_MAX     512
#define KB_STORE_VAULT_KEY_USE_ERROR          -1
#define KB_STORE_VAULT_KEY_USE_MISSING        -2
#define KB_STORE_VAULT_KEY_USE_INTEGRITY      -3
#define KB_STORE_VAULT_KEY_USE_SEALED         -4

typedef struct
{
   int64_t seal_epoch;
   int64_t version;
   uint8_t wrapped_dek[40];
   uint8_t nonce[12];
   uint8_t ciphertext[KB_STORE_VAULT_KEY_USE_CIPHERTEXT_MAX];
   size_t ciphertext_len;
   uint8_t tag[16];
   uint8_t hwm_attestation[KB_STORE_VAULT_KEY_USE_ATTEST_MAX];
   size_t hwm_attestation_len;
} kb_store_vault_key_use_envelope_t;

/* Begin a startup transaction, hold the primary barrier shared, and read the
 * authoritative epoch including while sealed. End must be called exactly once
 * after every successful begin; outputs are zeroed on failure. */
int kb_store_vault_control_startup_begin(int64_t *epoch_out, int *sealed_out);
int kb_store_vault_control_startup_end(int commit);

int kb_store_vault_key_use_candidate(const char *actor, int64_t team_id, const char *key_id,
                                     const char *principal, const char *agent, const char *cred,
                                     int64_t version, kb_store_vault_key_use_envelope_t *out);
/* Candidate returns MISSING when no signed exact-current row exists. */

/* Returns 1 for a new durable admission, 0 for an exact replay, or a typed
 * negative result. out->seal_epoch is populated for both success cases. */
int kb_store_vault_key_use_admit(const char *actor, int64_t team_id,
                                 const char *authenticated_origin, const char *use_id,
                                 const char *key_id, const char *principal, const char *agent,
                                 const char *cred, int64_t version, const char *request_digest,
                                 const char *provider, const char *model, const char *operation,
                                 const uint8_t *hwm_attestation, size_t hwm_attestation_len,
                                 kb_store_vault_key_use_envelope_t *out);

#endif
