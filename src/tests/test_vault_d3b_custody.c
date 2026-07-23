#include "modules/vault/vault_internal.h"
#include "modules/vault/vault_custody_tpm2.h"
#include "modules/vault/vault_server_key.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
   int sealed;
   int get_fail;
   int seal_calls;
   int preflight_calls;
   uint64_t generation;
} typed_mock_t;

static typed_mock_t g_mock;

static int mock_get(void *opaque, uint8_t kek[VAULT_KEK_LEN])
{
   typed_mock_t *m = opaque;
   if (m->sealed || m->get_fail)
      return -1;
   memset(kek, 0x73, VAULT_KEK_LEN);
   return 0;
}

static int mock_is_sealed(void *opaque)
{
   return ((typed_mock_t *)opaque)->sealed;
}

static int mock_unseal(void *opaque, const void *secret, size_t secret_len)
{
   typed_mock_t *m = opaque;
   if (!secret || secret_len != 2 || memcmp(secret, "ok", 2) != 0)
      return -1;
   m->sealed = 0;
   return 0;
}

static int mock_seal(void *opaque)
{
   typed_mock_t *m = opaque;
   m->sealed = 1;
   m->seal_calls++;
   return 0;
}

static int mock_preflight(void *opaque, const void *secret, size_t secret_len,
                          uint64_t expected_generation)
{
   typed_mock_t *m = opaque;
   m->preflight_calls++;
   if (!secret || secret_len != 2 || memcmp(secret, "ok", 2) != 0)
      return VAULT_CUSTODY_AUTH_WRONG_SECRET;
   return expected_generation == m->generation ? VAULT_CUSTODY_AUTHORIZED
                                               : VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
}

static int mock_unseal_typed(void *opaque, const void *secret, size_t secret_len)
{
   typed_mock_t *m = opaque;
   if (!secret || secret_len != 2 || memcmp(secret, "ok", 2) != 0)
      return VAULT_CUSTODY_AUTH_WRONG_SECRET;
   m->sealed = 0;
   return VAULT_CUSTODY_AUTHORIZED;
}

static int mock_preflight_current(void *opaque, const void *secret, size_t secret_len,
                                  uint64_t *generation)
{
   typed_mock_t *m = opaque;
   if (generation)
      *generation = UINT64_MAX; /* facade must clear this on every refusal */
   if (!secret || secret_len != 2)
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   if (memcmp(secret, "no", 2) == 0)
      return VAULT_CUSTODY_AUTH_WRONG_SECRET;
   if (memcmp(secret, "be", 2) == 0)
      return VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE;
   if (memcmp(secret, "in", 2) == 0)
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   if (memcmp(secret, "ok", 2) != 0 || !generation)
      return VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE;
   *generation = m->generation;
   return VAULT_CUSTODY_AUTHORIZED;
}

static const vault_custody_provider_t provider = {
    .name = "d3b-typed-mock",
    .ctx = &g_mock,
    .get_kek = mock_get,
    .is_sealed = mock_is_sealed,
    .unseal = mock_unseal,
    .seal = mock_seal,
    .authorization_preflight = mock_preflight,
    .typed_unseal = mock_unseal_typed,
    .authorization_preflight_current = mock_preflight_current,
};

static void bind_mock(uint64_t generation)
{
   memset(&g_mock, 0, sizeof(g_mock));
   g_mock.sealed = 1;
   g_mock.generation = generation;
   vault_custody_set_provider(&provider);
}

static void test_closed_preflight(void)
{
   bind_mock(7);
   uint64_t generation = UINT64_MAX;
   assert(vault_custody_selected_authorization_preflight_current(NULL, 0, &generation) ==
              VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE &&
          generation == 0);
   generation = UINT64_MAX;
   assert(vault_custody_selected_authorization_preflight_current("no", 2, &generation) ==
              VAULT_CUSTODY_AUTH_WRONG_SECRET &&
          generation == 0);
   generation = UINT64_MAX;
   assert(vault_custody_selected_authorization_preflight_current("be", 2, &generation) ==
              VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE &&
          generation == 0);
   generation = UINT64_MAX;
   assert(vault_custody_selected_authorization_preflight_current("in", 2, &generation) ==
              VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE &&
          generation == 0);
   generation = UINT64_MAX;
   assert(vault_custody_selected_authorization_preflight_current("ok", 2, &generation) ==
              VAULT_CUSTODY_AUTHORIZED &&
          generation == 7);
   assert(vault_custody_selected_authorization_preflight_current("ok", 2, NULL) ==
          VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE);
   assert(vault_custody_selected_authorization_preflight("ok", 2, 0) ==
          VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE);
   assert(g_mock.preflight_calls == 0);
   assert(vault_custody_selected_authorization_preflight("no", 2, 7) ==
          VAULT_CUSTODY_AUTH_WRONG_SECRET);
   assert(vault_custody_selected_authorization_preflight("ok", 2, 8) ==
          VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE);
   assert(vault_custody_selected_authorization_preflight("ok", 2, 7) == VAULT_CUSTODY_AUTHORIZED);
   assert(g_mock.sealed && g_mock.preflight_calls == 3);
}

static void test_typed_unseal_and_operational_finish(void)
{
   bind_mock(11);
   vault_maintenance_guard_t *guard = NULL;
   assert(vault_maintenance_guard_begin(&guard) == VAULT_MAINTENANCE_OK);
   assert(vault_maintenance_guard_sync_primary_epoch(guard, 41) == VAULT_MAINTENANCE_OK);
   assert(vault_maintenance_guard_unseal_typed(guard, "no", 2) == VAULT_CUSTODY_AUTH_WRONG_SECRET);
   assert(g_mock.sealed);
   assert(vault_maintenance_guard_unseal_typed(guard, "ok", 2) == VAULT_CUSTODY_AUTHORIZED);
   assert(!g_mock.sealed);
   assert(vault_maintenance_guard_end_operational(&guard, 41) == VAULT_MAINTENANCE_OK);
   assert(!guard && !g_mock.sealed && g_mock.seal_calls == 0);

   uint64_t epoch = vault_use_epoch_snapshot();
   uint8_t kek[VAULT_KEK_LEN];
   assert(epoch != 0 && vault_use_begin(epoch, 41, kek) == 0);
   assert(kek[0] == 0x73 && kek[VAULT_KEK_LEN - 1] == 0x73);
   vault_use_end();
   assert(vault_seal() == 0);
}

static void test_failed_operational_finish_seals_and_consumes(void)
{
   bind_mock(13);
   vault_maintenance_guard_t *guard = NULL;
   assert(vault_maintenance_guard_begin(&guard) == VAULT_MAINTENANCE_OK);
   assert(vault_maintenance_guard_sync_primary_epoch(guard, 51) == VAULT_MAINTENANCE_OK);
   assert(vault_maintenance_guard_unseal_typed(guard, "ok", 2) == VAULT_CUSTODY_AUTHORIZED);
   assert(vault_maintenance_guard_end_operational(&guard, 52) == VAULT_MAINTENANCE_EPOCH);
   assert(!guard && g_mock.sealed && g_mock.seal_calls == 1);

   assert(vault_maintenance_guard_begin(&guard) == VAULT_MAINTENANCE_OK);
   assert(vault_maintenance_guard_sync_primary_epoch(guard, 53) == VAULT_MAINTENANCE_OK);
   assert(vault_maintenance_guard_unseal_typed(guard, "ok", 2) == VAULT_CUSTODY_AUTHORIZED);
   g_mock.get_fail = 1;
   assert(vault_maintenance_guard_end_operational(&guard, 53) == VAULT_MAINTENANCE_ERROR);
   assert(!guard && g_mock.sealed && g_mock.seal_calls == 2);
}

typedef struct
{
   vault_maintenance_guard_t **guard;
   int finish_result;
} callback_ctx_t;

static int finish_from_callback(const uint8_t kek[VAULT_KEK_LEN], void *opaque)
{
   callback_ctx_t *ctx = opaque;
   assert(kek[0] == 0x73);
   ctx->finish_result = vault_maintenance_guard_end_operational(ctx->guard, 61);
   return 17;
}

static void test_active_callback_cannot_publish(void)
{
   bind_mock(17);
   vault_maintenance_guard_t *guard = NULL;
   assert(vault_maintenance_guard_begin(&guard) == VAULT_MAINTENANCE_OK);
   assert(vault_maintenance_guard_sync_primary_epoch(guard, 61) == VAULT_MAINTENANCE_OK);
   assert(vault_maintenance_guard_unseal_typed(guard, "ok", 2) == VAULT_CUSTODY_AUTHORIZED);
   callback_ctx_t ctx = {.guard = &guard};
   assert(vault_maintenance_guard_with_active_kek(guard, finish_from_callback, &ctx) == 17);
   assert(ctx.finish_result == VAULT_MAINTENANCE_BUSY && guard != NULL && !g_mock.sealed);
   assert(vault_maintenance_guard_end(&guard) == VAULT_MAINTENANCE_OK);
   assert(g_mock.sealed);
}

int main(void)
{
   test_closed_preflight();
   test_typed_unseal_and_operational_finish();
   test_failed_operational_finish_seals_and_consumes();
   test_active_callback_cannot_publish();
   vault_custody_set_provider(vault_custody_tpm2_provider());
   uint64_t generation = UINT64_MAX;
   assert(vault_custody_selected_authorization_preflight_current("ok", 2, &generation) ==
              VAULT_CUSTODY_AUTH_UNSUPPORTED &&
          generation == 0);
   vault_custody_set_provider(NULL);
   assert(vault_custody_selected_authorization_preflight("ok", 2, 1) ==
          VAULT_CUSTODY_AUTH_UNSUPPORTED);
   puts("vault_d3b_custody: all tests passed");
   return 0;
}
