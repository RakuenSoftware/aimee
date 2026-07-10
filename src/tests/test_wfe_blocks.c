/* test_wfe_blocks.c -- W3: the real git freeze helper + default executor
 * registration. The delegate/forge executors are integration-gated. */
#include "wfe_test_home.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wfe_blocks.h"
#include "wfe_iface.h"

static int sh(const char *cmd)
{
   return system(cmd);
}

int main(void)
{
   printf("wfe-blocks: ");

   /* --- default executors registered for every non-gate block --- */
   wfe_reset_block_executors();
   wfe_register_default_executors();
   assert(wfe_lookup_block_executor(WFE_BLK_AUTHOR_PROPOSAL));
   assert(wfe_lookup_block_executor(WFE_BLK_AUTHOR_PLAN));
   assert(wfe_lookup_block_executor(WFE_BLK_IMPLEMENT));
   assert(wfe_lookup_block_executor(WFE_BLK_FREEZE));
   assert(wfe_lookup_block_executor(WFE_BLK_PR_OPEN));
   assert(wfe_lookup_block_executor(WFE_BLK_MERGE));

   /* --- wfe_git_freeze against a real temp git repo --- */
   char dir[] = "/tmp/wfe_repo_XXXXXX";
   if (!wfe_test_mkdtemp(dir))
   {
      printf("(skip git freeze: mkdtemp) ok\n");
      return 0;
   }
   char cmd[2048];
   snprintf(cmd, sizeof cmd,
            "cd %s && git init -q && git config user.email t@t && git config user.name t && "
            "git config commit.gpgsign false && printf 'a\\n' > f.txt && git add -A && "
            "git commit -q -m base",
            dir);
   if (sh(cmd) != 0)
   {
      printf("(skip git freeze: no git) ok\n");
      return 0;
   }

   char base[64] = "", head[64] = "", h_empty[65] = "", err[128] = "";
   /* base commit only: base == head, diff is empty */
   assert(wfe_git_freeze(dir, "HEAD", base, head, h_empty, err, sizeof err) == 0);
   assert(base[0] && head[0]);
   assert(strcmp(base, head) == 0);
   assert(strlen(h_empty) == 64);

   /* new commit on a branch: head != base, diff hash differs */
   snprintf(cmd, sizeof cmd,
            "cd %s && git checkout -q -b feat && printf 'a\\nb\\n' > f.txt && git add -A && "
            "git commit -q -m change",
            dir);
   assert(sh(cmd) == 0);
   char base2[64] = "", head2[64] = "", h_change[65] = "";
   assert(wfe_git_freeze(dir, "master", base2, head2, h_change, err, sizeof err) == 0 ||
          wfe_git_freeze(dir, "main", base2, head2, h_change, err, sizeof err) == 0);
   assert(strcmp(base2, head2) != 0);      /* diverged from base branch */
   assert(strcmp(h_change, h_empty) != 0); /* a real diff -> different hash */

   /* deterministic: same state -> same hash */
   char b3[64] = "", h3[64] = "", hh3[65] = "";
   const char *bb = base2[0] ? "master" : "main";
   wfe_git_freeze(dir, bb, b3, h3, hh3, err, sizeof err);
   assert(strcmp(hh3, h_change) == 0);

   snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
   sh(cmd);
   printf("ok\n");
   return 0;
}
