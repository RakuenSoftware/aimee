/* test_delegate_ensemble.c: unit tests for MoA ensemble fan-out and synthesis. */
#include "aimee.h"
#include "delegate_ensemble.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- stubs for agent exec functions --- */

static int g_parallel_mode = 0; /* 0=all-succeed, 1=only-first-succeeds */
static int g_parallel_calls = 0;
static int g_named_calls = 0;
static int g_aggregator_calls = 0;

/* Capture the per-task participant selector the engine sets, so a test can
 * assert the §0.1 routing fix: each fan-out task must be pointed at its own
 * configured reference agent, not left NULL (which routed all N to the one
 * default agent). The old stub ignored `tasks` entirely — that blind spot is
 * exactly why the unrouted-references bug shipped unseen. */
#define CAP_MAX 8
static char g_captured_agents[CAP_MAX][128];
static int g_captured_count = 0;

int agent_run_parallel(agent_config_t *cfg, agent_task_t *tasks, int count, agent_result_t *out)
{
   (void)cfg;
   g_parallel_calls++;
   g_captured_count = count < CAP_MAX ? count : CAP_MAX;
   for (int i = 0; i < g_captured_count; i++)
      snprintf(g_captured_agents[i], sizeof(g_captured_agents[i]), "%s",
               tasks[i].agent ? tasks[i].agent : "(null)");
   for (int i = 0; i < count; i++)
      memset(&out[i], 0, sizeof(out[i]));
   if (g_parallel_mode == 1)
   {
      out[0].response = strdup("only one answer");
      out[0].prompt_tokens = 50;
      out[0].completion_tokens = 50;
      return 1;
   }
   for (int i = 0; i < count; i++)
   {
      char buf[128];
      snprintf(buf, sizeof(buf), "mock response from %s", tasks[i].agent ? tasks[i].agent : "default");
      out[i].response = strdup(buf);
      out[i].prompt_tokens = 50;
      out[i].completion_tokens = 50;
      out[i].success = 1;
   }
   return count;
}

int agent_run_named(agent_config_t *cfg, const char *name, const char *role,
                    const char *system_prompt, const char *user_prompt, int max_tokens,
                    double temperature, agent_result_t *out)
{
   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)user_prompt;
   (void)max_tokens;
   (void)temperature;
   g_named_calls++;
   memset(out, 0, sizeof(*out));
   char buf[128];
   snprintf(buf, sizeof(buf), "named response from %s", name ? name : "missing");
   out->response = strdup(buf);
   out->prompt_tokens = 40;
   out->completion_tokens = 20;
   out->success = 1;
   return 0;
}

int agent_run_with_tools_write_enforce(agent_config_t *cfg, const char *role,
                                       const char *system_prompt, const char *user_prompt,
                                       int max_tokens, int enforce_writes, agent_result_t *out)
{
   (void)cfg;
   (void)role;
   (void)system_prompt;
   (void)enforce_writes;
   (void)max_tokens;
   (void)user_prompt;
   memset(out, 0, sizeof(*out));
   if (role && strcmp(role, "reason") == 0)
      out->response = strdup("80");
   else
   {
      char buf[128];
      g_aggregator_calls++;
      snprintf(buf, sizeof(buf), "synthesized answer %d", g_aggregator_calls);
      out->response = strdup(buf);
   }
   out->prompt_tokens = 200;
   out->completion_tokens = 100;
   out->success = 1;
   return 0;
}

/* --- test helpers --- */

static config_t make_cfg(int enabled, int min_ok, double max_cost)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.ensemble_enabled = enabled;
   cfg.ensemble_min_successful = min_ok;
   cfg.ensemble_max_cost_usd = max_cost;
   cfg.ensemble_reference_count = 3;
   snprintf(cfg.ensemble_reference_models[0], 128, "model-a");
   snprintf(cfg.ensemble_reference_models[1], 128, "model-b");
   snprintf(cfg.ensemble_reference_models[2], 128, "model-c");
   snprintf(cfg.ensemble_aggregator, sizeof(cfg.ensemble_aggregator), "review");
   return cfg;
}

static agent_config_t make_acfg(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   snprintf(acfg.default_agent, sizeof(acfg.default_agent), "review");
   return acfg;
}

/* --- tests --- */

static void test_ensemble_basic(void)
{
   g_parallel_mode = 0;
   g_parallel_calls = 0;
   g_aggregator_calls = 0;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   delegate_ensemble_result_t result;
   int rc = delegate_ensemble_run(&acfg, &cfg, "what is 2+2?", &result);
   assert(rc == 0);
   assert(result.success == 1);
   assert(!result.degraded);
   assert(!result.cost_capped);
   assert(result.response[0] != '\0');
   assert(delegate_ensemble_cost_usd(&result) > 0.0);
   printf("  test_ensemble_basic: ok\n");
}

static void test_ensemble_min_successful_degradation(void)
{
   g_parallel_mode = 1; /* only first ref succeeds */
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   delegate_ensemble_result_t result;
   int rc = delegate_ensemble_run(&acfg, &cfg, "hard question?", &result);
   assert(rc == 0);
   assert(result.degraded == 1);
   assert(!result.cost_capped);
   g_parallel_mode = 0;
   printf("  test_ensemble_min_successful_degradation: ok\n");
}

static void test_ensemble_cost_cap(void)
{
   g_parallel_mode = 0;
   /* default: 3 refs * (50+50) tokens * $0.000015 = $0.0045 > $0.001 cap */
   config_t cfg = make_cfg(1, 2, 0.001);
   agent_config_t acfg = make_acfg();
   delegate_ensemble_result_t result;
   int rc = delegate_ensemble_run(&acfg, &cfg, "expensive question", &result);
   assert(rc == 0);
   assert(result.cost_capped == 1);
   assert(!result.degraded);
   printf("  test_ensemble_cost_cap: ok\n");
}

static void test_ensemble_disabled(void)
{
   config_t cfg = make_cfg(0, 2, 10.0); /* disabled */
   agent_config_t acfg = make_acfg();
   delegate_ensemble_result_t result;

   int rc = delegate_ensemble_run(&acfg, &cfg, "any prompt", &result);
   assert(rc == -1);
   printf("  test_ensemble_disabled: ok\n");
}

static void test_ensemble_null_args(void)
{
   delegate_ensemble_result_t result;
   assert(delegate_ensemble_run(NULL, NULL, NULL, &result) == -1);
   assert(delegate_ensemble_cost_usd(NULL) == 0.0);
   printf("  test_ensemble_null_args: ok\n");
}

/* §0.1 regression: the engine must route each fan-out task to its OWN configured
 * reference agent. Before the fix every task->agent was NULL, so all N ran the
 * single default agent; this asserts three configured references produce three
 * distinct, correctly-ordered selectors. */
static void test_ensemble_routes_to_distinct_agents(void)
{
   g_parallel_mode = 0;
   g_captured_count = 0;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   delegate_ensemble_result_t result;
   int rc = delegate_ensemble_run(&acfg, &cfg, "route check", &result);
   assert(rc == 0);
   assert(g_captured_count == 3);
   assert(strcmp(g_captured_agents[0], "model-a") == 0);
   assert(strcmp(g_captured_agents[1], "model-b") == 0);
   assert(strcmp(g_captured_agents[2], "model-c") == 0);
   /* distinct, and none left NULL (the bug) */
   assert(strcmp(g_captured_agents[0], "(null)") != 0);
   assert(strcmp(g_captured_agents[0], g_captured_agents[1]) != 0);
   assert(strcmp(g_captured_agents[1], g_captured_agents[2]) != 0);
   printf("  test_ensemble_routes_to_distinct_agents: ok\n");
}

static void test_roundtable_parallel_basic(void)
{
   g_parallel_mode = 0;
   g_parallel_calls = 0;
   g_aggregator_calls = 0;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 2;
   opts.converge_threshold = 0;
   opts.deadline_ms = 0;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft a short engineering proposal", &opts, &result);
   assert(rc == 0);
   assert(result.artifact != NULL);
   assert(strstr(result.artifact, "synthesized answer") != NULL);
   assert(result.rounds_run == 2);
   assert(result.best_round > 0);
   assert(result.cost_usd > 0.0);
   assert(g_parallel_calls == 2);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_parallel_basic: ok\n");
}

static void test_roundtable_sequential_uses_named_agents(void)
{
   g_parallel_mode = 0;
   g_named_calls = 0;
   g_aggregator_calls = 0;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_SEQUENTIAL;
   opts.max_rounds = 1;
   opts.converge_threshold = 10;
   opts.deadline_ms = 0;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "review this proposed design for correctness", &opts, &result);
   assert(rc == 0);
   assert(result.artifact != NULL);
   assert(g_named_calls == 3);
   assert(result.rounds_run == 1);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_sequential_uses_named_agents: ok\n");
}

static void test_roundtable_degrades_on_min_success(void)
{
   g_parallel_mode = 1;
   g_parallel_calls = 0;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   opts.deadline_ms = 0;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft with too few successful participants", &opts, &result);
   assert(rc == 0);
   assert(result.degraded == 1);
   assert(result.artifact != NULL);
   assert(strstr(result.artifact, "only one answer") != NULL);
   delegate_roundtable_result_free(&result);
   g_parallel_mode = 0;
   printf("  test_roundtable_degrades_on_min_success: ok\n");
}

int main(void)
{
   printf("delegate_ensemble tests\n");
   test_ensemble_disabled();
   test_ensemble_null_args();
   test_ensemble_basic();
   test_ensemble_cost_cap();
   test_ensemble_min_successful_degradation();
   test_ensemble_routes_to_distinct_agents();
   test_roundtable_parallel_basic();
   test_roundtable_sequential_uses_named_agents();
   test_roundtable_degrades_on_min_success();
   printf("all tests passed\n");
   return 0;
}
