#define _GNU_SOURCE 1
/* test_webchat_git_leak.c — WP-G Phase-1 gate: the vault-only-at-rest invariant.
 * A git credential a webchat user stores must live ONLY inside the encrypted
 * vault — its plaintext must appear in NO file on disk — yet still be available
 * in-memory to the credential-injection env. Plus the cross-principal denial. */
#include "git_cred_inject.h"
#include "git_forge_vault.h"
#include "vault_kek_cache.h"
#include "vault_service.h"

#include <assert.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The unique canary we store; if its plaintext shows up in any file, the vault
 * leaked it to disk. */
static const char *CANARY = "ghp_LEAKCANARY_d41d8cd98f00b204e9800998";
static int g_leaks = 0;

/* Read the whole (small) file and search its bytes for `needle`. Files under a
 * test AIMEE_HOME are tiny (vault blobs < 1 KiB); cap defensively at 16 MiB. */
static int file_contains(const char *path, const char *needle)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return 0;
   size_t nl = strlen(needle);
   char *buf = malloc(16u << 20);
   if (!buf)
   {
      fclose(f);
      return 0;
   }
   size_t total = fread(buf, 1, 16u << 20, f);
   fclose(f);
   int found = 0;
   if (total >= nl)
      for (size_t i = 0; i + nl <= total; i++)
         if (memcmp(buf + i, needle, nl) == 0)
         {
            found = 1;
            break;
         }
   free(buf);
   return found;
}

static int walk_cb(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
   (void)st;
   (void)ftw;
   if (type == FTW_F && file_contains(path, CANARY))
   {
      fprintf(stderr, "  LEAK: canary plaintext found in %s\n", path);
      g_leaks++;
   }
   return 0;
}

int main(void)
{
   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-gitleak-%d", (int)getpid());
   char mk[320];
   snprintf(mk, sizeof(mk), "rm -rf %s && mkdir -p %s", home, home);
   assert(system(mk) == 0);
   setenv("AIMEE_HOME", home, 1);
   vault_kek_cache_clear();

   const long T0 = 100000;
   const char *alice = "webuser:alice";

   /* Store the canary as alice's git token (the WP-B intake path). */
   const uint8_t pw[] = "alice-pw";
   assert(vault_service_unlock_password(alice, ATTEST_WEBCHAT_TRUSTED, pw, sizeof(pw) - 1, T0) ==
          VAULT_OK);
   assert(vault_service_set(alice, GIT_FORGE_VAULT_AGENT, GIT_FORGE_TOKEN_CRED, CANARY, T0) ==
          VAULT_OK);

   /* Drop the cached KEK so nothing transient holds the plaintext, then sync the
    * vault file to disk by walking the whole AIMEE_HOME tree. */
   vault_kek_cache_clear();

   /* INVARIANT: the canary's plaintext must appear in NO file under AIMEE_HOME
    * (the vault stores it AES-wrapped; the .vault blob is ciphertext). */
   g_leaks = 0;
   assert(nftw(home, walk_cb, 16, FTW_PHYS) == 0);
   if (g_leaks)
      fprintf(stderr, "vault leaked the credential to disk in %d file(s)\n", g_leaks);
   assert(g_leaks == 0);

   /* ...yet it is still resolvable in-memory for credential injection. */
   char tok[4096];
   assert(git_forge_vault_token(alice, tok, sizeof(tok)) == 1);
   assert(strcmp(tok, CANARY) == 0);

   char *const parent[] = {(char *)"PATH=/usr/bin", NULL};
   char **env = git_cred_inject_build_env(alice, parent);
   assert(env != NULL);
   int has = 0;
   for (int i = 0; env[i]; i++)
      if (strncmp(env[i], "GH_TOKEN=", 9) == 0 && strcmp(env[i] + 9, CANARY) == 0)
         has = 1;
   assert(has); /* the env carries it (in memory only) */
   git_cred_inject_free_env(env);

   /* Cross-principal denial: bob has no token and cannot read alice's. */
   char btok[64];
   assert(git_forge_vault_token("webuser:bob", btok, sizeof(btok)) == 0);
   assert(git_cred_inject_build_env("webuser:bob", parent) == NULL);

   char clean[320];
   snprintf(clean, sizeof(clean), "rm -rf %s", home);
   assert(system(clean) == 0);
   printf("webchat_git_leak: all tests passed (no plaintext on disk; vault-only)\n");
   return 0;
}
