#include "kb/kb_mgmt_status_custody.h"
#include "kb/kb_vault_protected_use.h"
#include "modules/db2/c/management_status_key.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int g_allowed = 1, g_admit = 1, g_guard_fail, g_guard_open, g_secret_len = 32;
static atomic_int g_protected_active, g_protected_max, g_protected_block, g_protected_entered;

int kb_vault_management_status_keys_allowed(void)
{
   return g_allowed;
}
uint64_t vault_use_epoch_snapshot(void)
{
   return 7;
}
int vault_hwm_read(const char *k, uint64_t *v, uint8_t *a, size_t cap, size_t *n)
{
   (void)k;
   (void)cap;
   *v = 3;
   memcpy(a, "att", 3);
   *n = 3;
   return 0;
}
int vault_hwm_verify(const char *k, uint64_t v, const uint8_t *a, size_t n)
{
   return !k || v != 3 || n != 3 || memcmp(a, "att", 3);
}
int db2_management_status_key_candidate(db2_management_status_key_ctx_t *db, const char *k,
                                        const char *wire, int64_t v,
                                        db2_vault_key_use_envelope_t *e)
{
   if (!db || !k || strcmp(wire, "status-1") || v != 3)
      return -1;
   memset(e, 0, sizeof(*e));
   e->version = 3;
   e->ciphertext_len = 32;
   memcpy(e->hwm_attestation, "att", 3);
   e->hwm_attestation_len = 3;
   return 0;
}
int db2_management_status_key_admit(db2_management_status_key_ctx_t *db,
                                    const db2_management_status_admission_t *p,
                                    db2_vault_key_use_envelope_t *e)
{
   assert(db && p && strlen(p->use_id) == 64 && strlen(p->request_digest) == 64);
   if (g_admit != 1)
      return g_admit;
   memset(e, 0, sizeof(*e));
   e->seal_epoch = 9;
   e->version = 3;
   e->ciphertext_len = 32;
   memcpy(e->hwm_attestation, "att", 3);
   e->hwm_attestation_len = 3;
   return 1;
}
int db2_management_status_key_guard_begin(db2_management_status_key_ctx_t *db, int64_t e)
{
   if (!db || g_guard_fail == 1 || e != 9)
      return -1;
   g_guard_open = 1;
   return 0;
}
int db2_management_status_key_guard_end(db2_management_status_key_ctx_t *db, int commit)
{
   assert(db && g_guard_open);
   g_guard_open = 0;
   return commit && g_guard_fail == 2 ? -1 : 0;
}
kb_vault_key_use_status_t kb_vault_protected_use(uint64_t epoch, const char *p, const char *a,
                                                 const char *c,
                                                 const db2_vault_key_use_envelope_t *e,
                                                 kb_vault_key_use_fn fn, void *ctx)
{
   unsigned char seed[32] = {1};
   int active = atomic_fetch_add(&g_protected_active, 1) + 1;
   int old_max = atomic_load(&g_protected_max);
   while (active > old_max && !atomic_compare_exchange_weak(&g_protected_max, &old_max, active))
      ;
   atomic_store(&g_protected_entered, 1);
   while (atomic_load(&g_protected_block))
      sched_yield();
   assert(epoch == 7 && !strcmp(p, "org:p5-status") && !strcmp(a, "management") &&
          !strcmp(c, "ed25519") && e->seal_epoch == 9 && g_guard_open);
   kb_vault_key_use_status_t rc =
       fn(seed, (size_t)g_secret_len, ctx) ? KB_VAULT_KEY_USE_CALLBACK_FAILED : KB_VAULT_KEY_USE_OK;
   atomic_fetch_sub(&g_protected_active, 1);
   return rc;
}

static kb_mgmt_status_t status(void)
{
   kb_mgmt_status_t s = {0};
   s.version = 1;
   strcpy(s.key_id, "status-1");
   strcpy(s.caller_issuer, "issuer");
   strcpy(s.caller_serial_norm, "01");
   memset(s.caller_fingerprint, 'a', 64);
   s.caller_fingerprint[64] = 0;
   strcpy(s.target_server_id, "server-1");
   memset(s.target_mgmt_fingerprint, 'b', 64);
   s.target_mgmt_fingerprint[64] = 0;
   strcpy(s.purpose, "management.health.v1");
   s.issued_at = 10;
   s.expires_at = 20;
   s.revocation_generation = 2;
   return s;
}

typedef struct
{
   kb_mgmt_status_t status;
   kb_mgmt_status_custody_t *custody;
   int rc;
} thread_call_t;

static void *sign_thread(void *opaque)
{
   thread_call_t *call = opaque;
   call->rc = kb_mgmt_status_custody_sign(&call->status, call->custody);
   return NULL;
}

int main(void)
{
   db2_management_status_key_ctx_t db = {.connection = &db};
   kb_mgmt_status_custody_t c = {.custody_key_id = "platform:p5-status", .database = &db};
   kb_mgmt_status_t s = status();
   assert(kb_mgmt_status_custody_sign(&s, &c) == 0 && !g_guard_open);
   unsigned char zero[64] = {0};
   assert(memcmp(s.signature, zero, 64));
   c.database = NULL;
   s = status();
   assert(kb_mgmt_status_custody_sign(&s, &c) == -1);
   c.database = &db;
   g_allowed = 0;
   s = status();
   assert(kb_mgmt_status_custody_sign(&s, &c) == -1);
   g_allowed = 1;
   g_admit = 0;
   s = status();
   assert(kb_mgmt_status_custody_sign(&s, &c) == KB_MGMT_STATUS_CUSTODY_CONFLICT);
   g_admit = 1;
   g_secret_len = 31;
   s = status();
   assert(kb_mgmt_status_custody_sign(&s, &c) == -1 && !memcmp(s.signature, zero, 64));
   g_secret_len = 32;
   g_guard_fail = 1;
   s = status();
   assert(kb_mgmt_status_custody_sign(&s, &c) == -1 && !g_guard_open);
   g_guard_fail = 2;
   s = status();
   assert(kb_mgmt_status_custody_sign(&s, &c) == -1 && !g_guard_open &&
          !memcmp(s.signature, zero, 64));

   g_guard_fail = 0;
   atomic_store(&g_protected_max, 0);
   thread_call_t calls[2] = {{.status = status(), .custody = &c},
                             {.status = status(), .custody = &c}};
   pthread_t threads[2];
   assert(!pthread_create(&threads[0], NULL, sign_thread, &calls[0]));
   assert(!pthread_create(&threads[1], NULL, sign_thread, &calls[1]));
   assert(!pthread_join(threads[0], NULL));
   assert(!pthread_join(threads[1], NULL));
   assert(calls[0].rc == 0 && calls[1].rc == 0 && atomic_load(&g_protected_max) == 1 &&
          !g_guard_open);

   atomic_store(&g_protected_entered, 0);
   atomic_store(&g_protected_block, 1);
   thread_call_t cancelled = {.status = status(), .custody = &c, .rc = 99};
   pthread_t cancel_thread;
   assert(!pthread_create(&cancel_thread, NULL, sign_thread, &cancelled));
   while (!atomic_load(&g_protected_entered))
      sched_yield();
   assert(!pthread_cancel(cancel_thread));
   atomic_store(&g_protected_block, 0);
   void *cancel_result = NULL;
   assert(!pthread_join(cancel_thread, &cancel_result));
   assert(cancel_result == PTHREAD_CANCELED && !g_guard_open &&
          !memcmp(cancelled.status.signature, zero, 64));

   s = status();
   assert(kb_mgmt_status_custody_sign(&s, &c) == 0 && !g_guard_open &&
          memcmp(s.signature, zero, 64));
   puts("kb_mgmt_status_custody: all tests passed");
   return 0;
}
