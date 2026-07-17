/* test_gw_orch_delegates.c -- the DELEGATES orchestration module (first port of the
 * orchestration seam). Proves the toggle gates the spawn and that, when enabled, the module
 * drives the spawn through the narrow capability handle -- using a fake capability so no real
 * delegate thread is created. The real coord_spawn_delegate adapter (which calls
 * delegate_spawn_ondemand) is exercised separately by unit-test-server-compute. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_orch_delegates.h"

static int g_spawns;
static char g_last_role[32];
static char g_last_brief[64];
static int fake_spawn(void *ctx, const char *role, const char *brief)
{
   (void)ctx;
   g_spawns++;
   snprintf(g_last_role, sizeof(g_last_role), "%s", role ? role : "");
   snprintf(g_last_brief, sizeof(g_last_brief), "%s", brief ? brief : "");
   return 0;
}

static void reset(void)
{
   g_spawns = 0;
   g_last_role[0] = g_last_brief[0] = 0;
}

int main(void)
{
   gw_turn_capabilities_t caps = {NULL, fake_spawn, NULL};

   /* The env default predicate stays pure (config is resolved at the wire site). */
   unsetenv("AIMEE_ORCH_DELEGATES");
   assert(gw_orch_delegates_enabled() == 1);
   for (const char **v = (const char *[]){"0", "off", "false", "no", NULL}; *v; v++)
   {
      setenv("AIMEE_ORCH_DELEGATES", *v, 1);
      assert(gw_orch_delegates_enabled() == 0);
   }
   setenv("AIMEE_ORCH_DELEGATES", "1", 1);
   assert(gw_orch_delegates_enabled() == 1);

   /* enabled=1: the spawn runs through the capability handle. */
   reset();
   assert(gw_orch_delegates_run(&caps, "coord-task-7", "reviewer", "check the diff", 1) == 0);
   assert(g_spawns == 1);
   assert(strcmp(g_last_role, "reviewer") == 0 && strcmp(g_last_brief, "check the diff") == 0);

   /* enabled=0: no spawn is attempted and the module reports -1 so the caller can release the
    * claim and retry (delegate spawning is paused while the module is off). */
   reset();
   assert(gw_orch_delegates_run(&caps, "coord-task-8", "reviewer", "x", 0) == -1);
   assert(g_spawns == 0);

   /* NULL caps is a clean -1 (no spawn), not a crash. */
   assert(gw_orch_delegates_run(NULL, "t", "r", "b", 1) == -1);

   /* a missing spawn_delegate handle is fail-open: the module still returns 0 (the hook ran),
    * and no spawn happens -- the runner surfaces the FAIL internally, never blocking. */
   {
      gw_turn_capabilities_t no_spawn = {NULL, NULL, NULL};
      reset();
      assert(gw_orch_delegates_run(&no_spawn, "t", "r", "b", 1) == 0);
      assert(g_spawns == 0);
   }

   printf("ok\n");
   return 0;
}
