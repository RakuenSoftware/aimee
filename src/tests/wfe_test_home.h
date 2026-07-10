#ifndef WFE_TEST_HOME_H
#define WFE_TEST_HOME_H 1

/* wfe_test_home.h -- a mkdtemp() that removes its directory at process exit, so a
 * WFE unit test never strands its /tmp AIMEE_HOME. Each test points AIMEE_HOME at
 * a mkdtemp'd dir and the engine fills it with git worktrees (wfe-worktrees/wi_*,
 * ~4k files each); left behind across runs they exhausted /tmp's inodes. This is a
 * drop-in for mkdtemp() -- same signature and return -- that also schedules an
 * `rm -rf` of every dir it created via atexit. Header-only + static so no Rules.mk
 * or link changes are needed: each test binary is one translation unit and gets
 * its own private list, so parallel test runs only ever remove their OWN dirs. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define WFE_TEST_HOME_MAX 16
static char wfe_test_home_dirs__[WFE_TEST_HOME_MAX][256];
static int wfe_test_home_n__;

static void wfe_test_home_sweep__(void)
{
   for (int i = 0; i < wfe_test_home_n__; i++)
   {
      char cmd[320];
      /* mkdtemp paths are our own short /tmp/wfe_*_XXXXXX templates (no shell
       * metacharacters); best-effort removal, status is irrelevant at exit. */
      if (snprintf(cmd, sizeof cmd, "rm -rf '%s'", wfe_test_home_dirs__[i]) < (int)sizeof cmd)
         (void)system(cmd);
   }
}

/* Drop-in mkdtemp(): creates the dir and schedules its recursive removal at exit.
 * Returns mkdtemp's result (the mutated template on success, NULL on failure). */
static char *wfe_test_mkdtemp(char *tmpl)
{
   char *p = mkdtemp(tmpl);
   if (p && wfe_test_home_n__ < WFE_TEST_HOME_MAX)
   {
      if (wfe_test_home_n__ == 0)
         atexit(wfe_test_home_sweep__);
      snprintf(wfe_test_home_dirs__[wfe_test_home_n__++], 256, "%s", p);
   }
   return p;
}

#endif /* WFE_TEST_HOME_H */
