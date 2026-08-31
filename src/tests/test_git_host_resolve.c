/* test_git_host_resolve.c — the per-host vault seam: dormant when unregistered,
 * resolves a token from an explicit remote URL or from a repo's origin remote. */
#include "modules/git/git_host_resolve.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A fake per-host lookup: a token only for a URL whose host is "vault.example". */
static int fake_lookup(const char *url, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (url && strstr(url, "vault.example"))
   {
      snprintf(out, cap, "TOK-vaulted");
      return 1;
   }
   return 0;
}

int main(void)
{
   char tok[256];

   /* Unregistered → always 0 (per-host step skipped, falls through upstream). */
   assert(git_host_resolve_token("https://vault.example/x/y", NULL, tok, sizeof(tok)) == 0);
   assert(tok[0] == '\0');

   git_host_resolve_register(fake_lookup);

   /* Explicit remote URL: hit and miss. */
   assert(git_host_resolve_token("https://vault.example/x/y", NULL, tok, sizeof(tok)) == 1);
   assert(strcmp(tok, "TOK-vaulted") == 0);
   assert(git_host_resolve_token("https://other.example/x/y", NULL, tok, sizeof(tok)) == 0);
   assert(tok[0] == '\0');

   /* Origin-based resolution from a real repo (no remote_url given). */
   char dir[256];
   snprintf(dir, sizeof(dir), "/tmp/aimee-ghr-%d", (int)getpid());
   char setup[1024];
   snprintf(setup, sizeof(setup),
            "rm -rf %s && git init -q %s && git -C %s remote add origin "
            "https://vault.example/a/b.git",
            dir, dir, dir);
   assert(system(setup) == 0);
   assert(git_host_resolve_token(NULL, dir, tok, sizeof(tok)) == 1);
   assert(strcmp(tok, "TOK-vaulted") == 0);

   /* A repo with no origin remote → 0. */
   char dir2[256];
   snprintf(dir2, sizeof(dir2), "/tmp/aimee-ghr2-%d", (int)getpid());
   char setup2[512];
   snprintf(setup2, sizeof(setup2), "rm -rf %s && git init -q %s", dir2, dir2);
   assert(system(setup2) == 0);
   assert(git_host_resolve_token(NULL, dir2, tok, sizeof(tok)) == 0);

   /* THE MIRROR CONDITION. A mirror workspace's server-side checkout is
    * reconstructed under the mirror base, so the cwd the client handed over does
    * NOT exist on this host. Resolving by that path finds no origin and yields
    * no token — which upstream is indistinguishable from "no credential", and is
    * how `aimee git push` came to fail with "could not read Username" while
    * reads (which never exec git) kept working.
    *
    * So the caller must pass the workspace's RECORDED remote. These two
    * assertions pin both halves: the path alone is not enough, the remote is. */
   char absent[256];
   snprintf(absent, sizeof(absent), "/tmp/aimee-ghr-absent-%d", (int)getpid());
   assert(access(absent, F_OK) != 0); /* genuinely not present */

   assert(git_host_resolve_token(NULL, absent, tok, sizeof(tok)) == 0);
   assert(tok[0] == '\0');

   assert(git_host_resolve_token("https://vault.example/a/b.git", absent, tok, sizeof(tok)) == 1);
   assert(strcmp(tok, "TOK-vaulted") == 0);

   /* Dormant again after a NULL re-register. */
   git_host_resolve_register(NULL);
   assert(git_host_resolve_token("https://vault.example/x/y", NULL, tok, sizeof(tok)) == 0);

   char clean[600];
   snprintf(clean, sizeof(clean), "rm -rf %s %s", dir, dir2);
   assert(system(clean) == 0);
   printf("git_host_resolve: all tests passed\n");
   return 0;
}
