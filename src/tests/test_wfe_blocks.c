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

/* Capture `git -C dir rev-parse HEAD` into out[64]. Returns 0 on success. */
static int git_head(const char *dir, char out[64])
{
   char cmd[1200];
   snprintf(cmd, sizeof cmd, "git -C %s rev-parse HEAD 2>/dev/null", dir);
   FILE *p = popen(cmd, "r");
   if (!p)
      return -1;
   char buf[128] = "";
   char *got = fgets(buf, sizeof buf, p);
   pclose(p);
   if (!got)
      return -1;
   size_t n = strlen(buf);
   while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
      buf[--n] = '\0';
   snprintf(out, 64, "%s", buf);
   return out[0] ? 0 : -1;
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

   /* --- wfe_repo_local: the work item's repo wins when it names a local dir --- */
   {
      unsetenv("AIMEE_WORKFLOW_REPO");
      assert(strcmp(wfe_repo_local("/tmp"), "/tmp") == 0);        /* local dir -> itself */
      assert(strcmp(wfe_repo_local("/no/such/dir-x"), ".") == 0); /* absent -> env/cwd */
      assert(strcmp(wfe_repo_local("org/repo"), ".") == 0);       /* identifier -> env/cwd */
      assert(strcmp(wfe_repo_local(""), ".") == 0);
      assert(strcmp(wfe_repo_local(NULL), ".") == 0);
      setenv("AIMEE_WORKFLOW_REPO", "/srv/shared", 1);
      assert(strcmp(wfe_repo_local("/tmp"), "/tmp") == 0); /* per-item repo still wins */
      assert(strcmp(wfe_repo_local("org/repo"), "/srv/shared") == 0);
      unsetenv("AIMEE_WORKFLOW_REPO");
   }

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

   /* --- TDD anti-deletion guard: red-authored tests must survive GREEN --- */
   /* red commit: add a test file (currently on branch feat). */
   snprintf(cmd, sizeof cmd,
            "cd %s && printf 'test\\n' > test_x.c && git add -A && git commit -q -m red", dir);
   assert(sh(cmd) == 0);
   char red[64] = "";
   assert(git_head(dir, red) == 0);

   /* good GREEN: change production code, keep the test -> survives. */
   snprintf(cmd, sizeof cmd,
            "cd %s && printf 'a\\nb\\nc\\n' > f.txt && git add -A && git commit -q -m green_good",
            dir);
   assert(sh(cmd) == 0);
   assert(wfe_tdd_tests_survive(dir, red) == 1);

   /* bad GREEN: delete the red-authored test -> does not survive. */
   snprintf(cmd, sizeof cmd, "cd %s && git rm test_x.c >/dev/null && git commit -q -m green_bad",
            dir);
   assert(sh(cmd) == 0);
   assert(wfe_tdd_tests_survive(dir, red) == 0);

   /* not applicable (empty args / unresolvable) -> permissive (1), the freeze +
    * aggregate verify are the backstop. */
   assert(wfe_tdd_tests_survive(dir, "") == 1);
   assert(wfe_tdd_tests_survive("", red) == 1);
   assert(wfe_tdd_tests_survive(dir, "0000000000000000000000000000000000000000") == 1);

   /* --- source.archive: content-addressed retire of the triggering file --- */
   {
      assert(wfe_lookup_block_executor(WFE_BLK_SOURCE_ARCHIVE));
      snprintf(cmd, sizeof cmd,
               "cd %s && mkdir -p docs/proposals/pending && printf 'proposal body\\n' > "
               "docs/proposals/pending/idea.md && git add -A && git commit -q -m proposal",
               dir);
      assert(sh(cmd) == 0);
      /* the trigger keys the run on the file's BLOB sha */
      char sha[64] = "";
      {
         char c2[1200];
         snprintf(c2, sizeof c2, "git -C %s rev-parse HEAD:docs/proposals/pending/idea.md", dir);
         FILE *p = popen(c2, "r");
         assert(p && fgets(sha, sizeof sha, p));
         pclose(p);
         sha[40] = '\0';
      }
      /* moved + committed */
      assert(wfe_source_archive_move(dir, sha, "docs/proposals/pending", "docs/proposals/done") ==
             1);
      snprintf(
          cmd, sizeof cmd,
          "test ! -e %s/docs/proposals/pending/idea.md && test -f "
          "%s/docs/proposals/done/idea.md && git -C %s status --porcelain | wc -l | grep -qx 0",
          dir, dir, dir);
      assert(sh(cmd) == 0);
      /* idempotent: the blob is gone from pending/ -> nothing to do */
      assert(wfe_source_archive_move(dir, sha, "docs/proposals/pending", "docs/proposals/done") ==
             0);
      /* unknown sha -> nothing to do (never an error) */
      assert(wfe_source_archive_move(dir, "0000000000000000000000000000000000000000",
                                     "docs/proposals/pending", "docs/proposals/done") == 0);
   }

   snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
   sh(cmd);
   printf("ok\n");
   return 0;
}
