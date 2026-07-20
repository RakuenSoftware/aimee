#ifndef AIMEE_DB2_ORG_VAULT_ROTATION_H
#define AIMEE_DB2_ORG_VAULT_ROTATION_H 1

#include <stddef.h>
#include <stdint.h>

#define DB2_VAULT_ROTATION_ATTEST_MAX 512

typedef struct
{
   int64_t id;
   int64_t team_id;
   int has_team;
   int64_t from_version;
   int64_t to_version;
   int compromise;
   char key_id[601];
   char principal[601];
   char agent[256];
   char cred[256];
   char state[16];
   char last_error[1001];
   uint8_t hwm_attestation[DB2_VAULT_ROTATION_ATTEST_MAX];
   size_t hwm_attestation_len;
} db2_vault_rotation_row_t;

int db2_vault_rotation_start(const char *actor, const char *key_id, const char *principal,
                             int has_team, int64_t team_id, const char *agent, const char *cred,
                             int64_t from_version, int compromise, int64_t *out_id);
int db2_vault_rotation_stage(const char *actor, int64_t rotation_id, const uint8_t *wrapped_dek,
                             size_t wrapped_dek_len, const uint8_t *nonce, size_t nonce_len,
                             const uint8_t *ciphertext, size_t ciphertext_len, const uint8_t *tag,
                             size_t tag_len, int64_t *out_version);
int db2_vault_rotation_transition(const char *actor, int64_t rotation_id, const char *expected,
                                  const char *next, const char *error);
int db2_vault_rotation_finalize(const char *actor, int64_t rotation_id,
                                const uint8_t *attestation, size_t attestation_len);
int db2_vault_rotation_get(int64_t rotation_id, db2_vault_rotation_row_t *out);

#endif
