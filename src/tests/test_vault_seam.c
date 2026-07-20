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
#include "vault_server_key.h"
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
   int salt_readonly_calls;
   int unlock_check_calls;
   int set_calls;
   int set_dual_calls;
   int set_server_calls;
   int get_server_calls;
   int add_server_wraps_calls;
   int get_calls;
   int has_entry_calls;
   int list_calls;
   int delete_calls;
   int rekey_calls;
   int rekey_field_calls;
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

/* ── (3) Public facade dispatches through the REBOUND backend (real swap) ─────
 * The findings that matter most: without a binder, "swappable" is unproven and
 * P7 can't compose a profile. These mock ops cover every vault_store_* slot so we
 * can bind a full mock, drive the PUBLIC facade, and assert each call reached the
 * mock (with ctx threaded) — then restore the default and confirm jsonfile is
 * back. This is the genuine swappability + facade-forwarding proof. */

static int fm_salt_readonly(void *ctx, const char *p, uint8_t salt[VAULT_SALT_LEN])
{
   g_mock.seen_ctx = ctx;
   g_mock.salt_readonly_calls++;
   snprintf(g_mock.last_principal, sizeof(g_mock.last_principal), "%s", p);
   memset(salt, 0x33, VAULT_SALT_LEN);
   return 0;
}
static int fm_unlock_check(void *ctx, const char *p, const uint8_t kek[VAULT_KEK_LEN])
{
   (void)p;
   (void)kek;
   g_mock.seen_ctx = ctx;
   g_mock.unlock_check_calls++;
   return 0;
}
static int fm_set_dual(void *ctx, const char *p, const uint8_t kek[VAULT_KEK_LEN],
                       const uint8_t skek[VAULT_KEK_LEN], const char *a, const char *c,
                       const char *s)
{
   (void)p;
   (void)kek;
   (void)skek;
   (void)a;
   (void)c;
   (void)s;
   g_mock.seen_ctx = ctx;
   g_mock.set_dual_calls++;
   return 0;
}
static int fm_set_server(void *ctx, const char *p, const uint8_t skek[VAULT_KEK_LEN], const char *a,
                         const char *c, const char *s)
{
   (void)p;
   (void)skek;
   (void)a;
   (void)c;
   (void)s;
   g_mock.seen_ctx = ctx;
   g_mock.set_server_calls++;
   return 0;
}
static int fm_get_server(void *ctx, const char *p, const uint8_t skek[VAULT_KEK_LEN], const char *a,
                         const char *c, char *out, size_t out_len)
{
   (void)p;
   (void)skek;
   (void)a;
   (void)c;
   g_mock.seen_ctx = ctx;
   g_mock.get_server_calls++;
   snprintf(out, out_len, "canned-server");
   return 0;
}
static int fm_add_server_wraps(void *ctx, const char *p, const uint8_t ukek[VAULT_KEK_LEN],
                               const uint8_t skek[VAULT_KEK_LEN])
{
   (void)p;
   (void)ukek;
   (void)skek;
   g_mock.seen_ctx = ctx;
   g_mock.add_server_wraps_calls++;
   return 0;
}
static int fm_has_entry(void *ctx, const char *p, const char *a, const char *c)
{
   (void)p;
   (void)a;
   (void)c;
   g_mock.seen_ctx = ctx;
   g_mock.has_entry_calls++;
   return 1;
}
static int fm_list(void *ctx, const char *p, vault_store_entry_t *out, int max)
{
   (void)p;
   (void)out;
   (void)max;
   g_mock.seen_ctx = ctx;
   g_mock.list_calls++;
   return 0;
}
static int fm_rekey(void *ctx, const char *p, const uint8_t o[VAULT_KEK_LEN],
                    const uint8_t n[VAULT_KEK_LEN])
{
   (void)p;
   (void)o;
   (void)n;
   g_mock.seen_ctx = ctx;
   g_mock.rekey_calls++;
   return 0;
}
static int fm_rekey_field(void *ctx, const char *p, const char *field,
                          const uint8_t o[VAULT_KEK_LEN], const uint8_t n[VAULT_KEK_LEN])
{
   (void)p;
   (void)field;
   (void)o;
   (void)n;
   g_mock.seen_ctx = ctx;
   g_mock.rekey_field_calls++;
   return 0;
}

static void test_facade_swaps_to_mock_backend(void)
{
   memset(&g_mock, 0, sizeof(g_mock));
   const vault_store_backend_t full_mock = {
       .name = "full-mock",
       .ctx = &g_ctx_marker,
       .get_or_create_salt = mock_get_or_create_salt,
       .salt_readonly = fm_salt_readonly,
       .unlock_check = fm_unlock_check,
       .set = mock_set,
       .set_dual = fm_set_dual,
       .set_server = fm_set_server,
       .get_server = fm_get_server,
       .add_server_wraps = fm_add_server_wraps,
       .get = mock_get,
       .has_entry = fm_has_entry,
       .list = fm_list,
       .delete = mock_delete,
       .rekey = fm_rekey,
       .rekey_field = fm_rekey_field,
       .list_principals = mock_list_principals,
   };

   vault_store_set_backend(&full_mock); /* the binder under test */

   uint8_t salt[VAULT_SALT_LEN], kek[VAULT_KEK_LEN], skek[VAULT_KEK_LEN];
   make_kek(kek, 3);
   make_kek(skek, 4);
   char out[64];
   vault_store_entry_t entries[2];
   char principals[2][VAULT_PRINCIPAL_MAX];

   /* Every PUBLIC facade call must now land on the mock, with ctx threaded. */
   assert(vault_store_get_or_create_salt("uid:9", salt) == 0);
   assert(vault_store_salt_readonly("uid:9", salt) == 0);
   assert(vault_store_unlock_check("uid:9", kek) == 0);
   assert(vault_store_set("uid:9", kek, "a", "c", "s") == 0);
   assert(vault_store_set_dual("uid:9", kek, skek, "a", "c", "s") == 0);
   assert(vault_store_set_server("uid:9", skek, "a", "c", "s") == 0);
   assert(vault_store_get_server("uid:9", skek, "a", "c", out, sizeof(out)) == 0);
   assert(vault_store_add_server_wraps("uid:9", kek, skek) == 0);
   assert(vault_store_get("uid:9", kek, "a", "c", out, sizeof(out)) == 0);
   assert(vault_store_has_entry("uid:9", "a", "c") == 1);
   assert(vault_store_list("uid:9", entries, 2) == 0);
   assert(vault_store_delete("uid:9", "a", "c") == 0);
   assert(vault_store_rekey("uid:9", kek, skek) == 0);
   assert(vault_store_rekey_field("uid:9", "wrapped_dek", kek, skek) == 0);
   assert(vault_store_list_principals(principals, 2) == 0);

   assert(g_mock.get_or_create_salt_calls == 1 && g_mock.salt_readonly_calls == 1);
   assert(g_mock.unlock_check_calls == 1 && g_mock.set_calls == 1);
   assert(g_mock.set_dual_calls == 1 && g_mock.set_server_calls == 1);
   assert(g_mock.get_server_calls == 1 && g_mock.add_server_wraps_calls == 1);
   assert(g_mock.get_calls == 1 && g_mock.has_entry_calls == 1 && g_mock.list_calls == 1);
   assert(g_mock.delete_calls == 1 && g_mock.rekey_calls == 1 && g_mock.rekey_field_calls == 1);
   assert(g_mock.list_principals_calls == 1);
   assert(g_mock.seen_ctx == &g_ctx_marker); /* the mock's ctx, not jsonfile's NULL */

   /* Restore the default and confirm jsonfile is genuinely back (a real op the
    * mock would have swallowed now round-trips on disk). */
   vault_store_set_backend(NULL);
   uint8_t rk[VAULT_KEK_LEN];
   make_kek(rk, 9);
   assert(vault_store_get_or_create_salt("uid:restored", salt) == 0);
   assert(vault_store_unlock_check("uid:restored", rk) == 0);
   assert(vault_store_set("uid:restored", rk, "claude", "api_key", "restored-secret") == 0);
   assert(vault_store_get("uid:restored", rk, "claude", "api_key", out, sizeof(out)) == 0);
   assert(strcmp(out, "restored-secret") == 0); /* jsonfile really handled it */
   printf("  PASS: test_facade_swaps_to_mock_backend\n");
}

/* ── (4) Custody seam: the public KEK path dispatches through the provider ──── */

static struct
{
   void *seen_ctx;
   int get_kek_calls;
   int rotate_calls;
   int hwm_read_calls;
   int hwm_cas_calls;
   int hwm_verify_calls;
   uint64_t hwm_version;
} g_custody_mock;

static int cm_get_kek(void *ctx, uint8_t kek[VAULT_KEK_LEN])
{
   g_custody_mock.seen_ctx = ctx;
   g_custody_mock.get_kek_calls++;
   memset(kek, 0x77, VAULT_KEK_LEN); /* canned KEK through the seam */
   return 0;
}
static int cm_rotate(void *ctx, const char *sp, int *op, int *oc, char *bp, size_t bpl, char *eb,
                     size_t el)
{
   (void)sp;
   (void)bp;
   (void)bpl;
   (void)eb;
   (void)el;
   g_custody_mock.seen_ctx = ctx;
   g_custody_mock.rotate_calls++;
   if (op)
      *op = 0;
   if (oc)
      *oc = 0;
   return 0;
}

static int cm_hwm_read(void *ctx, const char *key_id, uint64_t *version, uint8_t *att,
                       size_t att_cap, size_t *att_len)
{
   g_custody_mock.seen_ctx = ctx;
   g_custody_mock.hwm_read_calls++;
   if (!key_id || strcmp(key_id, "team:7|bedrock|primary") != 0 || att_cap < 8)
      return -1;
   *version = g_custody_mock.hwm_version;
   memset(att, 0xa5, 8);
   *att_len = 8;
   return 0;
}

static int cm_hwm_cas(void *ctx, const char *key_id, uint64_t expected, uint64_t next, uint8_t *att,
                      size_t att_cap, size_t *att_len)
{
   g_custody_mock.seen_ctx = ctx;
   g_custody_mock.hwm_cas_calls++;
   if (!key_id || strcmp(key_id, "team:7|bedrock|primary") != 0 ||
       expected != g_custody_mock.hwm_version || next != expected + 1 || att_cap < 8)
      return -1;
   g_custody_mock.hwm_version = next;
   memset(att, 0x5a, 8);
   *att_len = 8;
   return 0;
}

static int cm_hwm_verify(void *ctx, const char *key_id, uint64_t version, const uint8_t *att,
                         size_t att_len)
{
   g_custody_mock.seen_ctx = ctx;
   g_custody_mock.hwm_verify_calls++;
   return key_id && strcmp(key_id, "team:7|bedrock|primary") == 0 &&
                  version == g_custody_mock.hwm_version && att && att_len == 8
              ? 0
              : -1;
}

static void test_custody_facade_dispatches_through_provider(void)
{
   memset(&g_custody_mock, 0, sizeof(g_custody_mock));
   static int custody_marker;
   const vault_custody_provider_t mock = {
       .name = "mock-custody",
       .ctx = &custody_marker,
       .get_kek = cm_get_kek,
       .rotate = cm_rotate,
       .hwm_read = cm_hwm_read,
       .hwm_cas = cm_hwm_cas,
       .hwm_verify = cm_hwm_verify,
   };
   vault_custody_set_provider(&mock);

   uint8_t kek[VAULT_KEK_LEN];
   assert(vault_server_kek(kek) == 0); /* public facade -> provider->get_kek */
   assert(g_custody_mock.get_kek_calls == 1);
   assert(g_custody_mock.seen_ctx == &custody_marker); /* ctx threaded */
   assert(kek[0] == 0x77);

   int np = -1, nc = -1;
   char backup[256] = {0}, err[128] = {0};
   assert(vault_server_key_rotate("server", &np, &nc, backup, sizeof(backup), err, sizeof(err)) ==
          0);
   assert(g_custody_mock.rotate_calls == 1);
   assert(g_custody_mock.seen_ctx == &custody_marker);

   g_custody_mock.hwm_version = 4;
   uint8_t att[16];
   size_t att_len = 0;
   uint64_t version = 0;
   assert(vault_hwm_read("team:7|bedrock|primary", &version, att, sizeof(att), &att_len) == 0);
   assert(version == 4 && att_len == 8 && att[0] == 0xa5);
   assert(vault_hwm_cas("team:7|bedrock|primary", 4, 5, att, sizeof(att), &att_len) == 0);
   assert(g_custody_mock.hwm_version == 5 && att_len == 8 && att[0] == 0x5a);
   assert(vault_hwm_verify("team:7|bedrock|primary", 5, att, att_len) == 0);
   assert(g_custody_mock.hwm_verify_calls == 1);
   assert(vault_hwm_verify("team:7|bedrock|primary", 4, att, att_len) == -1);
   memset(att, 0xcc, sizeof(att));
   att_len = 9;
   assert(vault_hwm_cas("team:7|bedrock|primary", UINT64_MAX, 0, att, sizeof(att), &att_len) == -1);
   assert(att_len == 0 && att[0] == 0);

   const vault_custody_provider_t incomplete = {
       .name = "incomplete", .ctx = &custody_marker, .get_kek = cm_get_kek, .rotate = cm_rotate};
   vault_custody_set_provider(&incomplete);
   memset(att, 0xcc, sizeof(att));
   version = 99;
   att_len = 9;
   assert(vault_hwm_read("team:7|bedrock|primary", &version, att, sizeof(att), &att_len) == -1);
   assert(version == 0 && att_len == 0 && att[0] == 0);

   vault_custody_set_provider(NULL); /* restore the file provider */
   printf("  PASS: test_custody_facade_dispatches_through_provider\n");
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
   test_facade_swaps_to_mock_backend();
   test_custody_facade_dispatches_through_provider();

   char rm[320];
   snprintf(rm, sizeof(rm), "rm -rf %s", g_home);
   (void)system(rm);
   printf("vault_seam: all tests passed\n");
   return 0;
}
