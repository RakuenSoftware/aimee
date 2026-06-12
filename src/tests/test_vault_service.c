/* test_vault_service.c: WP-C.1 — the vault service gates + lifecycle. Pins the
 * security-critical decisions: un-attested/transport refusal, the locked-vs-
 * missing distinction (D15), unlock->set->get, lock, and TTL-expiry => locked. */
#include "vault_service.h"
#include "vault_crypto.h"
#include "vault_kek_cache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_home[256];
static const long T0 = 100000;

static void root_key(uint8_t rk[VAULT_ROOT_KEY_LEN], unsigned seed)
{
   for (int i = 0; i < VAULT_ROOT_KEY_LEN; i++)
      rk[i] = (uint8_t)(seed * 13 + i);
}

/* An un-attested (empty) principal is refused at unlock, and its get falls back
 * silently (no vault) rather than erroring. */
static void test_unattested_refused(void)
{
   uint8_t rk[VAULT_ROOT_KEY_LEN];
   root_key(rk, 1);
   assert(vault_service_unlock("", ATTEST_UDS_PEERCRED, rk, sizeof(rk), T0) ==
          VAULT_ERR_UNATTESTED);
   char out[64];
   assert(vault_service_get("", "claude", "api_key", out, sizeof(out), T0) == VAULT_NO_ENTRY);
   printf("  PASS: test_unattested_refused\n");
}

/* The root-key push is UDS-only: refused over TCP and for a webchat principal. */
static void test_root_key_transport_gate(void)
{
   uint8_t rk[VAULT_ROOT_KEY_LEN];
   root_key(rk, 1);
   assert(vault_service_unlock("uid:1000", ATTEST_TCP_BEARER, rk, sizeof(rk), T0) ==
          VAULT_ERR_TRANSPORT);
   assert(vault_service_unlock("webuser:alice", ATTEST_WEBCHAT_TRUSTED, rk, sizeof(rk), T0) ==
          VAULT_ERR_TRANSPORT);
   /* Wrong root-key length is a bad argument. */
   assert(vault_service_unlock("uid:1000", ATTEST_UDS_PEERCRED, rk, 16, T0) == VAULT_ERR_BADARG);
   printf("  PASS: test_root_key_transport_gate\n");
}

/* set before unlock is LOCKED; after unlock, set+get round-trips. */
static void test_unlock_set_get(void)
{
   const char *p = "uid:1000";
   /* Locked: no unlock yet. */
   assert(vault_service_set(p, "claude", "api_key", "secret", T0) == VAULT_ERR_LOCKED);

   uint8_t rk[VAULT_ROOT_KEY_LEN];
   root_key(rk, 1);
   assert(vault_service_unlock(p, ATTEST_UDS_PEERCRED, rk, sizeof(rk), T0) == VAULT_OK);
   assert(vault_service_set(p, "claude", "api_key", "sk-secret-value", T0) == VAULT_OK);

   char out[64];
   assert(vault_service_get(p, "claude", "api_key", out, sizeof(out), T0) == VAULT_OK);
   assert(strcmp(out, "sk-secret-value") == 0);
   printf("  PASS: test_unlock_set_get\n");
}

/* D15: a credential that EXISTS but whose vault is locked is a HARD error; a
 * credential that does NOT exist is NO_ENTRY (fall back). */
static void test_locked_vs_missing(void)
{
   const char *p = "uid:1000"; /* has claude/api_key from the prior test */
   char out[64];
   /* Missing credential -> NO_ENTRY even while unlocked. */
   assert(vault_service_get(p, "claude", "nonexistent", out, sizeof(out), T0) == VAULT_NO_ENTRY);

   /* Now lock the vault; the existing credential becomes a HARD locked error,
    * never a silent fall-through. */
   assert(vault_service_lock(p) == VAULT_OK);
   assert(vault_service_get(p, "claude", "api_key", out, sizeof(out), T0) == VAULT_ERR_LOCKED);
   assert(out[0] == '\0');
   /* A genuinely missing credential is still NO_ENTRY even when locked. */
   assert(vault_service_get(p, "claude", "nonexistent", out, sizeof(out), T0) == VAULT_NO_ENTRY);
   printf("  PASS: test_locked_vs_missing\n");
}

/* A KEK past its TTL means the vaulted credential reads as locked (fail-closed),
 * never with a stale key. */
static void test_ttl_expiry_locks(void)
{
   const char *p = "uid:5000";
   uint8_t rk[VAULT_ROOT_KEY_LEN];
   root_key(rk, 5);
   assert(vault_service_unlock(p, ATTEST_UDS_PEERCRED, rk, sizeof(rk), T0) == VAULT_OK);
   assert(vault_service_set(p, "openai", "api_key", "o-secret", T0) == VAULT_OK);

   char out[64];
   long later = T0 + VAULT_KEK_CACHE_TTL_SECONDS + 1;
   assert(vault_service_get(p, "openai", "api_key", out, sizeof(out), later) == VAULT_ERR_LOCKED);
   printf("  PASS: test_ttl_expiry_locks\n");
}

static void test_list_and_delete(void)
{
   const char *p = "uid:6000";
   uint8_t rk[VAULT_ROOT_KEY_LEN];
   root_key(rk, 6);
   assert(vault_service_unlock(p, ATTEST_UDS_PEERCRED, rk, sizeof(rk), T0) == VAULT_OK);
   assert(vault_service_set(p, "claude", "api_key", "c", T0) == VAULT_OK);
   assert(vault_service_set(p, "openai", "api_key", "o", T0) == VAULT_OK);

   vault_store_entry_t entries[8];
   int count = -1;
   assert(vault_service_list(p, entries, 8, &count) == VAULT_OK);
   assert(count == 2);

   assert(vault_service_delete(p, "claude", "api_key") == VAULT_OK);
   assert(vault_service_list(p, entries, 8, &count) == VAULT_OK && count == 1);
   /* list on an empty principal is unattested-refused. */
   assert(vault_service_list("", entries, 8, &count) == VAULT_ERR_UNATTESTED);
   printf("  PASS: test_list_and_delete\n");
}

int main(void)
{
   snprintf(g_home, sizeof(g_home), "/tmp/aimee-vaultsvc-test-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", g_home, g_home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", g_home, 1);
   vault_kek_cache_clear();

   test_unattested_refused();
   test_root_key_transport_gate();
   test_unlock_set_get();
   test_locked_vs_missing();
   test_ttl_expiry_locks();
   test_list_and_delete();

   vault_kek_cache_clear();
   char rm[320];
   snprintf(rm, sizeof(rm), "rm -rf %s", g_home);
   (void)system(rm);
   printf("vault_service: all tests passed\n");
   return 0;
}
