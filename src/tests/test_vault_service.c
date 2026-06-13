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

/* WP-C.2: the webuser password unlock (scrypt). */
static void test_webuser_password_unlock(void)
{
   const char *p = "webuser:alice";
   const uint8_t pw[] = "alice-login-password";

   /* set before unlock => locked. */
   assert(vault_service_set(p, "claude", "api_key", "s", T0) == VAULT_ERR_LOCKED);

   /* Password unlock requires the webchat-trusted transport. */
   assert(vault_service_unlock_password(p, ATTEST_UDS_PEERCRED, pw, sizeof(pw) - 1, T0) ==
          VAULT_ERR_TRANSPORT);
   assert(vault_service_unlock_password(p, ATTEST_TCP_BEARER, pw, sizeof(pw) - 1, T0) ==
          VAULT_ERR_TRANSPORT);
   assert(vault_service_unlock_password("", ATTEST_WEBCHAT_TRUSTED, pw, sizeof(pw) - 1, T0) ==
          VAULT_ERR_UNATTESTED);

   /* Unlock + round-trip. */
   assert(vault_service_unlock_password(p, ATTEST_WEBCHAT_TRUSTED, pw, sizeof(pw) - 1, T0) ==
          VAULT_OK);
   assert(vault_service_set(p, "claude", "api_key", "sk-webuser-secret", T0) == VAULT_OK);
   char out[64];
   assert(vault_service_get(p, "claude", "api_key", out, sizeof(out), T0) == VAULT_OK);
   assert(strcmp(out, "sk-webuser-secret") == 0);

   /* A wrong password is caught immediately by the key-check verifier (no KEK
    * cached), so the vault stays locked. */
   assert(vault_service_lock(p) == VAULT_OK);
   const uint8_t wrong[] = "not-alices-password";
   assert(vault_service_unlock_password(p, ATTEST_WEBCHAT_TRUSTED, wrong, sizeof(wrong) - 1, T0) ==
          VAULT_ERR_CRYPTO);
   assert(vault_service_get(p, "claude", "api_key", out, sizeof(out), T0) == VAULT_ERR_LOCKED);

   /* The right password again decrypts. */
   assert(vault_service_lock(p) == VAULT_OK);
   assert(vault_service_unlock_password(p, ATTEST_WEBCHAT_TRUSTED, pw, sizeof(pw) - 1, T0) ==
          VAULT_OK);
   assert(vault_service_get(p, "claude", "api_key", out, sizeof(out), T0) == VAULT_OK);
   assert(strcmp(out, "sk-webuser-secret") == 0);
   printf("  PASS: test_webuser_password_unlock\n");
}

/* WP-C.2b: password-change re-key. The stored credential survives a rekey
 * (re-wrapped, not re-encrypted), the new password decrypts it, the old no
 * longer does, and a wrong old password leaves the vault untouched. */
static void test_webuser_rekey(void)
{
   const char *p = "webuser:bob";
   const uint8_t old_pw[] = "bob-old-password";
   const uint8_t new_pw[] = "bob-new-password";
   assert(vault_service_unlock_password(p, ATTEST_WEBCHAT_TRUSTED, old_pw, sizeof(old_pw) - 1,
                                        T0) == VAULT_OK);
   assert(vault_service_set(p, "claude", "api_key", "bob-secret", T0) == VAULT_OK);

   /* Transport gate + bad args. */
   assert(vault_service_rekey_password(p, ATTEST_UDS_PEERCRED, old_pw, sizeof(old_pw) - 1, new_pw,
                                       sizeof(new_pw) - 1, T0) == VAULT_ERR_TRANSPORT);

   /* Wrong OLD password: rekey fails closed, vault untouched (old pw still works). */
   const uint8_t wrong_old[] = "not-bobs-old-password";
   assert(vault_service_rekey_password(p, ATTEST_WEBCHAT_TRUSTED, wrong_old, sizeof(wrong_old) - 1,
                                       new_pw, sizeof(new_pw) - 1, T0) == VAULT_ERR_CRYPTO);
   assert(vault_service_lock(p) == VAULT_OK);
   assert(vault_service_unlock_password(p, ATTEST_WEBCHAT_TRUSTED, old_pw, sizeof(old_pw) - 1,
                                        T0) == VAULT_OK);
   char out[64];
   assert(vault_service_get(p, "claude", "api_key", out, sizeof(out), T0) == VAULT_OK);
   assert(strcmp(out, "bob-secret") == 0); /* untouched by the failed rekey */

   /* Correct rekey: re-wrap under the new password; the new KEK is cached. */
   assert(vault_service_rekey_password(p, ATTEST_WEBCHAT_TRUSTED, old_pw, sizeof(old_pw) - 1,
                                       new_pw, sizeof(new_pw) - 1, T0) == VAULT_OK);
   assert(vault_service_get(p, "claude", "api_key", out, sizeof(out), T0) == VAULT_OK);
   assert(strcmp(out, "bob-secret") == 0); /* same secret, now under the new KEK */

   /* The OLD password no longer matches the re-keyed verifier (caught at unlock). */
   assert(vault_service_lock(p) == VAULT_OK);
   assert(vault_service_unlock_password(p, ATTEST_WEBCHAT_TRUSTED, old_pw, sizeof(old_pw) - 1,
                                        T0) == VAULT_ERR_CRYPTO);
   /* The NEW password does. */
   assert(vault_service_lock(p) == VAULT_OK);
   assert(vault_service_unlock_password(p, ATTEST_WEBCHAT_TRUSTED, new_pw, sizeof(new_pw) - 1,
                                        T0) == VAULT_OK);
   assert(vault_service_get(p, "claude", "api_key", out, sizeof(out), T0) == VAULT_OK);
   assert(strcmp(out, "bob-secret") == 0);
   printf("  PASS: test_webuser_rekey\n");
}

/* WP-C.2b roundtable blocker fix: rekey must validate the old password even with
 * ZERO credentials (the normal fresh state), and must not fail-open or
 * materialize a vault for a non-existent principal. */
static void test_rekey_empty_and_missing_vault(void)
{
   const uint8_t old_pw[] = "carol-old", new_pw[] = "carol-new", wrong[] = "carol-wrong";

   /* (b) rekey of a principal with NO vault at all -> fail closed, creates nothing. */
   assert(vault_service_rekey_password("webuser:nobody", ATTEST_WEBCHAT_TRUSTED, old_pw,
                                       sizeof(old_pw) - 1, new_pw, sizeof(new_pw) - 1,
                                       T0) == VAULT_ERR_CRYPTO);
   uint8_t salt[VAULT_SALT_LEN];
   assert(vault_store_salt_readonly("webuser:nobody", salt) == -1); /* no vault materialized */

   /* Provision + unlock an EMPTY vault (no credential stored). */
   const char *p = "webuser:carol";
   assert(vault_service_unlock_password(p, ATTEST_WEBCHAT_TRUSTED, old_pw, sizeof(old_pw) - 1,
                                        T0) == VAULT_OK);

   /* (a) rekey of the empty vault with a WRONG old password MUST fail closed —
    * this is the fail-open the verifier closes. */
   assert(vault_service_rekey_password(p, ATTEST_WEBCHAT_TRUSTED, wrong, sizeof(wrong) - 1, new_pw,
                                       sizeof(new_pw) - 1, T0) == VAULT_ERR_CRYPTO);
   /* The old password still unlocks (vault untouched by the failed rekey). */
   assert(vault_service_lock(p) == VAULT_OK);
   assert(vault_service_unlock_password(p, ATTEST_WEBCHAT_TRUSTED, old_pw, sizeof(old_pw) - 1,
                                        T0) == VAULT_OK);

   /* (c) correct rekey on the empty vault succeeds; afterward only the new
    * password unlocks. */
   assert(vault_service_rekey_password(p, ATTEST_WEBCHAT_TRUSTED, old_pw, sizeof(old_pw) - 1,
                                       new_pw, sizeof(new_pw) - 1, T0) == VAULT_OK);
   assert(vault_service_lock(p) == VAULT_OK);
   assert(vault_service_unlock_password(p, ATTEST_WEBCHAT_TRUSTED, old_pw, sizeof(old_pw) - 1,
                                        T0) == VAULT_ERR_CRYPTO);
   assert(vault_service_unlock_password(p, ATTEST_WEBCHAT_TRUSTED, new_pw, sizeof(new_pw) - 1,
                                        T0) == VAULT_OK);
   printf("  PASS: test_rekey_empty_and_missing_vault\n");
}

/* WP-C.4: the autonomous server path. After a "restart" (the user KEK cache is
 * cleared, so the user vault is LOCKED), a dual-wrapped credential is STILL
 * injectable via the server wrap — the whole point: aimee-server can drive
 * delegates without the client re-unlocking. A genuinely missing credential is
 * still NO_ENTRY and leaves the caller's api_key untouched. */
static void test_server_inject_after_restart(void)
{
   const char *p = "uid:7000";
   uint8_t rk[VAULT_ROOT_KEY_LEN];
   root_key(rk, 7);
   assert(vault_service_unlock(p, ATTEST_UDS_PEERCRED, rk, sizeof(rk), T0) == VAULT_OK);
   assert(vault_service_set(p, "claude", "api_key", "sk-autonomous", T0) == VAULT_OK);

   /* While unlocked, inject works. */
   char key[64] = "PRESET";
   assert(vault_service_inject_api_key(p, "claude", key, sizeof(key), T0) == VAULT_OK);
   assert(strcmp(key, "sk-autonomous") == 0);

   /* Simulate a restart: drop every cached user KEK -> the user vault is locked. */
   vault_kek_cache_clear();
   char out[64];
   assert(vault_service_get(p, "claude", "api_key", out, sizeof(out), T0) == VAULT_ERR_LOCKED);
   /* Inject still succeeds via the server wrap with NO unlock. */
   char key2[64] = "PRESET";
   assert(vault_service_inject_api_key(p, "claude", key2, sizeof(key2), T0) == VAULT_OK);
   assert(strcmp(key2, "sk-autonomous") == 0);

   /* A truly missing credential is NO_ENTRY; api_key left untouched (env fallback). */
   char key3[64] = "KEEP";
   assert(vault_service_inject_api_key(p, "absent-agent", key3, sizeof(key3), T0) ==
          VAULT_NO_ENTRY);
   assert(strcmp(key3, "KEEP") == 0);
   printf("  PASS: test_server_inject_after_restart\n");
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
   test_webuser_password_unlock();
   test_webuser_rekey();
   test_rekey_empty_and_missing_vault();
   test_server_inject_after_restart();

   vault_kek_cache_clear();
   char rm[320];
   snprintf(rm, sizeof(rm), "rm -rf %s", g_home);
   (void)system(rm);
   printf("vault_service: all tests passed\n");
   return 0;
}
