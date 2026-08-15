#include "kb_vault_key_use.h"

#include "modules/db2/c/db2_tenant.h"
#include "kb_vault_policy.h"
#include "kb_vault_protected_use.h"
#include "org_vault_key_use.h"
#include "vault_server_key.h"

#include <openssl/crypto.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int bounded(const char *s, size_t max, int empty_ok)
{
   return s && (empty_ok || s[0]) && strlen(s) <= max;
}

static int digest_valid(const char *s)
{
   if (!s || strlen(s) != 64)
      return 0;
   for (size_t i = 0; i < 64; i++)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int scope_begin(const kb_principal_t *caller, int64_t team_id, char actor[576])
{
   return caller && kb_identity_key(caller, actor, 576) == 0 &&
                  db2_tenant_scope_begin(caller, team_id) == 0
              ? 0
              : -1;
}

static int scope_finish(int rc)
{
   if (rc < 0)
   {
      db2_tenant_scope_rollback();
      return -1;
   }
   return db2_tenant_scope_commit() == 0 ? rc : -1;
}

kb_vault_key_use_status_t
kb_vault_key_use(const kb_principal_t *caller, int64_t team_id,
                 const kb_principal_t *authenticated_origin, const char *use_id, const char *key_id,
                 const char *principal, const char *agent, const char *cred,
                 const char *request_digest, const char *provider, const char *model,
                 const char *operation, kb_vault_key_use_fn callback, void *callback_ctx)
{
   char actor[576], origin[576];
   if (!caller || !authenticated_origin || team_id < 1 ||
       kb_identity_key(authenticated_origin, origin, sizeof(origin)) != 0 ||
       !bounded(use_id, 200, 0) || !bounded(key_id, 600, 0) || !bounded(principal, 600, 0) ||
       !bounded(agent, 255, 1) || !bounded(cred, 255, 1) || !digest_valid(request_digest) ||
       !bounded(provider, 64, 0) || !bounded(model, 255, 0) || !bounded(operation, 64, 0) ||
       !callback)
      return KB_VAULT_KEY_USE_INTEGRITY;
   if (!kb_vault_live_keys_allowed())
      return KB_VAULT_KEY_USE_SEALED;

   db2_vault_key_use_envelope_t candidate, admitted;
   memset(&candidate, 0, sizeof(candidate));
   memset(&admitted, 0, sizeof(admitted));
   uint8_t fresh_att[DB2_VAULT_KEY_USE_ATTEST_MAX] = {0};
   size_t fresh_att_len = 0;
   uint64_t anchor_version = 0;
   kb_vault_key_use_status_t result = KB_VAULT_KEY_USE_RETRY;
   int old_cancel_state = PTHREAD_CANCEL_ENABLE;
   int cancel_disabled = 0;

   if (vault_hwm_read(key_id, &anchor_version, fresh_att, sizeof(fresh_att), &fresh_att_len) != 0)
      goto done;
   if (anchor_version == 0 || anchor_version > INT64_MAX ||
       vault_hwm_verify(key_id, anchor_version, fresh_att, fresh_att_len) != 0)
   {
      result = KB_VAULT_KEY_USE_UNATTESTED;
      goto done;
   }

   if (scope_begin(caller, team_id, actor) != 0)
      goto done;
   int rc = db2_vault_key_use_candidate(actor, team_id, key_id, principal, agent, cred,
                                        (int64_t)anchor_version, &candidate);
   if (rc == DB2_VAULT_KEY_USE_MISSING)
   {
      db2_tenant_scope_rollback();
      result = KB_VAULT_KEY_USE_UNATTESTED;
      goto done;
   }
   if (rc == DB2_VAULT_KEY_USE_INTEGRITY)
   {
      db2_tenant_scope_rollback();
      result = KB_VAULT_KEY_USE_INTEGRITY;
      goto done;
   }
   if (scope_finish(rc) < 0)
      goto done;
   if (candidate.version != (int64_t)anchor_version ||
       vault_hwm_verify(key_id, anchor_version, candidate.hwm_attestation,
                        candidate.hwm_attestation_len) != 0)
   {
      result = KB_VAULT_KEY_USE_UNATTESTED;
      goto done;
   }

   uint64_t epoch = vault_use_epoch_snapshot();
   cancel_disabled = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state) == 0;
   if (!cancel_disabled)
      goto done;

   if (scope_begin(caller, team_id, actor) != 0)
      goto done;
   rc =
       db2_vault_key_use_admit(actor, team_id, origin, use_id, key_id, principal, agent, cred,
                               (int64_t)anchor_version, request_digest, provider, model, operation,
                               candidate.hwm_attestation, candidate.hwm_attestation_len, &admitted);
   if (rc == DB2_VAULT_KEY_USE_INTEGRITY)
   {
      db2_tenant_scope_rollback();
      result = KB_VAULT_KEY_USE_INTEGRITY;
      goto done;
   }
   if (rc == DB2_VAULT_KEY_USE_SEALED)
   {
      db2_tenant_scope_rollback();
      result = KB_VAULT_KEY_USE_SEALED;
      goto done;
   }
   rc = scope_finish(rc);
   if (rc < 0)
      goto done;
   if (admitted.seal_epoch < 1)
   {
      result = KB_VAULT_KEY_USE_INTEGRITY;
      goto done;
   }
   if (rc == 0)
   {
      result = KB_VAULT_KEY_USE_REPLAY;
      goto done;
   }
   if (admitted.version != (int64_t)anchor_version ||
       admitted.hwm_attestation_len != candidate.hwm_attestation_len ||
       CRYPTO_memcmp(admitted.hwm_attestation, candidate.hwm_attestation,
                     candidate.hwm_attestation_len) != 0)
   {
      result = KB_VAULT_KEY_USE_INTEGRITY;
      goto done;
   }
   result =
       kb_vault_protected_use(epoch, principal, agent, cred, &admitted, callback, callback_ctx);
done:
   if (cancel_disabled)
      (void)pthread_setcancelstate(old_cancel_state, NULL);
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   OPENSSL_cleanse(&admitted, sizeof(admitted));
   OPENSSL_cleanse(fresh_att, sizeof(fresh_att));
   return result;
}
