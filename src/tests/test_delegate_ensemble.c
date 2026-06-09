/* test_delegate_ensemble.c: unit tests for MoA ensemble fan-out and synthesis. */
#include "aimee.h"
#include "delegate_ensemble.h"
#include "model_registry.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- stubs for agent exec functions --- */

static int g_parallel_mode = 0; /* 0=all-succeed, 1=only-first-succeeds */
static int g_aggregator_mode = 0;
static int g_reason_mode = 0;
static int g_repair_mode = 0;
static int g_parallel_calls = 0;
static int g_named_calls = 0;
static int g_aggregator_calls = 0;
static int g_cancel_after_checks = -1;
static char g_last_parallel_prompt[8192];

/* Capture the per-task participant selector the engine sets, so a test can
 * assert the §0.1 routing fix: each fan-out task must be pointed at its own
 * configured reference agent, not left NULL (which routed all N to the one
 * default agent). The old stub ignored `tasks` entirely — that blind spot is
 * exactly why the unrouted-references bug shipped unseen. */
#define CAP_MAX 8
static char g_captured_agents[CAP_MAX][128];
static int g_captured_count = 0;

int model_capability_get(const char *provider, const char *model_id, model_capability_t *out)
{
   if (!out || !provider || strcmp(provider, "priced") != 0 || !model_id || !model_id[0])
      return 0;
   memset(out, 0, sizeof(*out));
   snprintf(out->provider, sizeof(out->provider), "%s", provider);
   snprintf(out->model_id, sizeof(out->model_id), "%s", model_id);
   out->cost_in_per_mtok = 1.0;
   out->cost_out_per_mtok = 3.0;
   return 1;
}

int agent_run_parallel(agent_config_t *cfg, agent_task_t *tasks, int count, agent_result_t *out)
{
   (void)cfg;
   g_parallel_calls++;
   snprintf(g_last_parallel_prompt, sizeof(g_last_parallel_prompt), "%s",
            count > 0 && tasks[0].user_prompt ? tasks[0].user_prompt : "");
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
   if (g_parallel_mode == 4)
   {
      struct timespec ts = {0, 3000000};
      nanosleep(&ts, NULL);
   }
   for (int i = 0; i < count; i++)
   {
      char buf[128];
      snprintf(out[i].agent_name, sizeof(out[i].agent_name), "%s",
               tasks[i].agent ? tasks[i].agent : "");
      if (g_parallel_mode == 2 && tasks[i].role && strcmp(tasks[i].role, "review") == 0)
         out[i].response = strdup("{\"issues\":[{\"severity\":\"blocking\",\"category\":\"api\","
                                  "\"location\":\"src/a.c:10\",\"summary\":\"same bug\","
                                  "\"recommendation\":\"fix it\"}],\"overall\":\"block\"}");
      else if (g_parallel_mode == 3 && tasks[i].role && strcmp(tasks[i].role, "review") == 0)
         out[i].response = strdup(i == 0 ? "not json" : "{\"issues\":[],\"overall\":\"ok\"}");
      else if (g_parallel_mode == 5 && tasks[i].role && strcmp(tasks[i].role, "review") == 0)
         out[i].response = strdup(
             "{\"items\":[{\"severity\":\"blocking\",\"category\":\"security\","
             "\"summary\":\"missing authorization check before write\"}],\"overall\":\"block\"}");
      else if (g_parallel_mode == 6 && tasks[i].role && strcmp(tasks[i].role, "review") == 0)
         out[i].response = strdup("{\"items\":[],\"overall\":\"ok\"}");
      else if (g_parallel_mode == 7 && tasks[i].role && strcmp(tasks[i].role, "review") == 0)
         out[i].response =
             strdup(i == 0 ? "{\"items\":[{\"severity\":\"blocking\",\"category\":\"correctness\","
                             "\"location\":\"src/a.c:10\",\"summary\":\"first bug\","
                             "\"recommendation\":\"fix first\"},{\"severity\":\"suggestion\","
                             "\"category\":\"correctness\",\"location\":\"src/a.c:10\","
                             "\"summary\":\"second bug\",\"recommendation\":\"fix second\"}],"
                             "\"overall\":\"mixed\"}"
                           : "{\"items\":[{\"severity\":\"nit\",\"category\":\"style\","
                             "\"location\":\"src/b.c:2\",\"summary\":\"rename local\","
                             "\"recommendation\":\"use clearer name\"}],\"overall\":\"nit\"}");
      else
      {
         snprintf(buf, sizeof(buf), "mock response from %s",
                  tasks[i].agent ? tasks[i].agent : "default");
         out[i].response = strdup(buf);
      }
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
   if (role && strcmp(role, "review") == 0 &&
       strstr(user_prompt ? user_prompt : "", "Repair this malformed roundtable review"))
   {
      out->response =
          strdup(g_repair_mode == 1 ? "{\"items\":[{\"severity\":\"blocking\","
                                      "\"category\":\"correctness\",\"location\":\"src/fixed.c:9\","
                                      "\"summary\":\"fixed malformed review\"}]}"
                                    : "still not json");
   }
   else if (role && strcmp(role, "review") == 0)
      out->response = strdup("{\"items\":[],\"overall\":\"ok\"}");
   else
   {
      char buf[128];
      snprintf(buf, sizeof(buf), "named response from %s", name ? name : "missing");
      out->response = strdup(buf);
   }
   out->prompt_tokens = 40;
   out->completion_tokens = 20;
   snprintf(out->agent_name, sizeof(out->agent_name), "%s", name ? name : "");
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
   {
      if (strstr(user_prompt ? user_prompt : "", "Answer the caller's roundtable review questions"))
         out->response = strdup("{\"answered_questions\":[{\"question\":\"does auth hold?\","
                                "\"answer\":\"yes\",\"evidence\":\"review mentions auth\","
                                "\"answered\":true}],\"coverage_gaps\":[]}");
      else if (strstr(user_prompt ? user_prompt : "", "{\"completion\":N}"))
         out->response = strdup("{\"completion\":95}");
      else if (g_reason_mode == 1)
         out->response =
             strdup(strstr(user_prompt ? user_prompt : "", "synthesized answer 1") ? "95" : "10");
      else
         out->response = strdup("80");
   }
   else
   {
      char buf[128];
      g_aggregator_calls++;
      if (g_aggregator_mode == 1)
         out->response =
             strdup(g_aggregator_calls == 1 ? "synthesized answer 1" : "inferior final artifact");
      else if (g_aggregator_mode == 2)
      {
         size_t n = 26000;
         out->response = malloc(n);
         memset(out->response, 'a', n - 2);
         out->response[n - 2] = '\n';
         out->response[n - 1] = '\0';
      }
      else
      {
         snprintf(buf, sizeof(buf), "synthesized answer %d", g_aggregator_calls);
         out->response = strdup(buf);
      }
   }
   out->prompt_tokens = 200;
   out->completion_tokens = 100;
   snprintf(out->agent_name, sizeof(out->agent_name), "%s", role ? role : "");
   out->success = 1;
   return 0;
}

static int test_cancel_requested(void *ctx)
{
   (void)ctx;
   if (g_cancel_after_checks < 0)
      return 0;
   return g_cancel_after_checks-- <= 0;
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

static agent_config_t make_priced_acfg(void)
{
   agent_config_t acfg = make_acfg();
   const char *names[] = {"model-a", "model-b", "model-c", "review", "reason", "draft"};
   acfg.agent_count = (int)(sizeof(names) / sizeof(names[0]));
   for (int i = 0; i < acfg.agent_count; i++)
   {
      snprintf(acfg.agents[i].name, sizeof(acfg.agents[i].name), "%s", names[i]);
      snprintf(acfg.agents[i].provider, sizeof(acfg.agents[i].provider), "%s", "priced");
      snprintf(acfg.agents[i].model, sizeof(acfg.agents[i].model), "priced-%s", names[i]);
   }
   return acfg;
}

static void reset_modes(void)
{
   g_parallel_mode = 0;
   g_aggregator_mode = 0;
   g_reason_mode = 0;
   g_repair_mode = 0;
   g_parallel_calls = 0;
   g_named_calls = 0;
   g_aggregator_calls = 0;
   g_cancel_after_checks = -1;
   g_last_parallel_prompt[0] = '\0';
}

/* --- tests --- */

static void test_ensemble_basic(void)
{
   reset_modes();
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

static void test_ensemble_cost_uses_model_registry_prices(void)
{
   reset_modes();
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_priced_acfg();
   delegate_ensemble_result_t result;
   int rc = delegate_ensemble_run(&acfg, &cfg, "priced question", &result);
   assert(rc == 0);
   /* 3 refs at 50 input + 50 output each, plus aggregator at 200 input + 100 output:
    * (350 * $1/MTok) + (250 * $3/MTok) = $0.0011. The old flat fallback would be
    * 600 * $15/MTok = $0.009, so this catches regressions to global pricing. */
   assert(result.cost_usd > 0.00109 && result.cost_usd < 0.00111);
   printf("  test_ensemble_cost_uses_model_registry_prices: ok\n");
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
   reset_modes();
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
   int rc =
       delegate_roundtable_run(&acfg, &cfg, "draft a short engineering proposal", &opts, &result);
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
   reset_modes();
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
   int rc = delegate_roundtable_run(&acfg, &cfg, "review this proposed design for correctness",
                                    &opts, &result);
   assert(rc == 0);
   assert(result.artifact != NULL);
   assert(g_named_calls == 3);
   assert(result.rounds_run == 1);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_sequential_uses_named_agents: ok\n");
}

static void test_roundtable_degrades_on_min_success(void)
{
   reset_modes();
   g_parallel_mode = 1;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   opts.deadline_ms = 0;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft with too few successful participants",
                                    &opts, &result);
   assert(rc == 0);
   assert(result.degraded == 1);
   assert(result.artifact != NULL);
   assert(strstr(result.artifact, "only one answer") != NULL);
   delegate_roundtable_result_free(&result);
   g_parallel_mode = 0;
   printf("  test_roundtable_degrades_on_min_success: ok\n");
}

static void test_roundtable_preflight_cap_warns_observed_cap_stops(void)
{
   reset_modes();
   config_t cfg = make_cfg(1, 2, 0.0001);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 3;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft a capped proposal", &opts, &result);
   assert(rc == 0);
   assert(result.cost_capped == 1);
   assert(g_parallel_calls == 1);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_preflight_cap_warns_observed_cap_stops: ok\n");
}

static void test_roundtable_keep_best_not_last(void)
{
   reset_modes();
   g_aggregator_mode = 1;
   g_reason_mode = 1;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 2;
   opts.converge_threshold = 0;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft then regress", &opts, &result);
   assert(rc == 0);
   assert(strcmp(result.artifact, "synthesized answer 1") == 0);
   assert(result.best_round == 1);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_keep_best_not_last: ok\n");
}

static void test_roundtable_post_fanout_cap_keeps_prior_best(void)
{
   reset_modes();
   g_aggregator_mode = 1;
   g_reason_mode = 1;
   config_t cfg = make_cfg(1, 2, 0.015);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 2;
   opts.converge_threshold = 0;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft then hit cap", &opts, &result);
   assert(rc == 0);
   assert(result.cost_capped == 1);
   assert(strcmp(result.artifact, "synthesized answer 1") == 0);
   assert(result.best_round == 1);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_post_fanout_cap_keeps_prior_best: ok\n");
}

static void test_roundtable_summarize_forward_sets_truncated(void)
{
   reset_modes();
   g_aggregator_mode = 2;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 2;
   opts.converge_threshold = 0;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft a very large artifact", &opts, &result);
   assert(rc == 0);
   assert(result.truncated == 1);
   assert(result.degraded == 1);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_summarize_forward_sets_truncated: ok\n");
}

static void test_roundtable_review_saturation_converges(void)
{
   reset_modes();
   g_parallel_mode = 2;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 3;
   opts.converge_threshold = 0;
   roundtable_result_t result;
   int rc =
       delegate_roundtable_run(&acfg, &cfg, "review with repeated blocking issue", &opts, &result);
   assert(rc == 0);
   assert(result.converged == 1);
   assert(result.rounds_run == 2);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_review_saturation_converges: ok\n");
}

static void test_roundtable_review_brief_and_items_return(void)
{
   reset_modes();
   g_parallel_mode = 7;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   const char *questions[] = {"does auth hold?"};
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   opts.brief = "focus:\n- auth checks\n";
   opts.questions = questions;
   opts.question_count = 1;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "review all severities", &opts, &result);
   assert(rc == 0);
   assert(strstr(g_last_parallel_prompt, "CALLER BRIEF:") != NULL);
   assert(strstr(g_last_parallel_prompt, "report any blocking issue") != NULL);
   assert(result.item_count == 3);
   assert(strcmp(result.items[0].severity, "blocking") == 0);
   assert(strcmp(result.items[1].severity, "suggestion") == 0);
   assert(strcmp(result.items[2].severity, "nit") == 0);
   assert(strcmp(result.items[0].identity_key, result.items[1].identity_key) == 0);
   assert(strcmp(result.items[0].summary, result.items[1].summary) != 0);
   assert(result.answered_question_count == 1);
   assert(result.answered_questions[0].answered == 1);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_review_brief_and_items_return: ok\n");
}

static void test_roundtable_cost_capped_skips_question_pass(void)
{
   reset_modes();
   g_parallel_mode = 7;
   config_t cfg = make_cfg(1, 2, 0.001); /* tiny cap trips after round 1 */
   agent_config_t acfg = make_acfg();
   const char *questions[] = {"does auth hold?"};
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 2;
   opts.questions = questions;
   opts.question_count = 1;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "review with cost cap", &opts, &result);
   assert(rc == 0);
   assert(result.cost_capped == 1);
   /* The reason-role question pass is skipped on a cost-capped run; the mock would
    * have answered "does auth hold?" with answered=true, so answered==0 proves it
    * was skipped while the question is still reported as an unanswered gap. */
   assert(result.answered_question_count == 1);
   assert(result.answered_questions[0].answered == 0);
   assert(strcmp(result.answered_questions[0].question, "does auth hold?") == 0);
   assert(result.coverage_gap_count == 1);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_cost_capped_skips_question_pass: ok\n");
}

static void test_roundtable_review_summary_fallback_key_converges(void)
{
   reset_modes();
   g_parallel_mode = 5;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 3;
   opts.converge_threshold = 0;
   roundtable_result_t result;
   int rc =
       delegate_roundtable_run(&acfg, &cfg, "review with no-location blockers", &opts, &result);
   assert(rc == 0);
   assert(result.converged == 1);
   assert(result.rounds_run == 2);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_review_summary_fallback_key_converges: ok\n");
}

static void test_roundtable_review_clean_round_converges(void)
{
   reset_modes();
   g_parallel_mode = 6;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 3;
   opts.converge_threshold = 0;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "review clean document", &opts, &result);
   assert(rc == 0);
   assert(result.converged == 1);
   assert(result.rounds_run == 1);
   assert(g_parallel_calls == 1);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_review_clean_round_converges: ok\n");
}

static void test_roundtable_malformed_review_json_counts_failed(void)
{
   reset_modes();
   g_parallel_mode = 3;
   g_repair_mode = 2;
   config_t cfg = make_cfg(1, 3, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "review malformed json handling", &opts, &result);
   assert(rc == 0);
   assert(result.degraded == 1);
   assert(g_named_calls == 1);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_malformed_review_json_counts_failed: ok\n");
}

static void test_roundtable_malformed_review_json_repair_counts_successful(void)
{
   reset_modes();
   g_parallel_mode = 3;
   g_repair_mode = 1;
   config_t cfg = make_cfg(1, 3, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "review malformed json repair", &opts, &result);
   assert(rc == 0);
   assert(result.degraded == 0);
   assert(g_named_calls == 1);
   assert(result.item_count == 1);
   assert(strcmp(result.items[0].location, "src/fixed.c:9") == 0);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_malformed_review_json_repair_counts_successful: ok\n");
}

static void test_roundtable_cancellation_stops(void)
{
   reset_modes();
   g_cancel_after_checks = 0;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 3;
   opts.cancel_requested = test_cancel_requested;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft but cancel before work", &opts, &result);
   assert(rc == 0);
   assert(result.cancelled == 1);
   assert(g_parallel_calls == 0);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_cancellation_stops: ok\n");
}

static void test_roundtable_deadline_returns_best_so_far(void)
{
   reset_modes();
   g_parallel_mode = 4;
   config_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 3;
   opts.deadline_ms = 1;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft until deadline", &opts, &result);
   assert(rc == 0);
   assert(result.deadline_hit == 1);
   assert(result.rounds_run == 1);
   assert(result.artifact != NULL && result.artifact[0]);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_deadline_returns_best_so_far: ok\n");
}

int main(void)
{
   printf("delegate_ensemble tests\n");
   test_ensemble_disabled();
   test_ensemble_null_args();
   test_ensemble_basic();
   test_ensemble_cost_cap();
   test_ensemble_cost_uses_model_registry_prices();
   test_ensemble_min_successful_degradation();
   test_ensemble_routes_to_distinct_agents();
   test_roundtable_parallel_basic();
   test_roundtable_sequential_uses_named_agents();
   test_roundtable_degrades_on_min_success();
   test_roundtable_preflight_cap_warns_observed_cap_stops();
   test_roundtable_keep_best_not_last();
   test_roundtable_post_fanout_cap_keeps_prior_best();
   test_roundtable_summarize_forward_sets_truncated();
   test_roundtable_review_saturation_converges();
   test_roundtable_review_brief_and_items_return();
   test_roundtable_cost_capped_skips_question_pass();
   test_roundtable_review_summary_fallback_key_converges();
   test_roundtable_review_clean_round_converges();
   test_roundtable_malformed_review_json_counts_failed();
   test_roundtable_malformed_review_json_repair_counts_successful();
   test_roundtable_cancellation_stops();
   test_roundtable_deadline_returns_best_so_far();
   printf("all tests passed\n");
   return 0;
}
