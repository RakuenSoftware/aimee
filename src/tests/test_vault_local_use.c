#define _GNU_SOURCE
#include "vault_server_key.h"
#include "vault_service.h"
#include "vault_internal.h"
#include <assert.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
static atomic_int sealed, seal_entered, seal_done, cleanup_ran, fail_key;
static int get_key(void *ctx, uint8_t *key)
{
   (void)ctx;
   memset(key, 0x63, 32);
   return atomic_load(&fail_key) ? atomic_load(&fail_key) : atomic_load(&sealed) ? -1 : 0;
}
static int is_sealed(void *ctx)
{
   (void)ctx;
   return atomic_load(&sealed);
}
static int seal_key(void *ctx)
{
   (void)ctx;
   atomic_store(&sealed, 1);
   return 0;
}
static int unseal_key(void *ctx, const void *value, size_t n)
{
   (void)ctx;
   (void)value;
   (void)n;
   atomic_store(&sealed, 0);
   return 0;
}
static const vault_custody_provider_t provider = {.name = "local-guard-test",
                                                  .get_kek = get_key,
                                                  .is_sealed = is_sealed,
                                                  .seal = seal_key,
                                                  .unseal = unseal_key};
static void zero(const uint8_t *key)
{
   for (size_t i = 0; i < 32; i++)
      assert(key[i] == 0);
}
static void denied(uint64_t epoch)
{
   uint8_t key[32];
   memset(key, 0xa5, 32);
   assert(vault_local_use_begin(epoch, key) == -1);
   zero(key);
}
static void *seal_thread(void *p)
{
   (void)p;
   atomic_store(&seal_entered, 1);
   assert(vault_seal() == 0);
   atomic_store(&seal_done, 1);
   return NULL;
}
static pthread_t sealer;
static unsigned callbacks;
static uint64_t selected_epoch;
static int use_value(const char *value, size_t n, void *ctx)
{
   (void)ctx;
   assert(n == strlen("local-seal-value") && !memcmp(value, "local-seal-value", n));
   callbacks++;
   denied(selected_epoch);
   uint8_t key[32];
   memset(key, 0xa5, 32);
   assert(vault_use_begin(selected_epoch, 5, key) == -1);
   zero(key);
   vault_use_end(); /* Entry callback must not release its outer seal guard. */
   assert(!pthread_create(&sealer, NULL, seal_thread, NULL));
   while (!atomic_load(&seal_entered))
      sched_yield();
   struct timespec delay = {.tv_nsec = 100000000};
   nanosleep(&delay, NULL);
   assert(!atomic_load(&seal_done));
   return 0;
}
static int ready_pipe[2], resume_pipe[2];
static void scrubbed_cleanup(void *p)
{
   zero(p);
   atomic_store(&cleanup_ran, 1);
}
static void *cancel_worker(void *p)
{
   (void)p;
   uint8_t key[32];
   assert(vault_local_use_begin(vault_use_epoch_snapshot(), key) == 0);
   pthread_cleanup_push(scrubbed_cleanup, key);
   assert(write(ready_pipe[1], "r", 1) == 1);
   char c;
   assert(read(resume_pipe[0], &c, 1) == 1);
   OPENSSL_cleanse(key, sizeof key);
   vault_use_end();
   pthread_testcancel();
   pthread_cleanup_pop(0);
   return NULL;
}
int main(void)
{
   alarm(20);
   char home[512];
   const char *tmp = getenv("TMPDIR");
   if (!tmp)
      tmp = "/tmp";
   assert(snprintf(home, sizeof home, "%s/local-use-XXXXXX", tmp) < (int)sizeof home);
   assert(mkdtemp(home) && !setenv("AIMEE_HOME", home, 1));
   vault_custody_set_provider(&provider);
   assert(aimee_vault_local_use_contract_v1 == 1);
   uint64_t epoch = vault_use_epoch_snapshot();
   assert(epoch && epoch != UINT64_MAX);
   uint8_t key[32];
   memset(key, 0xa5, 32);
   assert(vault_use_begin(epoch, 5, key) == -1);
   zero(key); /* Primary admission remains mandatory. */
   denied(0);
   denied(UINT64_MAX);
   denied(epoch + 1);
   for (int failure = -1; failure <= 1; failure += 2)
   {
      atomic_store(&fail_key, failure);
      denied(epoch);
      int prior;
      assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &prior) &&
             prior == PTHREAD_CANCEL_ENABLE);
      assert(!pthread_setcancelstate(prior, NULL));
   }
   atomic_store(&fail_key, 0);
   assert(vault_local_use_begin(epoch, key) == 0);
   for (size_t i = 0; i < 32; i++)
      assert(key[i] == 0x63);
   denied(epoch);
   vault_maintenance_guard_t *guard = NULL;
   assert(vault_maintenance_guard_begin(&guard) != VAULT_MAINTENANCE_OK && !guard);
   assert(vault_seal() == -1);
   OPENSSL_cleanse(key, sizeof key);
   vault_use_end();
   assert(vault_primary_epoch_initialize(5) == VAULT_MAINTENANCE_OK);
   memset(key, 0xa5, 32);
   assert(vault_use_begin(epoch, 6, key) == -1);
   zero(key);
   assert(vault_use_begin(epoch, 5, key) == 0);
   OPENSSL_cleanse(key, sizeof key);
   vault_use_end();
   char generation[VAULT_IMMUTABLE_GENERATION_CAP], secret[65537] = {0};
   strcpy(secret, "local-seal-value");
   assert(vault_service_create_server_wrap_once("uid:7703", "local-test", "entry", secret,
                                                generation,
                                                sizeof generation) == VAULT_CREATE_ONCE_CREATED);
   OPENSSL_cleanse(secret, sizeof secret);
   selected_epoch = epoch;
   assert(vault_local_use_begin(epoch, key) == 0);
   assert(vault_store_use_server_generation_fenced("uid:7703", key, "local-test", "entry",
                                                   generation, use_value, NULL) == 0);
   assert(callbacks == 1 && !atomic_load(&seal_done));
   OPENSSL_cleanse(key, sizeof key);
   vault_use_end();
   assert(!pthread_join(sealer, NULL) && atomic_load(&seal_done));
   denied(epoch);
   uint64_t fresh = vault_use_epoch_snapshot();
   assert(fresh > epoch);
   denied(fresh);
   assert(!vault_unseal(NULL, 0));
   memset(key, 0xa5, 32);
   assert(vault_use_begin(fresh, 5, key) == -1);
   zero(key);
   assert(vault_local_use_begin(fresh, key) == 0);
   OPENSSL_cleanse(key, sizeof key);
   vault_use_end();
   assert(vault_maintenance_guard_begin(&guard) == VAULT_MAINTENANCE_OK);
   denied(fresh);
   assert(vault_maintenance_guard_end(&guard) == VAULT_MAINTENANCE_OK && !guard);
   assert(!vault_unseal(NULL, 0));
   fresh = vault_use_epoch_snapshot();
   assert(vault_local_use_begin(fresh, key) == 0);
   pid_t child = fork();
   assert(child >= 0);
   if (!child)
   {
      denied(fresh);
      vault_use_end();
      _exit(0);
   }
   int status;
   assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
   OPENSSL_cleanse(key, sizeof key);
   vault_use_end();
   assert(!pipe(ready_pipe) && !pipe(resume_pipe));
   pthread_t worker;
   assert(!pthread_create(&worker, NULL, cancel_worker, NULL));
   char c;
   assert(read(ready_pipe[0], &c, 1) == 1);
   assert(!pthread_cancel(worker));
   assert(write(resume_pipe[1], "x", 1) == 1);
   void *result;
   assert(!pthread_join(worker, &result) && result == PTHREAD_CANCELED &&
          atomic_load(&cleanup_ran));
   assert(vault_seal() == 0 && !vault_unseal(NULL, 0)); /* Cancellation released the shared lock. */
   for (size_t i = 0; i < 2; i++)
   {
      close(ready_pipe[i]);
      close(resume_pipe[i]);
   }
   puts("PASS local seal guard: primary gate unchanged, actual epoch/seal barrier, entry reentry, "
        "maintenance, fork, cancelled-key cleanup");
   return 0;
}
