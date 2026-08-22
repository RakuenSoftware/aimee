/* test_agent_max_turns.c: unit tests for agent_resolve_max_turns(). */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "agent_exec.h"
#include "aimee.h"  /* KIND_COUNT, required by memory.h */
#include "memory.h" /* MEMORY_EMBED_TEST_FIXTURE */

static int g_max_iterations = 0;
static int g_max_iterations_delegate = 0;
static int g_primary_turn = 0;

int agent_routing_primary_turn(void)
{
   return g_primary_turn;
}

/* Accessor stubs expose the iteration limits under test. */
int config_max_iterations(void)
{
   return g_max_iterations;
}

int config_max_iterations_delegate(void)
{
   return g_max_iterations_delegate;
}

static agent_t make_agent(int max_turns)
{
   agent_t a;
   memset(&a, 0, sizeof(a));
   a.max_turns = max_turns;
   return a;
}

/* Primary session: agent->max_turns > 0 must be ignored */
static void test_primary_ignores_agent_max_turns(void)
{
   agent_t a = make_agent(30);
   int t = agent_resolve_max_turns(&a, NULL);
   assert(t == INT_MAX); /* primary ignores the cap; infinite by default */
   printf("  PASS: primary ignores agent->max_turns\n");
}

/* Primary session: uses config max_iterations when set */
static void test_primary_uses_config_max_iterations(void)
{
   agent_t a = make_agent(30);
   g_max_iterations = 200;
   int t = agent_resolve_max_turns(&a, NULL);
   g_max_iterations = 0;
   assert(t == 200);
   printf("  PASS: primary uses config max_iterations\n");
}

/* Primary session: max_turns == -1, no config → INFINITE */
static void test_primary_unlimited_default(void)
{
   agent_t a = make_agent(-1);
   int t = agent_resolve_max_turns(&a, NULL);
   assert(t == INT_MAX);
   printf("  PASS: primary unlimited default is infinite\n");
}

/* Delegate: agent->max_turns > 0 is honoured */
static void test_delegate_honours_agent_max_turns(void)
{
   agent_t a = make_agent(30);
   int t = agent_resolve_max_turns(&a, "code");
   assert(t == 30);
   printf("  PASS: delegate honours agent->max_turns\n");
}

/* Delegate: max_turns unset falls back to config */
static void test_delegate_falls_back_to_config(void)
{
   agent_t a = make_agent(-1);
   g_max_iterations_delegate = 50;
   int t = agent_resolve_max_turns(&a, "code");
   g_max_iterations_delegate = 0;
   assert(t == 50);
   printf("  PASS: delegate falls back to config max_iterations_delegate\n");
}

/* Delegate: max_turns unset (-1), no config cap → INFINITE (the default) */
static void test_delegate_default_is_infinite(void)
{
   agent_t a = make_agent(-1);
   int t = agent_resolve_max_turns(&a, "code");
   assert(t == INT_MAX);
   printf("  PASS: delegate default (-1) is infinite\n");
}

/* max_turns <= 0 means infinite, regardless of role, unless a config cap is set. */
static void test_zero_means_unlimited(void)
{
   agent_t a = make_agent(0);
   assert(agent_resolve_max_turns(&a, NULL) == INT_MAX);
   assert(agent_resolve_max_turns(&a, "code") == INT_MAX);
   printf("  PASS: max_turns<=0 means infinite for both primary and delegate\n");
}

/* Regression: the primary webchat turn is ROUTED as "code" (server_compute.c
 * calls agent_run_with_tools(&acfg, "code", ...)) but must be BUDGETED as a
 * primary session. Before agent_budget_role() existed, a chat turn resolved the
 * DELEGATE cap and then HARD-closed, injecting "[FINAL RESPONSE REQUIRED] The
 * tool turn budget is exhausted" into an ordinary conversation. */
static void test_primary_turn_is_not_budgeted_as_delegate(void)
{
   agent_t a = make_agent(0); /* agents.json ships max_turns: 0 */
   g_max_iterations = 200;
   g_max_iterations_delegate = 14;

   g_primary_turn = 1;
   const char *budget_role = agent_budget_role("code");
   int t = agent_resolve_max_turns(&a, budget_role);
   g_primary_turn = 0;

   assert(budget_role == NULL);
   assert(t == 200); /* the PRIMARY cap, not the 14-turn delegate cap */

   g_max_iterations = 0;
   g_max_iterations_delegate = 0;
   printf("  PASS: primary turn routed as \"code\" is budgeted as primary\n");
}

/* A real delegate keeps the delegate budget: the flag is thread-local and is
 * never set on the delegate worker threads a primary turn spawns. */
static void test_delegate_turn_keeps_delegate_budget(void)
{
   agent_t a = make_agent(0);
   g_max_iterations = 200;
   g_max_iterations_delegate = 14;

   g_primary_turn = 0;
   const char *budget_role = agent_budget_role("code");
   int t = agent_resolve_max_turns(&a, budget_role);

   assert(budget_role != NULL && strcmp(budget_role, "code") == 0);
   assert(t == 14);

   g_max_iterations = 0;
   g_max_iterations_delegate = 0;
   printf("  PASS: delegate turn keeps the delegate budget\n");
}

int main(void)
{
   printf("test_agent_max_turns:\n");

   test_primary_ignores_agent_max_turns();
   test_primary_uses_config_max_iterations();
   test_primary_unlimited_default();
   test_delegate_honours_agent_max_turns();
   test_delegate_falls_back_to_config();
   test_delegate_default_is_infinite();
   test_zero_means_unlimited();
   test_primary_turn_is_not_budgeted_as_delegate();
   test_delegate_turn_keeps_delegate_budget();

   printf("test_agent_max_turns: all tests passed\n");
   return 0;
}
