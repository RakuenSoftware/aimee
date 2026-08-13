/* Registering the same workspace twice is success, not failure.
 *
 * `workspace add` was not idempotent: the second call returned -2 and both the
 * RPC handler and the CLI turned that into a hard error. Any automation that
 * registers its workspace unconditionally -- which is the normal shape, because
 * checking first is racy -- therefore failed at setup on every re-run, before
 * doing any work.
 *
 * Observed: a benchmark cell re-executed after an unrelated harness fault died
 * in under 90 seconds with "workspace: already registered", having accomplished
 * nothing. The only way forward was to hand-edit the registry.
 *
 * Callers cannot avoid this by checking first. Between a `workspace list` and
 * the add, another session can register the same path. Idempotence is the only
 * race-free contract, so this pins both halves of it:
 *
 *   - config_workspace_add still REPORTS the duplicate (-2), so callers that
 *     want to say "already registered" can;
 *   - and the registry does not grow, so repeated adds cannot exhaust the 64
 *     slots (see test_workspace_prune_dead.c for what that costs).
 */
#include "config.h"
#include "modules/config/config_accessors.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static char g_home[256];

static void set_home(void)
{
   snprintf(g_home, sizeof(g_home), "%s/aimee-wsidem-XXXXXX", platform_tmpdir());
   assert(mkdtemp(g_home) != NULL);
   setenv("AIMEE_HOME", g_home, 1);
   setenv("HOME", g_home, 1);
}

static int registered_count(void)
{
   return config_workspace_count();
}

static void test_second_add_reports_duplicate_without_growing(void)
{
   char path[320];
   snprintf(path, sizeof(path), "%s/ws", g_home);
   assert(mkdir(path, 0700) == 0);

   assert(config_workspace_add(path, NULL, NULL, NULL) == 0);
   int after_first = registered_count();
   assert(after_first == 1);

   /* The duplicate is REPORTED -- callers need to be able to distinguish -- */
   assert(config_workspace_add(path, NULL, NULL, NULL) == -2);
   /* -- but it is not destructive, and it does not consume a slot. */
   assert(registered_count() == after_first);

   /* Still true on the third call: -2 is a stable steady state, not a
    * one-shot warning that flips to success or to a different error. */
   assert(config_workspace_add(path, NULL, NULL, NULL) == -2);
   assert(registered_count() == after_first);
}

static void test_distinct_paths_still_register(void)
{
   char other[320];
   snprintf(other, sizeof(other), "%s/ws2", g_home);
   assert(mkdir(other, 0700) == 0);

   int before = registered_count();
   assert(config_workspace_add(other, NULL, NULL, NULL) == 0);
   assert(registered_count() == before + 1);
}

int main(void)
{
   printf("workspace_add_idempotent: ");
   set_home();
   test_second_add_reports_duplicate_without_growing();
   test_distinct_paths_still_register();
   printf("ok\n");
   return 0;
}
