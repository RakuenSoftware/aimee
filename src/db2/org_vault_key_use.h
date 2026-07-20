#ifndef AIMEE_DB2_ORG_VAULT_KEY_USE_H
#define AIMEE_DB2_ORG_VAULT_KEY_USE_H

#include <stddef.h>
#include <stdint.h>

#define DB2_VAULT_KEY_USE_CIPHERTEXT_MAX 4096
#define DB2_VAULT_KEY_USE_ATTEST_MAX     512

typedef struct
{
   int64_t version;
   uint8_t wrapped_dek[40];
   uint8_t nonce[12];
   uint8_t ciphertext[DB2_VAULT_KEY_USE_CIPHERTEXT_MAX];
   size_t ciphertext_len;
   uint8_t tag[16];
   uint8_t hwm_attestation[DB2_VAULT_KEY_USE_ATTEST_MAX];
   size_t hwm_attestation_len;
} db2_vault_key_use_envelope_t;

int db2_vault_key_use_candidate(const char *actor, int64_t team_id, const char *key_id,
                                const char *principal, const char *agent, const char *cred,
                                int64_t version, db2_vault_key_use_envelope_t *out);
/* Candidate returns -2 when no signed exact-current row exists, -1 on backend error. */

/* Returns 1 for a new durable admission, 0 for an exact replay, -1 on error. */
int db2_vault_key_use_admit(const char *actor, int64_t team_id, const char *authenticated_origin,
                            const char *use_id, const char *key_id, const char *principal,
                            const char *agent, const char *cred, int64_t version,
                            const char *request_digest, const char *provider, const char *model,
                            const char *operation, const uint8_t *hwm_attestation,
                            size_t hwm_attestation_len, db2_vault_key_use_envelope_t *out);

#endif
