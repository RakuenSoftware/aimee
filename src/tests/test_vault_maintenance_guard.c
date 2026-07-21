#include "modules/vault/vault_internal.h"
#include "modules/vault/vault_server_key.h"

#include <assert.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct
{
   int sealed;
   int seal_fail;
   int seal_calls;
   int unseal_calls;
   int get_calls;
} mock_custody_t;

static mock_custody_t g_mock;

static int mock_get_kek(void *ctx, uint8_t kek[VAULT_KEK_LEN])
{
   mock_custody_t *m = ctx;
   m->get_calls++;
   if (m->sealed)
      return -1;
   memset(kek, 0x6b, VAULT_KEK_LEN);
   return 0;
}

static int mock_is_sealed(void *ctx)
{
   return ((mock_custody_t *)ctx)->sealed;
}

static int mock_unseal(void *ctx, const void *params, size_t len)
{
   mock_custody_t *m = ctx;
   if (!params || len != 4 || memcmp(params, "open", 4) != 0)
      return -1;
   m->unseal_calls++;
   m->sealed = 0;
   return 0;
}

static int mock_seal(void *ctx)
{
   mock_custody_t *m = ctx;
   m->seal_calls++;
   m->sealed = 1;
   return m->seal_fail ? -1 : 0;
}

static const vault_custody_provider_t g_provider = {
    .name = "maintenance-mock",
    .ctx = &g_mock,
    .get_kek = mock_get_kek,
    .is_sealed = mock_is_sealed,
    .unseal = mock_unseal,
    .seal = mock_seal,
};

static void bind_sealed(void)
{
   memset(&g_mock, 0, sizeof(g_mock));
   g_mock.sealed = 1;
   vault_custody_set_provider(&g_provider);
}

static int check_kek(const uint8_t kek[VAULT_KEK_LEN], void *ctx)
{
   int old = -1;
   assert(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old) == 0);
   assert(old == PTHREAD_CANCEL_DISABLE);
   assert(pthread_setcancelstate(old, NULL) == 0);
   assert(kek[0] == 0x6b && kek[VAULT_KEK_LEN - 1] == 0x6b);
   (*(int *)ctx)++;
   return 23;
}

static void test_primary_epoch_and_operational_use(void)
{
   bind_sealed();
   assert(vault_primary_epoch_initialize(0) == VAULT_MAINTENANCE_INVALID);
   assert(vault_primary_epoch_initialize((uint64_t)INT64_MAX + 1) == VAULT_MAINTENANCE_INVALID);
   assert(vault_primary_epoch_initialize(7) == VAULT_MAINTENANCE_OK);
   assert(vault_primary_epoch_initialize(7) == VAULT_MAINTENANCE_OK);
   assert(vault_primary_epoch_initialize(8) == VAULT_MAINTENANCE_EPOCH);

   uint64_t local = vault_use_epoch_snapshot();
   assert(vault_unseal("open", 4) == 0);
   uint8_t kek[VAULT_KEK_LEN];
   memset(kek, 0xa5, sizeof(kek));
   assert(vault_use_begin(local, 8, kek) == -1);
   for (size_t i = 0; i < sizeof(kek); i++)
      assert(kek[i] == 0);
   assert(vault_use_begin(local, 7, kek) == 0);
   assert(kek[0] == 0x6b);
   vault_use_end();

   assert(vault_seal() == 0);
   assert(vault_unseal("open", 4) == 0);
   memset(kek, 0xa5, sizeof(kek));
   assert(vault_use_begin(vault_use_epoch_snapshot(), 7, kek) == -1);
   for (size_t i = 0; i < sizeof(kek); i++)
      assert(kek[i] == 0);
   assert(vault_seal() == 0);
   puts("  PASS: primary epoch + atomic operational admission");
}

typedef struct
{
   vault_maintenance_guard_t *guard;
   int rc;
} wrong_owner_arg_t;

static void *wrong_owner(void *opaque)
{
   wrong_owner_arg_t *a = opaque;
   a->rc = vault_maintenance_guard_seal(a->guard);
   return NULL;
}

static void test_guard_identity_kek_and_cleanup(void)
{
   bind_sealed();
   vault_maintenance_guard_t *guard = NULL;
   assert(vault_maintenance_guard_begin(&guard) == VAULT_MAINTENANCE_OK);
   vault_maintenance_guard_t *recursive = NULL;
   assert(vault_maintenance_guard_begin(&recursive) == VAULT_MAINTENANCE_BUSY);
   assert(vault_primary_epoch_initialize(10) == VAULT_MAINTENANCE_BUSY);
   assert(vault_maintenance_guard_sync_primary_epoch(guard, 10) == VAULT_MAINTENANCE_OK);
   assert(vault_maintenance_guard_sync_primary_epoch(guard, 10) == VAULT_MAINTENANCE_OK);
   assert(vault_maintenance_guard_sync_primary_epoch(guard, 9) == VAULT_MAINTENANCE_EPOCH);
   assert(vault_maintenance_guard_unseal(guard, "open", 4) == 0);

   int callbacks = 0;
   assert(vault_maintenance_guard_with_active_kek(guard, check_kek, &callbacks) == 23);
   assert(callbacks == 1 && g_mock.get_calls == 1);

   wrong_owner_arg_t arg = {.guard = guard, .rc = 0};
   pthread_t thread;
   assert(pthread_create(&thread, NULL, wrong_owner, &arg) == 0);
   assert(pthread_join(thread, NULL) == 0);
   assert(arg.rc == VAULT_MAINTENANCE_WRONG_OWNER && !g_mock.sealed);

   vault_maintenance_guard_t *stale = guard;
   assert(vault_maintenance_guard_end(&guard) == VAULT_MAINTENANCE_OK);
   assert(guard == NULL && g_mock.sealed);
   assert(vault_maintenance_guard_seal(stale) == VAULT_MAINTENANCE_INVALID);
   assert(vault_maintenance_guard_seal((vault_maintenance_guard_t *)(uintptr_t)0x12345) ==
          VAULT_MAINTENANCE_INVALID);

   int prior = -1;
   assert(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &prior) == 0);
   assert(prior == PTHREAD_CANCEL_ENABLE);
   assert(vault_maintenance_guard_begin(&guard) == VAULT_MAINTENANCE_OK);
   assert(vault_maintenance_guard_end(&guard) == VAULT_MAINTENANCE_OK);
   int observed = -1;
   assert(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &observed) == 0);
   assert(observed == PTHREAD_CANCEL_DISABLE);
   assert(pthread_setcancelstate(prior, NULL) == 0);
   puts("  PASS: guard identity + callback-scoped KEK + owner cleanup");
}

typedef struct
{
   pthread_mutex_t mu;
   pthread_cond_t cv;
   int started;
   atomic_int acquired;
   vault_maintenance_guard_t *guard;
   int rc;
} blocked_begin_arg_t;

static void *blocked_begin(void *opaque)
{
   blocked_begin_arg_t *a = opaque;
   pthread_mutex_lock(&a->mu);
   a->started = 1;
   pthread_cond_signal(&a->cv);
   pthread_mutex_unlock(&a->mu);
   a->rc = vault_maintenance_guard_begin(&a->guard);
   atomic_store(&a->acquired, 1);
   if (a->rc == VAULT_MAINTENANCE_OK)
      assert(vault_maintenance_guard_end(&a->guard) == VAULT_MAINTENANCE_OK);
   return NULL;
}

static void test_cross_thread_blocks_and_seal_failure_releases(void)
{
   bind_sealed();
   vault_maintenance_guard_t *guard = NULL;
   assert(vault_maintenance_guard_begin(&guard) == VAULT_MAINTENANCE_OK);
   blocked_begin_arg_t arg = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
   pthread_t thread;
   assert(pthread_create(&thread, NULL, blocked_begin, &arg) == 0);
   pthread_mutex_lock(&arg.mu);
   while (!arg.started)
      pthread_cond_wait(&arg.cv, &arg.mu);
   pthread_mutex_unlock(&arg.mu);
   assert(!atomic_load(&arg.acquired));
   assert(vault_maintenance_guard_end(&guard) == VAULT_MAINTENANCE_OK);
   assert(pthread_join(thread, NULL) == 0);
   assert(arg.rc == VAULT_MAINTENANCE_OK && atomic_load(&arg.acquired));
   pthread_cond_destroy(&arg.cv);
   pthread_mutex_destroy(&arg.mu);

   assert(vault_maintenance_guard_begin(&guard) == VAULT_MAINTENANCE_OK);
   g_mock.seal_fail = 1;
   assert(vault_maintenance_guard_end(&guard) == VAULT_MAINTENANCE_ERROR);
   assert(guard == NULL && g_mock.sealed);
   g_mock.seal_fail = 0;
   assert(vault_maintenance_guard_begin(&guard) == VAULT_MAINTENANCE_OK);
   assert(vault_maintenance_guard_end(&guard) == VAULT_MAINTENANCE_OK);
   puts("  PASS: cross-thread exclusion + seal-failure release");
}

static void test_fork_child_is_fail_closed(void)
{
   bind_sealed();
   assert(vault_primary_epoch_initialize(31) == VAULT_MAINTENANCE_OK);
   vault_maintenance_guard_t *guard = NULL;
   assert(vault_maintenance_guard_begin(&guard) == VAULT_MAINTENANCE_OK);
   vault_maintenance_guard_t *inherited = guard;
   pid_t pid = fork();
   assert(pid >= 0);
   if (pid == 0)
   {
      uint8_t kek[VAULT_KEK_LEN];
      int stale_rc = vault_maintenance_guard_seal(inherited);
      int use_rc = vault_use_begin(vault_use_epoch_snapshot(), 31, kek);
      _exit(stale_rc == VAULT_MAINTENANCE_INVALID && use_rc != 0 ? 0 : 1);
   }
   int status = 0;
   assert(waitpid(pid, &status, 0) == pid);
   assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   assert(vault_maintenance_guard_end(&guard) == VAULT_MAINTENANCE_OK);
   puts("  PASS: fork child invalidates inherited authority");
}

int main(void)
{
   test_primary_epoch_and_operational_use();
   test_guard_identity_kek_and_cleanup();
   test_cross_thread_blocks_and_seal_failure_releases();
   test_fork_child_is_fail_closed();
   vault_custody_set_provider(NULL);
   puts("vault_maintenance_guard: all tests passed");
   return 0;
}
