/* test_vault_store.c: WP-C.1 — the on-disk vault substrate. Runs against a
 * throwaway AIMEE_HOME so it exercises the real file path/format. Pins the
 * round-trip, the at-rest "ciphertext only" property, principal isolation, and
 * the fail-closed / no-entry distinctions the use-path depends on. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* memmem */
#endif
#include "vault_store.h"
#include "vault_crypto.h"
#include "cJSON.h"
#include <assert.h>
#include <dirent.h>
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

/* Count files directly under <home>/.vault/, and assert none escaped it. The
 * filename is base64url(principal), which has no '/', so an attacker-influenced
 * principal can neither traverse out of .vault/ nor introduce a subdirectory. */
static int count_vault_files(void)
{
   char dir[320];
   snprintf(dir, sizeof(dir), "%s/.vault", g_home);
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   int n = 0;
   struct dirent *e;
   while ((e = readdir(d)))
   {
      if (e->d_name[0] == '.')
         continue;
      /* No path separator may appear in a vault filename. */
      assert(strchr(e->d_name, '/') == NULL);
      n++;
   }
   closedir(d);
   return n;
}

/* BLOCKER 2 (roundtable): the store's headline claim — an attacker-influenced
 * principal name can neither traverse the path nor collide with another. */
static void test_attacker_principal_filename_safety(void)
{
   uint8_t kek[VAULT_KEK_LEN];
   make_kek(kek, 42);
   int before = count_vault_files();

   /* A traversal attempt round-trips AND lands strictly inside .vault/. */
   const char *evil = "webuser:../../../etc/escape";
   uint8_t salt[VAULT_SALT_LEN];
   assert(vault_store_get_or_create_salt(evil, salt) == 0);
   assert(vault_store_set(evil, kek, "claude", "api_key", "escape-secret") == 0);
   char out[64];
   assert(vault_store_get(evil, kek, "claude", "api_key", out, sizeof(out)) == 0);
   assert(strcmp(out, "escape-secret") == 0);
   /* Nothing leaked outside .vault/ (no traversal). */
   char escaped[400];
   snprintf(escaped, sizeof(escaped), "%s/../escape", g_home);
   assert(access(escaped, F_OK) != 0);
   snprintf(escaped, sizeof(escaped), "/etc/escape");
   assert(access(escaped, F_OK) != 0);

   /* Names a naive encoder might fold must resolve to DISTINCT files and never
    * read each other's secret. */
   assert(vault_store_get_or_create_salt("webuser:a/b", salt) == 0);
   assert(vault_store_get_or_create_salt("webuser:a_b", salt) == 0);
   assert(vault_store_set("webuser:a/b", kek, "claude", "api_key", "secret-AB-slash") == 0);
   assert(vault_store_set("webuser:a_b", kek, "claude", "api_key", "secret-AB-under") == 0);
   assert(vault_store_get("webuser:a/b", kek, "claude", "api_key", out, sizeof(out)) == 0);
   assert(strcmp(out, "secret-AB-slash") == 0); /* not overwritten by a_b */
   assert(vault_store_get("webuser:a_b", kek, "claude", "api_key", out, sizeof(out)) == 0);
   assert(strcmp(out, "secret-AB-under") == 0);

   /* Case must not fold either. */
   assert(vault_store_get_or_create_salt("webuser:Alice", salt) == 0);
   assert(vault_store_get_or_create_salt("webuser:alice", salt) == 0);
   assert(vault_store_set("webuser:Alice", kek, "claude", "api_key", "upper") == 0);
   assert(vault_store_set("webuser:alice", kek, "claude", "api_key", "lower") == 0);
   assert(vault_store_get("webuser:Alice", kek, "claude", "api_key", out, sizeof(out)) == 0);
   assert(strcmp(out, "upper") == 0);

   /* 5 distinct principals touched here => 5 new flat files, all inside .vault/. */
   assert(count_vault_files() == before + 5);
   printf("  PASS: test_attacker_principal_filename_safety\n");
}

/* Locate the vault file for `principal` by matching the stored "principal"
 * field; returns 1 + path on success. */
static int find_vault_file(const char *principal, char *path, size_t cap)
{
   char dir[320];
   snprintf(dir, sizeof(dir), "%s/.vault", g_home);
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   struct dirent *e;
   int found = 0;
   while (!found && (e = readdir(d)))
   {
      if (e->d_name[0] == '.')
         continue;
      char p[640];
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      FILE *f = fopen(p, "rb");
      if (!f)
         continue;
      char buf[8192];
      size_t n = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      buf[n] = '\0';
      cJSON *root = cJSON_Parse(buf);
      cJSON *jp = root ? cJSON_GetObjectItemCaseSensitive(root, "principal") : NULL;
      if (cJSON_IsString(jp) && strcmp(jp->valuestring, principal) == 0)
      {
         snprintf(path, cap, "%s", p);
         found = 1;
      }
      cJSON_Delete(root);
   }
   closedir(d);
   return found;
}

/* BLOCKER 3 (roundtable): build_aad must bind agent+cred WITHIN a principal, so
 * an intra-principal slot swap (two of the user's own agents) fails closed. */
static void test_intra_principal_aad_swap(void)
{
   const char *p = "uid:9100";
   uint8_t kek[VAULT_KEK_LEN];
   make_kek(kek, 91);
   uint8_t salt[VAULT_SALT_LEN];
   assert(vault_store_get_or_create_salt(p, salt) == 0);
   assert(vault_store_set(p, kek, "claude", "api_key", "CLAUDE-SECRET") == 0);
   assert(vault_store_set(p, kek, "openai", "api_key", "OPENAI-SECRET") == 0);

   /* Read the file, swap the four crypto fields between the two entries while
    * keeping each entry's agent/cred labels, write back. */
   char path[640];
   assert(find_vault_file(p, path, sizeof(path)));
   FILE *f = fopen(path, "rb");
   assert(f);
   char buf[16384];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   cJSON *root = cJSON_Parse(buf);
   assert(root);
   cJSON *creds = cJSON_GetObjectItemCaseSensitive(root, "creds");
   assert(cJSON_IsArray(creds) && cJSON_GetArraySize(creds) == 2);
   cJSON *e0 = cJSON_GetArrayItem(creds, 0);
   cJSON *e1 = cJSON_GetArrayItem(creds, 1);
   const char *fields[] = {"wrapped_dek", "nonce", "ciphertext", "tag"};
   for (int i = 0; i < 4; i++)
   {
      cJSON *a = cJSON_GetObjectItemCaseSensitive(e0, fields[i]);
      cJSON *b = cJSON_GetObjectItemCaseSensitive(e1, fields[i]);
      char *av = strdup(a->valuestring), *bv = strdup(b->valuestring);
      cJSON_SetValuestring(a, bv);
      cJSON_SetValuestring(b, av);
      free(av);
      free(bv);
   }
   char *txt = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   f = fopen(path, "wb");
   assert(f);
   fwrite(txt, 1, strlen(txt), f);
   fclose(f);
   free(txt);

   /* Both entries now carry the OTHER's ciphertext under their own (agent,cred)
    * AAD -> the GCM tag fails to authenticate -> fail closed, never the swapped
    * secret. */
   char out[64];
   assert(vault_store_get(p, kek, "claude", "api_key", out, sizeof(out)) == -1);
   assert(vault_store_get(p, kek, "openai", "api_key", out, sizeof(out)) == -1);
   printf("  PASS: test_intra_principal_aad_swap\n");
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
   test_attacker_principal_filename_safety();
   test_intra_principal_aad_swap();

   char rm[320];
   snprintf(rm, sizeof(rm), "rm -rf %s", g_home);
   (void)system(rm);
   printf("vault_store: all tests passed\n");
   return 0;
}
