/* test_vault_master_rotate.c — D13 master-key rotation.
 *
 * Proves vault_server_key_rotate() re-wraps every server wrap from the old
 * `.server-master.key` to a freshly minted one (a re-wrap, not a re-encrypt):
 *   - all server-principal credentials still decrypt under the NEW server KEK;
 *   - they NO LONGER decrypt under the OLD server KEK (the old key is dead);
 *   - the on-disk master key actually changed;
 *   - a recoverable pre-rotation backup of .vault was taken;
 *   - the reported re-wrap count matches the number of stored credentials. */
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

int main(void)
{
   snprintf(g_home, sizeof(g_home), "/tmp/aimee-vaultrotate-test-%d", (int)getpid());
   char cmd[640];
   snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", g_home, g_home);
   assert(system(cmd) == 0);
   setenv("AIMEE_HOME", g_home, 1);
   vault_server_key_reset_for_test();

   const char *secretA = "sk-rotateA-3f9c2e";
   const char *secretB = "sk-rotateB-7a1d8b";

   /* Two server-principal credentials — this mints the master key + server wraps. */
   assert(vault_service_set_server("agentA", VAULT_API_KEY_CRED, secretA) == VAULT_OK);
   assert(vault_service_set_server("agentB", VAULT_API_KEY_CRED, secretB) == VAULT_OK);

   /* Capture the OLD server KEK and master key. */
   uint8_t old_kek[VAULT_KEK_LEN];
   assert(vault_server_kek(old_kek) == 0);
   uint8_t mk_before[VAULT_ROOT_KEY_LEN];
   assert(read_master_key(mk_before) == 0);

   /* Rotate. */
   int np = -1, nc = -1;
   char backup[1280] = "", err[256] = "";
   int rc = vault_server_key_rotate(VAULT_SERVER_PRINCIPAL, &np, &nc, backup, sizeof(backup), err,
                                    sizeof(err));
   assert(rc == 0 && "rotation failed");
   assert(nc == 2 && "expected exactly 2 credentials re-wrapped");
   assert(np >= 1 && "expected at least the server principal");
   printf("  rotate: %d principal(s), %d cred(s) re-wrapped, backup=%s\n", np, nc, backup);

   /* The on-disk master key actually changed. */
   uint8_t mk_after[VAULT_ROOT_KEY_LEN];
   assert(read_master_key(mk_after) == 0);
   assert(memcmp(mk_before, mk_after, VAULT_ROOT_KEY_LEN) != 0 && "master key did not change");

   /* A recoverable pre-rotation backup exists and is non-empty. */
   assert(backup[0] && "no backup path reported");
   assert(dir_has_regular_file(backup) && "backup directory is empty");

   /* Re-derive the server KEK from the NEW master and confirm both creds decrypt. */
   vault_server_key_reset_for_test();
   uint8_t new_kek[VAULT_KEK_LEN];
   assert(vault_server_kek(new_kek) == 0);
   assert(memcmp(old_kek, new_kek, VAULT_KEK_LEN) != 0 && "server KEK did not change");

   /* Server-principal creds hold the server KEK as their PRIMARY wrap, so the
    * read path is vault_store_get (wrapped_dek), not _get_server. */
   char out[256];
   assert(vault_store_get(VAULT_SERVER_PRINCIPAL, new_kek, "agentA", VAULT_API_KEY_CRED, out,
                          sizeof(out)) == 0);
   assert(strcmp(out, secretA) == 0 && "agentA did not survive rotation under the new KEK");
   assert(vault_store_get(VAULT_SERVER_PRINCIPAL, new_kek, "agentB", VAULT_API_KEY_CRED, out,
                          sizeof(out)) == 0);
   assert(strcmp(out, secretB) == 0 && "agentB did not survive rotation under the new KEK");

   /* The OLD KEK is now dead — it must NOT decrypt the re-wrapped server wrap. */
   assert(vault_store_get(VAULT_SERVER_PRINCIPAL, old_kek, "agentA", VAULT_API_KEY_CRED, out,
                          sizeof(out)) != 0 &&
          "old server KEK still decrypts after rotation");

   snprintf(cmd, sizeof(cmd), "rm -rf %s %s", g_home, backup);
   (void)system(cmd);

   printf("PASS: vault master-key rotation re-wraps all server wraps (D13)\n");
   return 0;
}
