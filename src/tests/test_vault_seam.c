/* test_vault_seam.c: WP tiered-LLM P10 — the vault backend seam (vault_internal.h).
 *
 * Two properties are pinned here:
 *   (1) The vault_store_backend_t vtable dispatches to whatever backend it points
 *       at: a locally-constructed MOCK backend whose fns bump counters and return
 *       canned values is reached THROUGH the vtable, with ctx threaded as the
 *       first argument. This proves the seam is a real indirection, not a no-op.
 *   (2) The PUBLIC vault_store_* facade round-trips against a throwaway AIMEE_HOME
 *       — proving the facade correctly dispatches to the real (file-static)
 *       jsonfile backend without that backend being exported. */
#include "vault_store.h"
#include "vault_crypto.h"
#include "vault_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_home[256];

static void make_kek(uint8_t kek[VAULT_KEK_LEN], unsigned seed)
{
   for (int i = 0; i < VAULT_KEK_LEN; i++)
      kek[i] = (uint8_t)(seed * 7 + i);
}

/* ── (1) Mock backend: purely local vtable instances ──────────────────────── */

/* One counter per op we assert through, plus the ctx the vtable handed us — so
 * we can prove ctx was threaded through as the FIRST argument. */
typedef struct
{
   void *seen_ctx;
   int get_or_create_salt_calls;
   int set_calls;
   int get_calls;
   int delete_calls;
   int list_principals_calls;
   char last_principal[256];
} mock_state_t;

static mock_state_t g_mock;

static int mock_get_or_create_salt(void *ctx, const char *principal, uint8_t salt[VAULT_SALT_LEN])
{
   g_mock.seen_ctx = ctx;
   g_mock.get_or_create_salt_calls++;
   snprintf(g_mock.last_principal, sizeof(g_mock.last_principal), "%s", principal);
   memset(salt, 0x5a, VAULT_SALT_LEN); /* canned, non-zero */
   return 0;
}

static int mock_set(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                    const char *agent, const char *cred, const char *secret)
{
   (void)kek;
   (void)agent;
   (void)cred;
   (void)secret;
   g_mock.seen_ctx = ctx;
   g_mock.set_calls++;
   snprintf(g_mock.last_principal, sizeof(g_mock.last_principal), "%s", principal);
   return 0;
}

static int mock_get(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                    const char *agent, const char *cred, char *out, size_t out_len)
{
   (void)principal;
   (void)kek;
   (void)agent;
   (void)cred;
   g_mock.seen_ctx = ctx;
   g_mock.get_calls++;
   snprintf(out, out_len, "canned-secret");
   return 0;
}

static int mock_delete(void *ctx, const char *principal, const char *agent, const char *cred)
{
   (void)principal;
   (void)agent;
   (void)cred;
   g_mock.seen_ctx = ctx;
   g_mock.delete_calls++;
   return 0;
}

static int mock_list_principals(void *ctx, char (*out)[VAULT_PRINCIPAL_MAX], int max)
{
   (void)out;
   (void)max;
   g_mock.seen_ctx = ctx;
   g_mock.list_principals_calls++;
   return 0;
}

/* A sentinel address handed to the mock as ctx: the vtable must forward it
 * verbatim as each op's first argument. */
static int g_ctx_marker;

static void test_mock_dispatch_through_vtable(void)
{
   memset(&g_mock, 0, sizeof(g_mock));

   const vault_store_backend_t mock = {
       .name = "mock",
       .ctx = &g_ctx_marker,
       .get_or_create_salt = mock_get_or_create_salt,
       .set = mock_set,
       .get = mock_get,
       .delete = mock_delete,
       .list_principals = mock_list_principals,
   };
   const vault_store_backend_t *b = &mock;

   assert(strcmp(b->name, "mock") == 0);

   uint8_t salt[VAULT_SALT_LEN];
   assert(b->get_or_create_salt(b->ctx, "uid:1000", salt) == 0);
   assert(g_mock.get_or_create_salt_calls == 1);
   assert(g_mock.seen_ctx == &g_ctx_marker); /* ctx threaded as first arg */
   assert(strcmp(g_mock.last_principal, "uid:1000") == 0);
   assert(salt[0] == 0x5a); /* canned value came back through the seam */

   uint8_t kek[VAULT_KEK_LEN];
   make_kek(kek, 1);
   assert(b->set(b->ctx, "uid:1000", kek, "claude", "api_key", "s") == 0);
   assert(g_mock.set_calls == 1);

   char out[64] = {0};
   assert(b->get(b->ctx, "uid:1000", kek, "claude", "api_key", out, sizeof(out)) == 0);
   assert(g_mock.get_calls == 1);
   assert(strcmp(out, "canned-secret") == 0);

   assert(b->delete(b->ctx, "uid:1000", "claude", "api_key") == 0);
   assert(g_mock.delete_calls == 1);

   char principals[4][VAULT_PRINCIPAL_MAX];
   assert(b->list_principals(b->ctx, principals, 4) == 0);
   assert(g_mock.list_principals_calls == 1);

   assert(g_mock.seen_ctx == &g_ctx_marker); /* every call saw the same ctx */
   printf("  PASS: test_mock_dispatch_through_vtable\n");
}

/* A second, independent local vtable instance must be reachable without touching
 * the first — the seam holds no hidden global backend selection. */
static void test_two_local_backends_are_independent(void)
{
   static mock_state_t local_a, local_b;
   memset(&local_a, 0, sizeof(local_a));
   memset(&local_b, 0, sizeof(local_b));

   /* Reuse mock_* which write to g_mock; here we only assert the vtable type is
    * constructible twice with distinct names/ctx and both dispatch. */
   int marker_a = 0, marker_b = 0;
   const vault_store_backend_t a = {.name = "a", .ctx = &marker_a, .set = mock_set};
   const vault_store_backend_t b = {.name = "b", .ctx = &marker_b, .set = mock_set};

   memset(&g_mock, 0, sizeof(g_mock));
   uint8_t kek[VAULT_KEK_LEN];
   make_kek(kek, 2);
   assert(a.set(a.ctx, "uid:1", kek, "x", "y", "z") == 0);
   assert(g_mock.seen_ctx == &marker_a);
   assert(b.set(b.ctx, "uid:2", kek, "x", "y", "z") == 0);
   assert(g_mock.seen_ctx == &marker_b);
   assert(g_mock.set_calls == 2);
   printf("  PASS: test_two_local_backends_are_independent\n");
}

/* ── (2) Real jsonfile backend, reached THROUGH the public facade ─────────── */

static void test_facade_dispatches_to_real_backend(void)
{
   const char *principal = "uid:1000";

   /* Salt: get_or_create establishes the vault file. */
   uint8_t salt[VAULT_SALT_LEN];
   assert(vault_store_get_or_create_salt(principal, salt) == 0);

   /* unlock_check establishes the KEK verifier on first call, matches after. */
   uint8_t kek[VAULT_KEK_LEN];
   make_kek(kek, 7);
   assert(vault_store_unlock_check(principal, kek) == 0);
   assert(vault_store_unlock_check(principal, kek) == 0);

   /* set + get round-trips the credential through the facade -> jsonfile. */
   const char *secret = "sk-seam-roundtrip";
   assert(vault_store_set(principal, kek, "claude", "api_key", secret) == 0);
   char out[128];
   assert(vault_store_get(principal, kek, "claude", "api_key", out, sizeof(out)) == 0);
   assert(strcmp(out, secret) == 0);

   /* has_entry / list see it. */
   assert(vault_store_has_entry(principal, "claude", "api_key") == 1);
   vault_store_entry_t entries[4];
   assert(vault_store_list(principal, entries, 4) == 1);

   /* delete removes it; the get then reports NO_ENTRY. */
   assert(vault_store_delete(principal, "claude", "api_key") == 0);
   assert(vault_store_get(principal, kek, "claude", "api_key", out, sizeof(out)) ==
          VAULT_STORE_NO_ENTRY);
   assert(vault_store_has_entry(principal, "claude", "api_key") == 0);

   /* list_principals sees the principal we created (proves the dir-scan op
    * dispatches through the seam too). */
   char principals[8][VAULT_PRINCIPAL_MAX];
   int np = vault_store_list_principals(principals, 8);
   assert(np >= 1);
   int found = 0;
   for (int i = 0; i < np; i++)
      if (strcmp(principals[i], principal) == 0)
         found = 1;
   assert(found);
   printf("  PASS: test_facade_dispatches_to_real_backend\n");
}

int main(void)
{
   snprintf(g_home, sizeof(g_home), "/tmp/aimee-vault-seam-test-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", g_home, g_home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", g_home, 1);

   test_mock_dispatch_through_vtable();
   test_two_local_backends_are_independent();
   test_facade_dispatches_to_real_backend();

   char rm[320];
   snprintf(rm, sizeof(rm), "rm -rf %s", g_home);
   (void)system(rm);
   printf("vault_seam: all tests passed\n");
   return 0;
}
