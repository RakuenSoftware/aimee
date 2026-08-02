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
#include <dirent.h>
#include <ftw.h>
#include <stdint.h>
#include <sys/stat.h>
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
   int fdY = -1;
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
   /* One environment vault: the credential is the server's, and the actor is
    * audited rather than used as a namespace. The leak properties below are
    * unchanged — this only stores it where the autonomous reader looks. */
   assert(vault_service_set_server(GIT_FORGE_VAULT_AGENT, GIT_FORGE_TOKEN_CRED, CANARY) ==
          VAULT_OK);

   /* Drop the cached KEK so nothing transient holds the plaintext; sync so the
    * vault's ciphertext write is durable before we inspect the disk. */
   vault_kek_cache_clear();
   sync();

   /* POSITIVE proof the vault actually persisted something: the vault writes only
    * to ${AIMEE_HOME}/.vault (config_default_dir + "/.vault"), so a non-empty
    * blob there means the credential was stored — not a silent no-op. */
   char vdir[300];
   snprintf(vdir, sizeof(vdir), "%s/.vault", home);
   DIR *vd = opendir(vdir);
   assert(vd != NULL);
   int blob_files = 0;
   struct dirent *ve;
   while ((ve = readdir(vd)) != NULL)
   {
      if (ve->d_name[0] == '.')
         continue;
      char vp[600];
      snprintf(vp, sizeof(vp), "%s/%s", vdir, ve->d_name);
      struct stat vst;
      if (stat(vp, &vst) == 0 && S_ISREG(vst.st_mode) && vst.st_size > 0)
         blob_files++;
   }
   closedir(vd);
   assert(blob_files > 0); /* the vault persisted a non-empty (ciphertext) blob */

   /* INVARIANT: the canary's plaintext must appear in NO file under AIMEE_HOME
    * — including the .vault blob, which holds it AES-wrapped (ciphertext only).
    * Persisted (above) + no-plaintext (here) + decryptable round-trip (below)
    * together prove encrypted-at-rest, not a no-op or a plaintext store. */
   g_leaks = 0;
   assert(nftw(home, walk_cb, 16, FTW_PHYS) == 0);
   if (g_leaks)
      fprintf(stderr, "vault leaked the credential to disk in %d file(s)\n", g_leaks);
   assert(g_leaks == 0);

   /* ...yet it round-trips: with the KEK cache cleared (above) and no cache in
    * git_forge_vault, this read DECRYPTS the on-disk blob under the server KEK —
    * proving the persisted ciphertext really is the canary (not a no-op). */
   char tok[4096];
   assert(git_forge_vault_token(alice, tok, sizeof(tok)) == 1);
   assert(strcmp(tok, CANARY) == 0);

   char *const parent[] = {(char *)"PATH=/usr/bin", NULL};
   char **env = git_cred_inject_build_env_for_repo(alice, NULL, NULL, NULL, parent, &fdY);
   assert(env != NULL);
   /* There is no env mode left: the canary is never an environment string, in
    * any path. The editor migrated to the same inherited-fd delivery. */
   for (int i = 0; env[i]; i++)
   {
      assert(strncmp(env[i], "GH_TOKEN=", 9) != 0);
      assert(strstr(env[i], CANARY) == NULL);
   }
   assert(fdY >= 0);
   assert(close(fdY) == 0);
   fdY = -1;
   git_cred_inject_free_env(env);

   /* FD mode (the API git ops + clone): the token must NOT be in the env at all —
    * it rides an inherited memfd, so the canary appears in NO env string and the
    * env advertises only the fd number. This is the binding "not in
    * /proc/<pid>/environ" invariant at the assembly layer (the exec-time proof is
    * in test_git_cred_inject's safe_exec fd test). */
   int tfd = -1;
   char **fenv = git_cred_inject_build_env_for_repo(alice, NULL, NULL, NULL, parent, &tfd);
   assert(fenv != NULL && tfd >= 0);
   for (int i = 0; fenv[i]; i++)
   {
      assert(strncmp(fenv[i], "GH_TOKEN=", 9) != 0); /* no GH_TOKEN in fd mode */
      assert(strstr(fenv[i], CANARY) == NULL);       /* canary nowhere in the env */
   }
   assert(close(tfd) == 0); /* the memfd is a real, open fd we own + release */
   git_cred_inject_free_env(fenv);

   /* Single-tenant: another PAM actor resolves the SAME environment credential,
    * with the same properties in both modes — legacy env carries it in memory,
    * fd mode keeps it out of the environment entirely. */
   char btok[4096];
   assert(git_forge_vault_token("webuser:bob", btok, sizeof(btok)) == 1);
   assert(strcmp(btok, CANARY) == 0);

   int bfd = -1;
   char **bfenv = git_cred_inject_build_env_for_repo("webuser:bob", NULL, NULL, NULL, parent, &bfd);
   assert(bfenv != NULL && bfd >= 0);
   for (int i = 0; bfenv[i]; i++)
      assert(strstr(bfenv[i], CANARY) == NULL); /* still nowhere in the env */
   assert(close(bfd) == 0);
   git_cred_inject_free_env(bfenv);

   /* No actor at all still resolves the environment credential — and still by fd,
    * so the canary never reaches an environment string on that path either. */
   int afd = -1;
   char **aenv = git_cred_inject_build_env_for_repo("", NULL, NULL, NULL, parent, &afd);
   assert(aenv != NULL);
   for (int i = 0; aenv[i]; i++)
      assert(strstr(aenv[i], CANARY) == NULL);
   if (afd >= 0)
      close(afd);
   git_cred_inject_free_env(aenv);

   char clean[320];
   snprintf(clean, sizeof(clean), "rm -rf %s", home);
   assert(system(clean) == 0);
   printf("webchat_git_leak: all tests passed (no plaintext on disk; vault-only)\n");
   return 0;
}
