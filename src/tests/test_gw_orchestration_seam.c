/* test_gw_orchestration_seam.c -- Slice 3 of the response/orchestration-stages proposal:
 * the orchestration-hook registry + runner, proven THROUGH the runner. The worked example
 * is a delegate hook (the first-port target): a hook that spawns a delegate via the narrow
 * capability handle and returns a turn-control verb. Also proves the fail-OPEN contract that
 * distinguishes this seam from the fail-closed response-stage registry. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_orchestration_seam.h"

/* --- a fake capability backing, standing in for the real delegate spawner --- */
static int g_spawns;
static char g_last_role[32];
static int fake_spawn_delegate(void *ctx, const char *role, const char *brief)
{
   (void)ctx;
   (void)brief;
   g_spawns++;
   if (role)
      snprintf(g_last_role, sizeof(g_last_role), "%s", role);
   return 0;
}
static int g_dispatches;
static char g_last_lane[32];
static int fake_dispatch_workflow(void *ctx, const char *lane, const char *payload)
{
   (void)ctx;
   (void)payload;
   g_dispatches++;
   if (lane)
      snprintf(g_last_lane, sizeof(g_last_lane), "%s", lane);
   return 0;
}

/* --- hooks --- */
static int g_ran_delegate, g_ran_after;

/* The worked example: a delegate hook spawns through the capability handle, then continues. */
static gw_orch_result_t delegate_hook(const gw_turn_snapshot_t *turn,
                                      const gw_turn_capabilities_t *caps, void *ud)
{
   (void)turn;
   (void)ud;
   g_ran_delegate++;
   caps->spawn_delegate(caps->ctx, "reviewer", "check the diff");
   gw_orch_result_t r = {GW_ORCH_CONTINUE, NULL};
   return r;
}
static gw_orch_result_t after_hook(const gw_turn_snapshot_t *turn,
                                   const gw_turn_capabilities_t *caps, void *ud)
{
   (void)turn;
   (void)caps;
   (void)ud;
   g_ran_after++;
   gw_orch_result_t r = {GW_ORCH_CONTINUE, NULL};
   return r;
}
static gw_orch_result_t failing_hook(const gw_turn_snapshot_t *turn,
                                     const gw_turn_capabilities_t *caps, void *ud)
{
   (void)turn;
   (void)caps;
   (void)ud;
   gw_orch_result_t r = {GW_ORCH_FAIL, NULL};
   return r;
}
static gw_orch_result_t complete_hook(const gw_turn_snapshot_t *turn,
                                      const gw_turn_capabilities_t *caps, void *ud)
{
   (void)turn;
   (void)caps;
   (void)ud;
   gw_orch_result_t r = {GW_ORCH_COMPLETE, NULL};
   return r;
}
static gw_orch_result_t suspend_hook(const gw_turn_snapshot_t *turn,
                                     const gw_turn_capabilities_t *caps, void *ud)
{
   (void)turn;
   (void)caps;
   (void)ud;
   gw_orch_result_t r = {GW_ORCH_SUSPEND, NULL};
   r.continuation = strdup("resume-token-42"); /* owned by the caller after return */
   return r;
}
/* A workflow hook dispatches through the second narrow capability handle, then continues. */
static gw_orch_result_t workflow_hook(const gw_turn_snapshot_t *turn,
                                      const gw_turn_capabilities_t *caps, void *ud)
{
   (void)turn;
   (void)ud;
   caps->dispatch_workflow(caps->ctx, "build", "{}");
   gw_orch_result_t r = {GW_ORCH_CONTINUE, NULL};
   return r;
}
/* A malformed SUSPEND: claims to suspend but provides no continuation to re-enter with. */
static gw_orch_result_t suspend_no_cont_hook(const gw_turn_snapshot_t *turn,
                                             const gw_turn_capabilities_t *caps, void *ud)
{
   (void)turn;
   (void)caps;
   (void)ud;
   gw_orch_result_t r = {GW_ORCH_SUSPEND, NULL};
   return r;
}

int main(void)
{
   gw_orch_hook_t out[8];
   gw_turn_snapshot_t turn;
   memset(&turn, 0, sizeof(turn));
   turn.turn_id = "t1";
   gw_turn_capabilities_t caps = {NULL, fake_spawn_delegate, fake_dispatch_workflow};

   /* enabled hooks run through the runner, in order; the delegate hook spawns via the
    * capability handle. */
   {
      gw_orch_hook_slot_t slots[] = {
          {"delegates", delegate_hook, NULL, 1},
          {"after", after_hook, NULL, 1},
      };
      int n = gw_orchestration_registry_build(slots, 2, out, 8);
      assert(n == 2 && strcmp(out[0].name, "delegates") == 0);
      g_ran_delegate = g_ran_after = g_spawns = 0;
      g_last_role[0] = 0;
      gw_orch_result_t res = gw_orchestration_run(&turn, &caps, out, (size_t)n);
      assert(res.status == GW_ORCH_CONTINUE);
      assert(g_ran_delegate == 1 && g_ran_after == 1);
      assert(g_spawns == 1 && strcmp(g_last_role, "reviewer") == 0);
   }

   /* disabled module is OMITTED and never spawns. */
   {
      gw_orch_hook_slot_t slots[] = {
          {"delegates", delegate_hook, NULL, 0},
          {"after", after_hook, NULL, 1},
      };
      int n = gw_orchestration_registry_build(slots, 2, out, 8);
      assert(n == 1 && strcmp(out[0].name, "after") == 0);
      g_ran_delegate = g_spawns = 0;
      gw_orchestration_run(&turn, &caps, out, (size_t)n);
      assert(g_ran_delegate == 0 && g_spawns == 0);
   }

   /* FAIL is fail-OPEN: a later hook STILL runs (the distinguishing contract vs response
    * stages, where a non-OK stage stops the pipeline) -- AND the failure is surfaced as an
    * aggregate FAIL so the wire site can observe/log it (the turn still proceeds). */
   {
      gw_orch_hook_slot_t slots[] = {
          {"bad", failing_hook, NULL, 1},
          {"after", after_hook, NULL, 1},
      };
      int n = gw_orchestration_registry_build(slots, 2, out, 8);
      assert(n == 2);
      g_ran_after = 0;
      gw_orch_result_t res = gw_orchestration_run(&turn, &caps, out, (size_t)n);
      assert(res.status == GW_ORCH_FAIL && g_ran_after == 1);
   }

   /* a COMPLETE after a FAIL supersedes it: the definitive turn outcome wins. */
   {
      gw_orch_hook_slot_t slots[] = {
          {"bad", failing_hook, NULL, 1},
          {"done", complete_hook, NULL, 1},
      };
      int n = gw_orchestration_registry_build(slots, 2, out, 8);
      assert(n == 2);
      gw_orch_result_t res = gw_orchestration_run(&turn, &caps, out, (size_t)n);
      assert(res.status == GW_ORCH_COMPLETE);
   }

   /* the workflow-dispatch capability is exercised through the second narrow handle. */
   {
      gw_orch_hook_slot_t slots[] = {{"workflow", workflow_hook, NULL, 1}};
      int n = gw_orchestration_registry_build(slots, 1, out, 8);
      assert(n == 1);
      g_dispatches = 0;
      g_last_lane[0] = 0;
      gw_orch_result_t res = gw_orchestration_run(&turn, &caps, out, (size_t)n);
      assert(res.status == GW_ORCH_CONTINUE);
      assert(g_dispatches == 1 && strcmp(g_last_lane, "build") == 0);
   }

   /* a malformed SUSPEND (NULL continuation) is a fail-open failure, not a turn suspend. */
   {
      gw_orch_hook_slot_t slots[] = {{"badsuspend", suspend_no_cont_hook, NULL, 1},
                                     {"after", after_hook, NULL, 1}};
      int n = gw_orchestration_registry_build(slots, 2, out, 8);
      assert(n == 2);
      g_ran_after = 0;
      gw_orch_result_t res = gw_orchestration_run(&turn, &caps, out, (size_t)n);
      assert(res.status == GW_ORCH_FAIL && g_ran_after == 1);
   }

   /* a NULL fn in a hand-built catalog is handled fail-open (no crash), surfaced as FAIL. */
   {
      gw_orch_hook_t hand[] = {{NULL, NULL, "broken"}, {after_hook, NULL, "after"}};
      g_ran_after = 0;
      gw_orch_result_t res = gw_orchestration_run(&turn, &caps, hand, 2);
      assert(res.status == GW_ORCH_FAIL && g_ran_after == 1);
   }

   /* COMPLETE short-circuits: later hooks do NOT run, status propagates. */
   {
      gw_orch_hook_slot_t slots[] = {
          {"done", complete_hook, NULL, 1},
          {"after", after_hook, NULL, 1},
      };
      int n = gw_orchestration_registry_build(slots, 2, out, 8);
      assert(n == 2);
      g_ran_after = 0;
      gw_orch_result_t res = gw_orchestration_run(&turn, &caps, out, (size_t)n);
      assert(res.status == GW_ORCH_COMPLETE && g_ran_after == 0);
   }

   /* SUSPEND stops and propagates the hook's owned continuation to the caller (caller frees). */
   {
      gw_orch_hook_slot_t slots[] = {
          {"suspend", suspend_hook, NULL, 1},
          {"after", after_hook, NULL, 1},
      };
      int n = gw_orchestration_registry_build(slots, 2, out, 8);
      assert(n == 2);
      g_ran_after = 0;
      gw_orch_result_t res = gw_orchestration_run(&turn, &caps, out, (size_t)n);
      assert(res.status == GW_ORCH_SUSPEND && g_ran_after == 0);
      assert(res.continuation && strcmp(res.continuation, "resume-token-42") == 0);
      free(res.continuation); /* caller owns it */
   }

   /* build errors: duplicate enabled name, NULL fn, empty name, overflow. */
   {
      gw_orch_hook_slot_t dup[] = {{"d", delegate_hook, NULL, 1}, {"d", after_hook, NULL, 1}};
      assert(gw_orchestration_registry_build(dup, 2, out, 8) == -1);
      gw_orch_hook_slot_t bad[] = {{"x", NULL, NULL, 1}};
      assert(gw_orchestration_registry_build(bad, 1, out, 8) == -1);
      gw_orch_hook_slot_t noname[] = {{"", delegate_hook, NULL, 1}};
      assert(gw_orchestration_registry_build(noname, 1, out, 8) == -1);
      gw_orch_hook_slot_t two[] = {{"a", delegate_hook, NULL, 1}, {"b", delegate_hook, NULL, 1}};
      assert(gw_orchestration_registry_build(two, 2, out, 1) == -1);
   }

   /* a disabled duplicate is fine (only the enabled set is checked). */
   {
      gw_orch_hook_slot_t slots[] = {{"m", delegate_hook, NULL, 1}, {"m", after_hook, NULL, 0}};
      assert(gw_orchestration_registry_build(slots, 2, out, 8) == 1);
   }

   /* a NULL hook name in the catalog is rejected up front (distinct from the empty-name case). */
   {
      gw_orch_hook_slot_t nullname[] = {{NULL, delegate_hook, NULL, 1}};
      assert(gw_orchestration_registry_build(nullname, 1, out, 8) == -1);
   }

   /* a clean empty/NULL hook list returns CONTINUE, not FAIL. */
   {
      assert(gw_orchestration_run(&turn, &caps, NULL, 0).status == GW_ORCH_CONTINUE);
      gw_orch_hook_t empty[1];
      assert(gw_orchestration_run(&turn, &caps, empty, 0).status == GW_ORCH_CONTINUE);
   }

   /* fail-OPEN at the boundary: a NULL turn OR NULL caps returns FAIL WITHOUT aborting. */
   {
      gw_orch_hook_t none[1];
      assert(gw_orchestration_run(NULL, &caps, none, 0).status == GW_ORCH_FAIL);
      assert(gw_orchestration_run(&turn, NULL, none, 0).status == GW_ORCH_FAIL);
   }

   printf("ok\n");
   return 0;
}
