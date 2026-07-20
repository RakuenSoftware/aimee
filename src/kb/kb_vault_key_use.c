#include "kb_vault_key_use.h"

#include "db2_tenant.h"
#include "kb_vault_policy.h"
#include "org_vault_key_use.h"
#include "vault_crypto.h"
#include "vault_server_key.h"

#include <openssl/crypto.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#define KEY_USE_AAD_MAX 1113

typedef struct
{
   unsigned char plaintext[DB2_VAULT_KEY_USE_CIPHERTEXT_MAX];
   unsigned char kek[VAULT_KEK_LEN];
   unsigned char dek[VAULT_DEK_LEN];
   char aad[KEY_USE_AAD_MAX];
} key_use_arena_t;

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

static key_use_arena_t *arena_new(size_t *mapped)
{
#if defined(__linux__) && defined(MADV_DONTDUMP)
   long page = sysconf(_SC_PAGESIZE);
   if (page <= 0)
      return NULL;
   size_t n = (sizeof(key_use_arena_t) + (size_t)page - 1) & ~((size_t)page - 1);
   void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (p == MAP_FAILED)
      return NULL;
   if (mlock(p, n) != 0 || madvise(p, n, MADV_DONTDUMP) != 0)
   {
      OPENSSL_cleanse(p, n);
      (void)munlock(p, n);
      (void)munmap(p, n);
      return NULL;
   }
   *mapped = n;
   return p;
#else
   (void)mapped;
   return NULL;
#endif
}

static void arena_free(key_use_arena_t *arena, size_t mapped)
{
   if (!arena)
      return;
   OPENSSL_cleanse(arena, mapped);
#if defined(__linux__)
   (void)munlock(arena, mapped);
   (void)munmap(arena, mapped);
#else
   (void)mapped;
#endif
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

static int aad_build(key_use_arena_t *arena, const char *principal, const char *agent,
                     const char *cred, size_t *aad_len)
{
   int n = snprintf(arena->aad, sizeof(arena->aad), "%s|%s|%s", principal, agent, cred);
   if (n < 0 || (size_t)n >= sizeof(arena->aad))
      return -1;
   *aad_len = (size_t)n;
   return 0;
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
       !callback || !kb_vault_live_keys_allowed())
      return KB_VAULT_KEY_USE_INTEGRITY;

   db2_vault_key_use_envelope_t candidate, admitted;
   memset(&candidate, 0, sizeof(candidate));
   memset(&admitted, 0, sizeof(admitted));
   uint8_t fresh_att[DB2_VAULT_KEY_USE_ATTEST_MAX] = {0};
   size_t fresh_att_len = 0;
   uint64_t anchor_version = 0;
   kb_vault_key_use_status_t result = KB_VAULT_KEY_USE_RETRY;

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
   if (rc == -2)
   {
      db2_tenant_scope_rollback();
      result = KB_VAULT_KEY_USE_UNATTESTED;
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
   size_t mapped = 0;
   key_use_arena_t *arena = arena_new(&mapped);
   if (!arena)
      goto done;
   int old_cancel_state = PTHREAD_CANCEL_ENABLE;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state) != 0)
   {
      arena_free(arena, mapped);
      goto done;
   }

   if (scope_begin(caller, team_id, actor) != 0)
      goto cleanup;
   rc =
       db2_vault_key_use_admit(actor, team_id, origin, use_id, key_id, principal, agent, cred,
                               (int64_t)anchor_version, request_digest, provider, model, operation,
                               candidate.hwm_attestation, candidate.hwm_attestation_len, &admitted);
   rc = scope_finish(rc);
   if (rc < 0)
      goto cleanup;
   if (rc == 0)
   {
      result = KB_VAULT_KEY_USE_REPLAY;
      goto cleanup;
   }
   if (admitted.version != (int64_t)anchor_version ||
       admitted.hwm_attestation_len != candidate.hwm_attestation_len ||
       CRYPTO_memcmp(admitted.hwm_attestation, candidate.hwm_attestation,
                     candidate.hwm_attestation_len) != 0)
   {
      result = KB_VAULT_KEY_USE_INTEGRITY;
      goto cleanup;
   }

   if (vault_use_begin(epoch, arena->kek) != 0)
   {
      result = vault_is_sealed() ? KB_VAULT_KEY_USE_SEALED : KB_VAULT_KEY_USE_RETRY;
      goto cleanup;
   }
   int guard_held = 1;
   size_t aad_len = 0;
   if (aad_build(arena, principal, agent, cred, &aad_len) != 0 ||
       vault_dek_unwrap(arena->kek, admitted.wrapped_dek, arena->dek) != 0 ||
       vault_secret_decrypt(arena->dek, (const uint8_t *)arena->aad, aad_len, admitted.nonce,
                            admitted.ciphertext, admitted.ciphertext_len, admitted.tag,
                            arena->plaintext) != 0)
      result = KB_VAULT_KEY_USE_INTEGRITY;
   else
      result = callback(arena->plaintext, admitted.ciphertext_len, callback_ctx) == 0
                   ? KB_VAULT_KEY_USE_OK
                   : KB_VAULT_KEY_USE_CALLBACK_FAILED;
   if (guard_held)
      vault_use_end();

cleanup:
   arena_free(arena, mapped);
   (void)pthread_setcancelstate(old_cancel_state, NULL);
done:
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   OPENSSL_cleanse(&admitted, sizeof(admitted));
   OPENSSL_cleanse(fresh_att, sizeof(fresh_att));
   return result;
}
