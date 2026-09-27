#ifndef AIMEE_DB2_ORG_VAULT_ROTATION_H
#define AIMEE_DB2_ORG_VAULT_ROTATION_H 1

#include <stddef.h>
#include <stdint.h>

#define DB2_VAULT_ROTATION_ATTEST_MAX 512
#define DB2_VAULT_ROTATION_REF_MAX    512
#define DB2_VAULT_ROTATION_OWNER_MAX  200
#define DB2_VAULT_ROTATION_SECRET_MAX 4096

typedef struct
{
   int64_t version;
   uint8_t wrapped_dek[40];
   uint8_t nonce[12];
   uint8_t ciphertext[DB2_VAULT_ROTATION_SECRET_MAX];
   size_t ciphertext_len;
   uint8_t tag[16];
} db2_vault_rotation_envelope_t;

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
   char old_vendor_ref[DB2_VAULT_ROTATION_REF_MAX + 1];
   char new_vendor_ref[DB2_VAULT_ROTATION_REF_MAX + 1];
   char revoke_receipt[DB2_VAULT_ROTATION_REF_MAX + 1];
   char failure_phase[33];
   char claim_owner[DB2_VAULT_ROTATION_OWNER_MAX + 1];
   int64_t claim_token;
   char claim_until[64];
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
int db2_vault_rotation_finalize(const char *actor, int64_t rotation_id, const uint8_t *attestation,
                                size_t attestation_len);
int db2_vault_rotation_get(int64_t rotation_id, db2_vault_rotation_row_t *out);
int db2_vault_rotation_claim(const char *actor, int64_t rotation_id, const char *expected,
                             const char *owner, int ttl_seconds, int64_t *token);
int db2_vault_rotation_heartbeat(const char *actor, int64_t rotation_id, const char *owner,
                                 int64_t token, int ttl_seconds);
int db2_vault_rotation_release(const char *actor, int64_t rotation_id, const char *owner,
                               int64_t token);
int db2_vault_rotation_checkpoint_old_ref(const char *actor, int64_t rotation_id, const char *owner,
                                          int64_t token, const char *old_vendor_ref);
int db2_vault_rotation_stage_claimed(const char *actor, int64_t rotation_id, const char *owner,
                                     int64_t token, const char *new_vendor_ref,
                                     const db2_vault_rotation_envelope_t *envelope);
int db2_vault_rotation_probe_admit(const char *actor, int64_t rotation_id, const char *owner,
                                   int64_t token, const char *operation_key,
                                   db2_vault_rotation_envelope_t *envelope);
int db2_vault_rotation_transition_claimed(const char *actor, int64_t rotation_id, const char *owner,
                                          int64_t token, const char *expected, const char *next,
                                          const char *receipt);
int db2_vault_rotation_fail_claimed(const char *actor, int64_t rotation_id, const char *owner,
                                    int64_t token, const char *expected, const char *phase,
                                    const char *error);
int db2_vault_rotation_remediate(const char *actor, int64_t rotation_id, const char *owner,
                                 int64_t token, int64_t anchor_version, const char *evidence);

#endif
