/* test_agent_max_turns.c: unit tests for agent_resolve_max_turns(). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "agent_exec.h"

static int g_max_iterations = 0;
static int g_max_iterations_delegate = 0;

int config_load(config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->max_iterations = g_max_iterations;
   cfg->max_iterations_delegate = g_max_iterations_delegate;
   return 0;
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
   assert(t == 1000); /* falls through to primary default */
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

/* Primary session: max_turns == -1, no config → 1000 */
static void test_primary_unlimited_default(void)
{
   agent_t a = make_agent(-1);
   int t = agent_resolve_max_turns(&a, NULL);
   assert(t == 1000);
   printf("  PASS: primary unlimited default is 1000\n");
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

/* Delegate: max_turns unset, no config → compile-time default */
static void test_delegate_compile_time_default(void)
{
   agent_t a = make_agent(-1);
   int t = agent_resolve_max_turns(&a, "code");
   assert(t == CONFIG_DEFAULT_MAX_ITERATIONS_DELEGATE);
   printf("  PASS: delegate compile-time default is %d\n", CONFIG_DEFAULT_MAX_ITERATIONS_DELEGATE);
}

/* max_turns == 0 means unlimited (1000 safety cap), regardless of role */
static void test_zero_means_unlimited(void)
{
   agent_t a = make_agent(0);
   assert(agent_resolve_max_turns(&a, NULL) == 1000);
   assert(agent_resolve_max_turns(&a, "code") == 1000);
   printf("  PASS: max_turns==0 means unlimited (1000) for both primary and delegate\n");
}

int main(void)
{
   printf("test_agent_max_turns:\n");

   test_primary_ignores_agent_max_turns();
   test_primary_uses_config_max_iterations();
   test_primary_unlimited_default();
   test_delegate_honours_agent_max_turns();
   test_delegate_falls_back_to_config();
   test_delegate_compile_time_default();
   test_zero_means_unlimited();

   printf("test_agent_max_turns: all tests passed\n");
   return 0;
}

const char *config_embedding_command(const config_t *cfg, const char *requested)
{
   if (requested && requested[0])
      return requested;
   if (cfg && cfg->embedding_command[0])
      return cfg->embedding_command;
   return "builtin";
}
