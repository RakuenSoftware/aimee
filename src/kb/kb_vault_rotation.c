#include "kb_vault_rotation.h"

#include "org_vault_rotation.h"
#include "modules/db2/c/db2_tenant.h"
#include "vault_server_key.h"

#include <limits.h>
#include <openssl/crypto.h>
#include <string.h>

int kb_vault_rotation_classify(uint64_t anchor_version, uint64_t from_version, uint64_t to_version)
{
   if (from_version == 0 || from_version == UINT64_MAX || to_version != from_version + 1)
      return -1;
   if (anchor_version == from_version)
      return KB_VAULT_ROTATION_RETRY_CAS;
   if (anchor_version == to_version)
      return KB_VAULT_ROTATION_FINALIZE;
   return -1;
}

static int rotation_scope_begin(const kb_principal_t *caller, int64_t team_id, char actor[576])
{
   if (!caller || kb_identity_key(caller, actor, 576) != 0 ||
       db2_tenant_scope_begin(caller, team_id) != 0)
      return -1;
   return 0;
}

static int rotation_scope_finish(int rc)
{
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      return -1;
   }
   return db2_tenant_scope_commit() == 0 ? 0 : -1;
}

int kb_vault_rotation_start(const kb_principal_t *caller, int64_t team_id, const char *key_id,
                            const char *principal, const char *agent, const char *cred,
                            int64_t from_version, int compromise, int64_t *out_rotation_id)
{
   if (from_version < 1 || from_version == INT64_MAX)
      return -1;
   uint8_t att[DB2_VAULT_ROTATION_ATTEST_MAX];
   size_t att_len = 0;
   uint64_t anchor = 0;
   int rc = vault_hwm_read(key_id, &anchor, att, sizeof(att), &att_len);
   OPENSSL_cleanse(att, sizeof(att));
   if (rc != 0 || anchor != (uint64_t)from_version)
      return -1;
   char actor[576];
   if (rotation_scope_begin(caller, team_id, actor) != 0)
      return -1;
   rc = db2_vault_rotation_start(actor, key_id, principal, team_id > 0, team_id, agent, cred,
                                 from_version, compromise, out_rotation_id);
   return rotation_scope_finish(rc);
}

int kb_vault_rotation_stage(const kb_principal_t *caller, int64_t team_id, int64_t rotation_id,
                            const uint8_t *wrapped_dek, size_t wrapped_dek_len,
                            const uint8_t *nonce, size_t nonce_len, const uint8_t *ciphertext,
                            size_t ciphertext_len, const uint8_t *tag, size_t tag_len)
{
   char actor[576];
   if (rotation_scope_begin(caller, team_id, actor) != 0)
      return -1;
   int64_t version = 0;
   int rc = db2_vault_rotation_stage(actor, rotation_id, wrapped_dek, wrapped_dek_len, nonce,
                                     nonce_len, ciphertext, ciphertext_len, tag, tag_len, &version);
   return rotation_scope_finish(rc);
}

int kb_vault_rotation_mark_probed(const kb_principal_t *caller, int64_t team_id,
                                  int64_t rotation_id)
{
   char actor[576];
   if (rotation_scope_begin(caller, team_id, actor) != 0)
      return -1;
   return rotation_scope_finish(
       db2_vault_rotation_transition(actor, rotation_id, "staged", "probed", ""));
}

int kb_vault_rotation_activate_or_resume(const kb_principal_t *caller, int64_t team_id,
                                         int64_t rotation_id)
{
   db2_vault_rotation_row_t row;
   char actor[576];
   if (rotation_scope_begin(caller, team_id, actor) != 0)
      return -1;
   int rc = db2_vault_rotation_get(rotation_id, &row);
   if (rotation_scope_finish(rc) != 0)
      return -1;
   if (strcmp(row.state, "activated") == 0)
      return KB_VAULT_ROTATION_COMPLETE;
   if (strcmp(row.state, "probed") == 0)
   {
      if (rotation_scope_begin(caller, team_id, actor) != 0)
         return -1;
      rc = db2_vault_rotation_transition(actor, rotation_id, "probed", "activating", "");
      if (rotation_scope_finish(rc) != 0)
         return -1;
      memcpy(row.state, "activating", sizeof("activating"));
   }
   if (strcmp(row.state, "activating") != 0 || row.from_version < 1 || row.to_version < 1)
      return -1;

   uint8_t att[DB2_VAULT_ROTATION_ATTEST_MAX];
   size_t att_len = 0;
   uint64_t anchor = 0;
   if (vault_hwm_read(row.key_id, &anchor, att, sizeof(att), &att_len) != 0)
   {
      OPENSSL_cleanse(att, sizeof(att));
      return -1;
   }
   int action =
       kb_vault_rotation_classify(anchor, (uint64_t)row.from_version, (uint64_t)row.to_version);
   if (action == KB_VAULT_ROTATION_RETRY_CAS)
   {
      OPENSSL_cleanse(att, sizeof(att));
      att_len = 0;
      if (vault_hwm_cas(row.key_id, (uint64_t)row.from_version, (uint64_t)row.to_version, att,
                        sizeof(att), &att_len) != 0)
      {
         OPENSSL_cleanse(att, sizeof(att));
         return -1;
      }
   }
   else if (action != KB_VAULT_ROTATION_FINALIZE)
   {
      OPENSSL_cleanse(att, sizeof(att));
      return -1;
   }
   if (rotation_scope_begin(caller, team_id, actor) != 0)
   {
      OPENSSL_cleanse(att, sizeof(att));
      return -1;
   }
   rc = db2_vault_rotation_finalize(actor, rotation_id, att, att_len);
   OPENSSL_cleanse(att, sizeof(att));
   return rotation_scope_finish(rc) == 0 ? KB_VAULT_ROTATION_COMPLETE : -1;
}
