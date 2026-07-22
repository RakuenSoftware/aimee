#include "kb_vault_protected_use.h"

#include "vault_crypto.h"
#include "vault_server_key.h"

#include <openssl/crypto.h>
#include <pthread.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef struct
{
   unsigned char plaintext[DB2_VAULT_KEY_USE_CIPHERTEXT_MAX];
   unsigned char kek[VAULT_KEK_LEN];
   unsigned char dek[VAULT_DEK_LEN];
   uint8_t aad[VAULT_ENVELOPE_AAD_MAX];
} protected_arena_t;

static protected_arena_t *arena_new(size_t *mapped)
{
#if defined(__linux__) && defined(MADV_DONTDUMP) && defined(MADV_WIPEONFORK)
   long page = sysconf(_SC_PAGESIZE);
   if (page <= 0)
      return NULL;
   size_t n = (sizeof(protected_arena_t) + (size_t)page - 1) & ~((size_t)page - 1);
   void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (p == MAP_FAILED)
      return NULL;
   if (mlock(p, n) || madvise(p, n, MADV_DONTDUMP) || madvise(p, n, MADV_WIPEONFORK))
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

static void arena_free(protected_arena_t *p, size_t n)
{
   if (!p)
      return;
   OPENSSL_cleanse(p, n);
#if defined(__linux__)
   (void)munlock(p, n);
   (void)munmap(p, n);
#endif
}

kb_vault_key_use_status_t kb_vault_protected_use(uint64_t expected_epoch, const char *principal,
                                                 const char *agent, const char *cred,
                                                 const db2_vault_key_use_envelope_t *e,
                                                 kb_vault_key_use_fn callback, void *ctx)
{
   if (!expected_epoch || !principal || !agent || !cred || !e || e->seal_epoch < 1 ||
       e->version < 1 || !e->ciphertext_len || e->ciphertext_len > sizeof(e->ciphertext) ||
       !callback)
      return KB_VAULT_KEY_USE_INTEGRITY;
   size_t mapped = 0;
   protected_arena_t *a = arena_new(&mapped);
   if (!a)
      return KB_VAULT_KEY_USE_RETRY;
   int old_state = PTHREAD_CANCEL_ENABLE;
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state))
   {
      arena_free(a, mapped);
      return KB_VAULT_KEY_USE_RETRY;
   }
   kb_vault_key_use_status_t rc = KB_VAULT_KEY_USE_RETRY;
   int held = 0;
   if (vault_use_begin(expected_epoch, (uint64_t)e->seal_epoch, a->kek))
      rc = vault_is_sealed() ? KB_VAULT_KEY_USE_SEALED : KB_VAULT_KEY_USE_RETRY;
   else
   {
      held = 1;
      size_t aad_len = 0;
      if (vault_aad_build_v2(principal, agent, cred, e->version, a->aad, sizeof(a->aad),
                             &aad_len) ||
          vault_dek_unwrap(a->kek, e->wrapped_dek, a->dek) ||
          (vault_secret_decrypt(a->dek, a->aad, aad_len, e->nonce, e->ciphertext, e->ciphertext_len,
                                e->tag, a->plaintext) &&
           (vault_aad_build_v1_safe(principal, agent, cred, e->version, a->aad, sizeof(a->aad),
                                    &aad_len) ||
            vault_secret_decrypt(a->dek, a->aad, aad_len, e->nonce, e->ciphertext,
                                 e->ciphertext_len, e->tag, a->plaintext))))
         rc = KB_VAULT_KEY_USE_INTEGRITY;
      else
         rc = callback(a->plaintext, e->ciphertext_len, ctx) ? KB_VAULT_KEY_USE_CALLBACK_FAILED
                                                             : KB_VAULT_KEY_USE_OK;
   }
   arena_free(a, mapped);
   if (held)
      vault_use_end();
   (void)pthread_setcancelstate(old_state, NULL);
   return rc;
}
