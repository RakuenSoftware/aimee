/* test_vault_master_rotate.c — D13 master-key rotation.
 *
 * Proves vault_server_key_rotate() re-wraps every server-KEK-held wrap from the
 * old `.server-master.key` to a freshly minted one (a re-wrap, not a re-encrypt):
 *   - server-principal creds (server KEK in the PRIMARY "wrapped_dek") survive;
 *   - a user dual-access cred's SERVER wrap ("wrapped_dek_server") is re-wrapped
 *     while its USER wrap ("wrapped_dek") is left untouched;
 *   - nothing decrypts under the OLD server KEK afterwards;
 *   - the on-disk master key actually changed; a recoverable backup was taken;
 *   - a no-master-key vault is a clean no-op (exit 0);
 *   - a corrupted server wrap makes rotation ABORT fail-closed (master unchanged). */
#include "vault_service.h"
#include "vault_server_key.h"
#include "vault_store.h"
#include "vault_crypto.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_home[256];

static int read_master_key(uint8_t out[VAULT_ROOT_KEY_LEN])
{
   char path[400];
   snprintf(path, sizeof(path), "%s/.vault/.server-master.key", g_home);
   FILE *f = fopen(path, "rb");
   if (!f)
      return -1;
   size_t n = fread(out, 1, VAULT_ROOT_KEY_LEN, f);
   fclose(f);
   return n == VAULT_ROOT_KEY_LEN ? 0 : -1;
}

static int dir_has_regular_file(const char *dir)
{
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   int found = 0;
   struct dirent *de;
   while ((de = readdir(d)) != NULL)
   {
      if (de->d_name[0] == '.')
         continue;
      found = 1;
      break;
   }
   closedir(d);
   return found;
}

static void fresh_home(const char *tag)
{
   snprintf(g_home, sizeof(g_home), "/tmp/aimee-vaultrotate-%s-%d", tag, (int)getpid());
   char cmd[640];
   snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", g_home, g_home);
   assert(system(cmd) == 0);
   setenv("AIMEE_HOME", g_home, 1);
   vault_server_key_reset_for_test();
}

static void cleanup_home(void)
{
   char cmd[640];
   snprintf(cmd, sizeof(cmd), "rm -rf %s %s.rotate-bak.* 2>/dev/null", g_home, g_home);
   (void)system(cmd);
}

/* Flip one base64url char of the first `field` value in the server-principal
 * vault file so it still decodes but fails the AES-KW integrity check. */
static void corrupt_field(const char *field)
{
   char vpath[512];
   /* b64url("server") = "c2VydmVy" */
   snprintf(vpath, sizeof(vpath), "%s/.vault/c2VydmVy.json", g_home);
   FILE *f = fopen(vpath, "rb");
   assert(f);
   char buf[8192];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);
   char needle[64];
   snprintf(needle, sizeof(needle), "\"%s\":\"", field);
   char *p = strstr(buf, needle);
   assert(p && "field not found to corrupt");
   p += strlen(needle);
   *p = (*p == 'A') ? 'B' : 'A'; /* still a valid b64url char */
   f = fopen(vpath, "wb");
   assert(f);
   fwrite(buf, 1, n, f);
   fclose(f);
}

/* ── Scenario: no master key yet → clean no-op ──────────────────────────────── */
static void test_no_master(void)
{
   fresh_home("nomaster");
   int np = -1, nc = -1;
   char backup[1280] = "x", err[256] = "";
   int rc = vault_server_key_rotate(VAULT_SERVER_PRINCIPAL, &np, &nc, backup, sizeof(backup), err,
                                    sizeof(err));
   assert(rc == 0 && "no-master rotation should be a clean no-op");
   assert(np == 0 && nc == 0 && "nothing to rotate");
   assert(backup[0] == '\0' && "no backup for an empty vault");
   cleanup_home();
   printf("  PASS: no master key -> clean no-op\n");
}

/* ── Scenario: a corrupted server wrap aborts fail-closed ───────────────────── */
static void test_corrupt_wrap_aborts(void)
{
   fresh_home("corrupt");
   assert(vault_service_set_server("agentA", VAULT_API_KEY_CRED, "sk-corrupt-test") == VAULT_OK);
   uint8_t mk_before[VAULT_ROOT_KEY_LEN];
   assert(read_master_key(mk_before) == 0);
   corrupt_field("wrapped_dek");

   int np = -1, nc = -1;
   char backup[1280] = "", err[256] = "";
   int rc = vault_server_key_rotate(VAULT_SERVER_PRINCIPAL, &np, &nc, backup, sizeof(backup), err,
                                    sizeof(err));
   assert(rc != 0 && "rotation must abort on a corrupt/unwrappable server wrap");

   uint8_t mk_after[VAULT_ROOT_KEY_LEN];
   assert(read_master_key(mk_after) == 0);
   assert(memcmp(mk_before, mk_after, VAULT_ROOT_KEY_LEN) == 0 &&
          "master key must be UNCHANGED after an aborted rotation");
   cleanup_home();
   printf("  PASS: corrupt server wrap -> abort, master unchanged\n");
}

/* ── Scenario: server creds + a user dual-access cred all rotate correctly ──── */
static void test_full_rotation(void)
{
   fresh_home("full");
   const char *secretA = "sk-rotateA-3f9c2e";
   const char *secretB = "sk-rotateB-7a1d8b";
   const char *secretC = "sk-dual-C-1a2b3c";

   /* Two server-principal creds (server KEK is their PRIMARY wrap). */
   assert(vault_service_set_server("agentA", VAULT_API_KEY_CRED, secretA) == VAULT_OK);
   assert(vault_service_set_server("agentB", VAULT_API_KEY_CRED, secretB) == VAULT_OK);

   /* A user principal with a DUAL-access cred: a user wrap (wrapped_dek under a
    * user KEK) plus a server wrap (wrapped_dek_server under the server KEK). Only
    * the server wrap must be re-wrapped; the user wrap must be left untouched. */
   uint8_t old_kek[VAULT_KEK_LEN];
   assert(vault_server_kek(old_kek) == 0);
   uint8_t user_root[VAULT_ROOT_KEY_LEN];
   memset(user_root, 0x5a, sizeof(user_root));
   uint8_t user_salt[VAULT_SALT_LEN];
   memset(user_salt, 0x11, sizeof(user_salt));
   uint8_t user_kek[VAULT_KEK_LEN];
   assert(vault_kek_derive(user_root, sizeof(user_root), user_salt, sizeof(user_salt), user_kek) ==
          0);
   /* The principal's vault file must exist before a set; the stored salt is
    * unused here since we pass the derived KEK directly. */
   uint8_t throwaway_salt[VAULT_SALT_LEN];
   assert(vault_store_get_or_create_salt("uid:9999", throwaway_salt) == 0);
   assert(vault_store_set_dual("uid:9999", user_kek, old_kek, "agentC", VAULT_API_KEY_CRED,
                               secretC) == 0);

   uint8_t mk_before[VAULT_ROOT_KEY_LEN];
   assert(read_master_key(mk_before) == 0);

   int np = -1, nc = -1;
   char backup[1280] = "", err[256] = "";
   int rc = vault_server_key_rotate(VAULT_SERVER_PRINCIPAL, &np, &nc, backup, sizeof(backup), err,
                                    sizeof(err));
   assert(rc == 0 && "rotation failed");
   assert(nc == 3 && "expected 3 server-KEK wraps re-wrapped (agentA, agentB, agentC-server)");
   assert(np >= 2 && "expected the server principal + uid:9999");
   printf("  rotate: %d principal(s), %d wrap(s) re-wrapped, backup=%s\n", np, nc, backup);

   uint8_t mk_after[VAULT_ROOT_KEY_LEN];
   assert(read_master_key(mk_after) == 0);
   assert(memcmp(mk_before, mk_after, VAULT_ROOT_KEY_LEN) != 0 && "master key did not change");
   assert(backup[0] && dir_has_regular_file(backup) && "backup missing/empty");

   /* New server KEK reads the server-principal creds; old KEK is dead. */
   vault_server_key_reset_for_test();
   uint8_t new_kek[VAULT_KEK_LEN];
   assert(vault_server_kek(new_kek) == 0);
   assert(memcmp(old_kek, new_kek, VAULT_KEK_LEN) != 0 && "server KEK did not change");

   char out[256];
   assert(vault_store_get(VAULT_SERVER_PRINCIPAL, new_kek, "agentA", VAULT_API_KEY_CRED, out,
                          sizeof(out)) == 0 &&
          strcmp(out, secretA) == 0);
   assert(vault_store_get(VAULT_SERVER_PRINCIPAL, new_kek, "agentB", VAULT_API_KEY_CRED, out,
                          sizeof(out)) == 0 &&
          strcmp(out, secretB) == 0);
   assert(vault_store_get(VAULT_SERVER_PRINCIPAL, old_kek, "agentA", VAULT_API_KEY_CRED, out,
                          sizeof(out)) != 0 &&
          "old server KEK still decrypts a server-principal cred");

   /* The dual cred's SERVER wrap re-wrapped to the new KEK; old KEK dead there too. */
   assert(vault_store_get_server("uid:9999", new_kek, "agentC", VAULT_API_KEY_CRED, out,
                                 sizeof(out)) == 0 &&
          strcmp(out, secretC) == 0 && "dual server wrap did not survive under the new KEK");
   assert(vault_store_get_server("uid:9999", old_kek, "agentC", VAULT_API_KEY_CRED, out,
                                 sizeof(out)) != 0 &&
          "old server KEK still decrypts the dual server wrap");

   /* The USER wrap was NOT touched — still decrypts under the unchanged user KEK. */
   assert(vault_store_get("uid:9999", user_kek, "agentC", VAULT_API_KEY_CRED, out, sizeof(out)) ==
              0 &&
          strcmp(out, secretC) == 0 && "user wrap was altered by master-key rotation");

   cleanup_home();
   printf("  PASS: server + dual-access rotation; user wrap preserved\n");
}

int main(void)
{
   test_no_master();
   test_corrupt_wrap_aborts();
   test_full_rotation();
   printf("PASS: vault master-key rotation (D13)\n");
   return 0;
}
