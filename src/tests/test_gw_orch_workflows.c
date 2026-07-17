/* test_gw_orch_workflows.c -- the WORKFLOWS orchestration module (second port of the
 * orchestration seam). Proves the toggle gates the dispatch and that, when enabled, the module
 * drives the dispatch through the narrow capability handle -- using a fake capability so no
 * real workflow work item is created. The real trigger_dispatch_workflow adapter (which calls
 * wfe_work_item_create) is exercised separately by unit-test-cron-runtime. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_orch_workflows.h"

static int g_dispatches;
static char g_last_lane[32];
static char g_last_payload[64];
static int fake_dispatch(void *ctx, const char *lane, const char *payload)
{
   (void)ctx;
   g_dispatches++;
   snprintf(g_last_lane, sizeof(g_last_lane), "%s", lane ? lane : "");
   snprintf(g_last_payload, sizeof(g_last_payload), "%s", payload ? payload : "");
   return 0;
}

static void reset(void)
{
   g_dispatches = 0;
   g_last_lane[0] = g_last_payload[0] = 0;
}

int main(void)
{
   gw_turn_capabilities_t caps = {NULL, NULL, fake_dispatch};

   /* The env default predicate stays pure (config is resolved at the wire site). */
   unsetenv("AIMEE_ORCH_WORKFLOWS");
   assert(gw_orch_workflows_enabled() == 1);
   for (const char **v = (const char *[]){"0", "off", "false", "no", NULL}; *v; v++)
   {
      setenv("AIMEE_ORCH_WORKFLOWS", *v, 1);
      assert(gw_orch_workflows_enabled() == 0);
   }
   setenv("AIMEE_ORCH_WORKFLOWS", "1", 1);
   assert(gw_orch_workflows_enabled() == 1);

   /* enabled=1: the dispatch runs through the capability handle. */
   reset();
   assert(gw_orch_workflows_run(&caps, "trigger-proposals", "managed-change", "/w/wi-1.md", 1) ==
          0);
   assert(g_dispatches == 1);
   assert(strcmp(g_last_lane, "managed-change") == 0 && strcmp(g_last_payload, "/w/wi-1.md") == 0);

   /* enabled=0: no dispatch is attempted and the module reports -1 so the caller can log/skip. */
   reset();
   assert(gw_orch_workflows_run(&caps, "trigger-x", "managed-change", "/w/wi-2.md", 0) == -1);
   assert(g_dispatches == 0);

   /* NULL caps is a clean -1 (no dispatch), not a crash. */
   assert(gw_orch_workflows_run(NULL, "t", "l", "p", 1) == -1);

   /* a missing dispatch_workflow handle is fail-open: the module still returns 0 (the hook
    * ran), and no dispatch happens -- the runner surfaces the FAIL internally, never blocking. */
   {
      gw_turn_capabilities_t no_dispatch = {NULL, NULL, NULL};
      reset();
      assert(gw_orch_workflows_run(&no_dispatch, "t", "l", "p", 1) == 0);
      assert(g_dispatches == 0);
   }

   printf("ok\n");
   return 0;
}
