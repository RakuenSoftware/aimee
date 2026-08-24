/* test_policy_arms.c: instructions as measurable arms (S6).
 *
 * The properties that matter are the safety ones. A registry that can be talked
 * into injecting an undeclared fragment, or that lets a sample quietly become
 * the default, is worse than no registry: it changes what the agent is told
 * without anyone deciding to.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"

#include <aimee/learning/approach_memory.h>
#include "approach_store.h"
#include "support/store_module_fixture.h"
#include <aimee/learning/learning.h>
#include <aimee/learning/policy_arms.h>

/* Rendering asks the knowledge service which arm to apply. This test links
 * none, so NULL is returned and the LOCAL default stands — which is exactly the
 * fallback the renderer is required to take. */
char *kb_client_learning_policy_select_json(const char *decision_point)
{
   (void)decision_point;
   return NULL;
}

static int g_sampler_calls;
static int g_sampler_answer;

static int fake_sampler(const char *decision_point, const char (*arms)[LEARNING_POLICY_ARM_LEN],
                        int n)
{
   (void)decision_point;
   (void)arms;
   (void)n;
   g_sampler_calls++;
   return g_sampler_answer;
}

static char g_rewarded_point[LEARNING_POLICY_POINT_LEN];
static char g_rewarded_arm[LEARNING_POLICY_ARM_LEN];
static double g_rewarded_value;
static int g_reward_calls;

static void fake_reward(const char *decision_point, const char *arm, double reward)
{
   snprintf(g_rewarded_point, sizeof(g_rewarded_point), "%s", decision_point);
   snprintf(g_rewarded_arm, sizeof(g_rewarded_arm), "%s", arm);
   g_rewarded_value = reward;
   g_reward_calls++;
}

static void test_registry_shape(void)
{
   char arms[LEARNING_POLICY_MAX_ARMS][LEARNING_POLICY_ARM_LEN];
   int n = learning_policy_arms(LEARNING_POLICY_PLAN_ADVISORY, arms, LEARNING_POLICY_MAX_ARMS);
   assert(n == 3);

   /* A control arm MUST exist, or "is this block worth its tokens?" has
    * nothing to be measured against. */
   int has_off = 0;
   for (int i = 0; i < n; i++)
      if (strcmp(arms[i], LEARNING_POLICY_ADVISORY_OFF) == 0)
         has_off = 1;
   assert(has_off);

   assert(learning_policy_arm_is_valid(LEARNING_POLICY_PLAN_ADVISORY,
                                       LEARNING_POLICY_ADVISORY_FULL) == 1);
   assert(learning_policy_arm_is_valid(LEARNING_POLICY_PLAN_ADVISORY, "invented") == 0);
   assert(learning_policy_arm_is_valid("no_such_point", LEARNING_POLICY_ADVISORY_FULL) == 0);
   assert(learning_policy_arms("no_such_point", arms, LEARNING_POLICY_MAX_ARMS) == -1);
   assert(learning_policy_default_arm("no_such_point") == NULL);
}

static void test_no_sampler_means_no_change(void)
{
   /* The case that governs every installation with no knowledge service: the
    * registry must be invisible. */
   learning_policy_reset();
   char arm[LEARNING_POLICY_ARM_LEN] = "";
   assert(learning_policy_select(LEARNING_POLICY_PLAN_ADVISORY, arm, sizeof(arm)) == 0);
   assert(strcmp(arm, LEARNING_POLICY_ADVISORY_FULL) == 0);
   assert(strcmp(learning_policy_default_arm(LEARNING_POLICY_PLAN_ADVISORY),
                 LEARNING_POLICY_ADVISORY_FULL) == 0);
}

static void test_a_sampler_is_honoured_but_not_trusted(void)
{
   learning_policy_reset();
   learning_policy_register_sampler(fake_sampler);

   char arm[LEARNING_POLICY_ARM_LEN] = "";
   g_sampler_calls = 0;
   g_sampler_answer = 0; /* "off" */
   assert(learning_policy_select(LEARNING_POLICY_PLAN_ADVISORY, arm, sizeof(arm)) == 0);
   assert(g_sampler_calls == 1);
   assert(strcmp(arm, LEARNING_POLICY_ADVISORY_OFF) == 0);

   /* Declining is normal, not an error: it means "use the default". */
   g_sampler_answer = -1;
   assert(learning_policy_select(LEARNING_POLICY_PLAN_ADVISORY, arm, sizeof(arm)) == 0);
   assert(strcmp(arm, LEARNING_POLICY_ADVISORY_FULL) == 0);

   /* A sampler that answers out of range must not be able to inject anything.
    * It runs in another process; treating its answer as authoritative would
    * make an undeclared fragment reachable from outside. */
   g_sampler_answer = 99;
   assert(learning_policy_select(LEARNING_POLICY_PLAN_ADVISORY, arm, sizeof(arm)) == 0);
   assert(strcmp(arm, LEARNING_POLICY_ADVISORY_FULL) == 0);

   g_sampler_answer = -999;
   assert(learning_policy_select(LEARNING_POLICY_PLAN_ADVISORY, arm, sizeof(arm)) == 0);
   assert(strcmp(arm, LEARNING_POLICY_ADVISORY_FULL) == 0);

   /* Sampling never moves the default. Only promotion does. */
   assert(strcmp(learning_policy_default_arm(LEARNING_POLICY_PLAN_ADVISORY),
                 LEARNING_POLICY_ADVISORY_FULL) == 0);
}

static void test_reward_refuses_an_arm_nobody_could_choose(void)
{
   learning_policy_reset();
   learning_policy_register_reward_sink(fake_reward);
   g_reward_calls = 0;

   assert(learning_policy_report(LEARNING_POLICY_PLAN_ADVISORY, LEARNING_POLICY_ADVISORY_BRIEF,
                                 0.75) == 0);
   assert(g_reward_calls == 1);
   assert(strcmp(g_rewarded_arm, LEARNING_POLICY_ADVISORY_BRIEF) == 0);
   assert(g_rewarded_value == 0.75);

   /* Crediting a fragment that is not declared would attach a measurement to
    * something no selection could have produced. */
   assert(learning_policy_report(LEARNING_POLICY_PLAN_ADVISORY, "invented", 1.0) == -1);
   assert(learning_policy_report("no_such_point", LEARNING_POLICY_ADVISORY_FULL, 1.0) == -1);
   assert(g_reward_calls == 1);

   /* With no sink installed the call is refused rather than silently dropped. */
   learning_policy_register_reward_sink(NULL);
   assert(learning_policy_report(LEARNING_POLICY_PLAN_ADVISORY, LEARNING_POLICY_ADVISORY_FULL,
                                 1.0) == -1);
}

static void test_promotion_is_gated_and_reversible(void)
{
   learning_policy_reset();

   /* The gate is open on an empty ledger (S0), so a deliberate promotion
    * works and moves the default. */
   assert(learning_gate_check(NULL) == LEARNING_GATE_OPEN);
   assert(learning_policy_promote(LEARNING_POLICY_PLAN_ADVISORY, LEARNING_POLICY_ADVISORY_BRIEF) ==
          0);
   assert(strcmp(learning_policy_default_arm(LEARNING_POLICY_PLAN_ADVISORY),
                 LEARNING_POLICY_ADVISORY_BRIEF) == 0);

   char arm[LEARNING_POLICY_ARM_LEN] = "";
   assert(learning_policy_select(LEARNING_POLICY_PLAN_ADVISORY, arm, sizeof(arm)) == 0);
   assert(strcmp(arm, LEARNING_POLICY_ADVISORY_BRIEF) == 0);

   /* An undeclared arm cannot be promoted into the default either. */
   assert(learning_policy_promote(LEARNING_POLICY_PLAN_ADVISORY, "invented") == -1);
   assert(learning_policy_promote("no_such_point", LEARNING_POLICY_ADVISORY_OFF) == -1);

   /* Reset restores what shipped — an operator can undo a promotion. */
   learning_policy_reset();
   assert(strcmp(learning_policy_default_arm(LEARNING_POLICY_PLAN_ADVISORY),
                 LEARNING_POLICY_ADVISORY_FULL) == 0);
}

static void test_the_arm_changes_what_is_actually_said(void)
{
   /* Reads the store back, so it needs one attached. main() has started the
      module when a database was named; without one there is nothing to assert
      against and the pure tests above still ran. */
   if (!store_module_fixture_available())
      return;

   /* The registry is only worth anything if the arm reaches real output. */
   assert(approach_store_record("Rebuild the search index for the docs project",
                                "drop and re-ingest every document", "ran out of disk", "agent_job",
                                "agent_job:7") == 0);
   const char *goal = "rebuild the search index for the docs project";
   char out[2048];
   char arm[LEARNING_POLICY_ARM_LEN];

   learning_policy_reset();
   int n = approach_store_render(goal, out, sizeof(out), arm, sizeof(arm));
   assert(n == 1);
   assert(strcmp(arm, LEARNING_POLICY_ADVISORY_FULL) == 0);
   assert(strstr(out, "drop and re-ingest every document") != NULL);

   assert(learning_policy_promote(LEARNING_POLICY_PLAN_ADVISORY, LEARNING_POLICY_ADVISORY_BRIEF) ==
          0);
   n = approach_store_render(goal, out, sizeof(out), arm, sizeof(arm));
   assert(n == 1);
   assert(strcmp(arm, LEARNING_POLICY_ADVISORY_BRIEF) == 0);
   /* One line, and crucially NOT the detail. */
   assert(strstr(out, "already failed") != NULL);
   assert(strstr(out, "drop and re-ingest every document") == NULL);

   /* The control arm says nothing at all — and reports 0 hits, so a caller
    * cannot mistake silence for "nothing was known". */
   assert(learning_policy_promote(LEARNING_POLICY_PLAN_ADVISORY, LEARNING_POLICY_ADVISORY_OFF) ==
          0);
   n = approach_store_render(goal, out, sizeof(out), arm, sizeof(arm));
   assert(n == 0);
   assert(out[0] == '\0');
   assert(strcmp(arm, LEARNING_POLICY_ADVISORY_OFF) == 0);

   learning_policy_reset();
}

int main(void)
{
   printf("policy_arms: ");

   if (store_module_fixture_available())
      store_module_fixture_start();
   db2_test_shim_open();

   test_registry_shape();
   test_no_sampler_means_no_change();
   test_a_sampler_is_honoured_but_not_trusted();
   test_reward_refuses_an_arm_nobody_could_choose();
   test_promotion_is_gated_and_reversible();
   test_the_arm_changes_what_is_actually_said();

   {
      char arm[LEARNING_POLICY_ARM_LEN];
      assert(learning_policy_select(NULL, arm, sizeof(arm)) == -1);
      assert(learning_policy_select(LEARNING_POLICY_PLAN_ADVISORY, NULL, 4) == -1);
      assert(learning_policy_select(LEARNING_POLICY_PLAN_ADVISORY, arm, 0) == -1);
   }

   printf("ok\n");
   return 0;
}
