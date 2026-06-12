/* test_vault_store.c: WP-C.1 — the on-disk vault substrate. Runs against a
 * throwaway AIMEE_HOME so it exercises the real file path/format. Pins the
 * round-trip, the at-rest "ciphertext only" property, principal isolation, and
 * the fail-closed / no-entry distinctions the use-path depends on. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* memmem */
#endif
#include "vault_store.h"
#include "vault_crypto.h"
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

/* Read the principal's raw vault file bytes into buf; returns length or -1.
 * (We reach into .vault/ by listing it — there is exactly one file per test
 * principal we create, so we scan the dir.) */
static long read_vault_blob(char *buf, size_t cap)
{
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "cat %s/.vault/*.json 2>/dev/null", g_home);
   FILE *p = popen(cmd, "r");
   if (!p)
      return -1;
   size_t n = fread(buf, 1, cap - 1, p);
   pclose(p);
   buf[n] = '\0';
   return (long)n;
}

static void test_set_get_roundtrip(void)
{
   const char *principal = "uid:1000";
   uint8_t salt[VAULT_SALT_LEN];
   assert(vault_store_get_or_create_salt(principal, salt) == 0);

   uint8_t kek[VAULT_KEK_LEN];
   make_kek(kek, 1);
   const char *secret = "sk-ant-deadbeef-credential";
   assert(vault_store_set(principal, kek, "claude", "api_key", secret) == 0);

   char out[256];
   assert(vault_store_get(principal, kek, "claude", "api_key", out, sizeof(out)) == 0);
   assert(strcmp(out, secret) == 0);
   printf("  PASS: test_set_get_roundtrip\n");
}

static void test_at_rest_is_ciphertext_only(void)
{
   const char *principal = "uid:1000";
   uint8_t kek[VAULT_KEK_LEN];
   make_kek(kek, 1);
   const char *secret = "PLAINTEXT-SHOULD-NOT-APPEAR-ON-DISK";
   assert(vault_store_set(principal, kek, "openai", "api_key", secret) == 0);

   char blob[8192];
   assert(read_vault_blob(blob, sizeof(blob)) > 0);
   /* The plaintext secret must NOT appear anywhere in the file. */
   assert(strstr(blob, secret) == NULL);
   /* The raw KEK bytes must not appear either. */
   assert(memmem(blob, strlen(blob), kek, VAULT_KEK_LEN) == NULL);
   /* But the structural fields are present. */
   assert(strstr(blob, "wrapped_dek") != NULL);
   assert(strstr(blob, "ciphertext") != NULL);
   assert(strstr(blob, "salt") != NULL);
   printf("  PASS: test_at_rest_is_ciphertext_only\n");
}

static void test_wrong_kek_fails_closed(void)
{
   const char *principal = "uid:1000";
   uint8_t kek[VAULT_KEK_LEN], wrong[VAULT_KEK_LEN];
   make_kek(kek, 1);
   make_kek(wrong, 99);
   assert(vault_store_set(principal, kek, "claude", "api_key", "the-secret") == 0);

   char out[256];
   assert(vault_store_get(principal, wrong, "claude", "api_key", out, sizeof(out)) == -1);
   assert(out[0] == '\0'); /* cleansed */
   printf("  PASS: test_wrong_kek_fails_closed\n");
}

static void test_no_entry_vs_error(void)
{
   uint8_t kek[VAULT_KEK_LEN];
   make_kek(kek, 1);
   char out[256];
   /* Principal with a file but no such cred -> NO_ENTRY (fall back). */
   assert(vault_store_get("uid:1000", kek, "claude", "missing_cred", out, sizeof(out)) ==
          VAULT_STORE_NO_ENTRY);
   /* Principal with no file at all -> NO_ENTRY. */
   assert(vault_store_get("uid:7777", kek, "claude", "api_key", out, sizeof(out)) ==
          VAULT_STORE_NO_ENTRY);
   assert(vault_store_has_entry("uid:1000", "claude", "api_key") == 1);
   assert(vault_store_has_entry("uid:1000", "claude", "missing_cred") == 0);
   printf("  PASS: test_no_entry_vs_error\n");
}

static void test_principal_isolation(void)
{
   uint8_t kek_a[VAULT_KEK_LEN], kek_b[VAULT_KEK_LEN];
   make_kek(kek_a, 10);
   make_kek(kek_b, 20);
   uint8_t salt_b[VAULT_SALT_LEN];
   assert(vault_store_get_or_create_salt("uid:2000", salt_b) == 0);
   assert(vault_store_set("uid:2000", kek_b, "claude", "api_key", "b-secret") == 0);

   /* uid:1000's cred is not visible under uid:2000, and uid:2000's KEK can't read
    * it (different file + different AAD principal). */
   char out[256];
   assert(vault_store_get("uid:2000", kek_b, "claude", "api_key", out, sizeof(out)) == 0);
   assert(strcmp(out, "b-secret") == 0);
   assert(vault_store_get("uid:2000", kek_a, "claude", "api_key", out, sizeof(out)) == -1);
   printf("  PASS: test_principal_isolation\n");
}

static void test_replace_list_delete(void)
{
   const char *principal = "uid:3000";
   uint8_t kek[VAULT_KEK_LEN];
   make_kek(kek, 3);
   uint8_t salt[VAULT_SALT_LEN];
   assert(vault_store_get_or_create_salt(principal, salt) == 0);
   assert(vault_store_set(principal, kek, "claude", "api_key", "v1") == 0);
   assert(vault_store_set(principal, kek, "claude", "api_key", "v2") == 0); /* replace */
   assert(vault_store_set(principal, kek, "openai", "api_key", "o1") == 0);

   char out[64];
   assert(vault_store_get(principal, kek, "claude", "api_key", out, sizeof(out)) == 0);
   assert(strcmp(out, "v2") == 0); /* latest wins, no duplicate */

   vault_store_entry_t entries[8];
   int n = vault_store_list(principal, entries, 8);
   assert(n == 2); /* claude + openai, not 3 */

   assert(vault_store_delete(principal, "claude", "api_key") == 0);
   assert(vault_store_get(principal, kek, "claude", "api_key", out, sizeof(out)) ==
          VAULT_STORE_NO_ENTRY);
   assert(vault_store_list(principal, entries, 8) == 1);
   printf("  PASS: test_replace_list_delete\n");
}

static void test_salt_is_stable(void)
{
   uint8_t s1[VAULT_SALT_LEN], s2[VAULT_SALT_LEN];
   assert(vault_store_get_or_create_salt("uid:4000", s1) == 0);
   assert(vault_store_get_or_create_salt("uid:4000", s2) == 0);
   assert(memcmp(s1, s2, VAULT_SALT_LEN) == 0); /* stable once created */
   printf("  PASS: test_salt_is_stable\n");
}

int main(void)
{
   snprintf(g_home, sizeof(g_home), "/tmp/aimee-vault-test-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", g_home, g_home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", g_home, 1);

   test_set_get_roundtrip();
   test_at_rest_is_ciphertext_only();
   test_wrong_kek_fails_closed();
   test_no_entry_vs_error();
   test_principal_isolation();
   test_replace_list_delete();
   test_salt_is_stable();

   char rm[320];
   snprintf(rm, sizeof(rm), "rm -rf %s", g_home);
   (void)system(rm);
   printf("vault_store: all tests passed\n");
   return 0;
}
