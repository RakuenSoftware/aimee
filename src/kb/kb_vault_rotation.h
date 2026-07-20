#ifndef AIMEE_KB_VAULT_ROTATION_H
#define AIMEE_KB_VAULT_ROTATION_H 1

#include <stddef.h>
#include <stdint.h>
#include "kb_identity.h"

enum
{
   KB_VAULT_ROTATION_RETRY_CAS = 1,
   KB_VAULT_ROTATION_FINALIZE = 2,
   KB_VAULT_ROTATION_COMPLETE = 3
};

int kb_vault_rotation_classify(uint64_t anchor_version, uint64_t from_version, uint64_t to_version);
int kb_vault_rotation_start(const kb_principal_t *caller, int64_t team_id, const char *key_id,
                            const char *principal, const char *agent, const char *cred,
                            int64_t from_version, int compromise, int64_t *out_rotation_id);
int kb_vault_rotation_stage(const kb_principal_t *caller, int64_t team_id, int64_t rotation_id,
                            const uint8_t *wrapped_dek, size_t wrapped_dek_len,
                            const uint8_t *nonce, size_t nonce_len, const uint8_t *ciphertext,
                            size_t ciphertext_len, const uint8_t *tag, size_t tag_len);
int kb_vault_rotation_mark_probed(const kb_principal_t *caller, int64_t team_id,
                                  int64_t rotation_id);
int kb_vault_rotation_activate_or_resume(const kb_principal_t *caller, int64_t team_id,
                                         int64_t rotation_id);

#endif
