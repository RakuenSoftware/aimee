/* test_workspace_client_diff.c: the working-tree patch shipped for a server-side
 * reconstruction is either whole or absent, never truncated. A patch cut at the
 * capture cap cannot be applied, which left delegates with no usable tree. */
#include <aimee/workspace/client_diff.h>
#include "platform_test_util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sh(const char *cmd)
{
   int rc = system(cmd);
   assert(rc == 0);
}

int main(void)
{
   char repo[512], cmd[2048];
   snprintf(repo, sizeof repo, "%s/aimee-test-clientdiff-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(repo));
   snprintf(cmd, sizeof cmd,
            "cd '%s' && git init -q && git config user.email t@t && git config user.name t && "
            "echo one > a.txt && git add a.txt && git commit -qm base",
            repo);
   sh(cmd);
   char base[64] = "";
   snprintf(cmd, sizeof cmd, "git -C '%s' rev-parse HEAD", repo);
   FILE *p = popen(cmd, "r");
   assert(p && fgets(base, sizeof base, p));
   pclose(p);
   base[strcspn(base, "\n")] = '\0';

   /* A small change ships as a whole patch. */
   snprintf(cmd, sizeof cmd, "echo two >> '%s/a.txt'", repo);
   sh(cmd);
   char *patch = workspace_client_diff_compute(repo, base);
   assert(patch && strstr(patch, "+two"));
   free(patch);

   /* Over the cap (an untracked 17 MiB file): no patch rather than a cut one. */
   snprintf(cmd, sizeof cmd, "head -c 17825792 /dev/urandom | base64 > '%s/big.txt'", repo);
   sh(cmd);
   patch = workspace_client_diff_compute(repo, base);
   assert(patch == NULL);

   snprintf(cmd, sizeof cmd, "rm -rf '%s'", repo);
   sh(cmd);
   puts("workspace_client_diff: whole patch or none, never truncated");
   return 0;
}
