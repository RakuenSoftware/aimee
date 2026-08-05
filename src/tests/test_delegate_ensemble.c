/* test_delegate_ensemble.c: unit tests for MoA ensemble fan-out and synthesis. */
#include "aimee.h"
#include "delegate_ensemble.h"
#include "delegate_ensemble_internal.h" /* parse_model_json_lenient */
#include "cJSON.h"
#include "model_registry.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- stubs for agent exec functions --- */

static int g_parallel_mode = 0; /* 0=all, 1=only-first, 9=last-fails, 10=first-fails,
                                 * 11=first seat fails only on the first fan-out */
static int g_aggregator_mode = 0;
static int g_reason_mode = 0;
static int g_scorer_calls = 0; /* counts run_quality_scorer ("reason" + score prompt) invocations */
static int g_repair_mode = 0;
static int g_parallel_calls = 0;
static int g_named_calls = 0;
static int g_aggregator_calls = 0;
static char g_aggregator_fallback_who[128] = "";
static int g_cancel_after_checks = -1;
static int g_suppress_successful_tool_calls = 0;
static char g_last_parallel_prompt[8192];
static int g_named_require_initial_tool;

void agent_run_require_initial_tool_call(int on)
{
   g_named_require_initial_tool = on ? 1 : 0;
}

/* Capture the per-task participant selector the engine sets, so a test can
 * assert the §0.1 routing fix: each fan-out task must be pointed at its own
 * configured reference agent, not left NULL (which routed all N to the one
 * default agent). The old stub ignored `tasks` entirely — that blind spot is
 * exactly why the unrouted-references bug shipped unseen. */
#define CAP_MAX 8
static char g_captured_agents[CAP_MAX][128];
static int g_captured_count = 0;
/* Per-participant system prompt (persona) the engine sets on each fan-out task,
 * captured to assert panel-composition wires a persona per panelist. The
 * persona_compose stub returns strdup(name), so the captured system_prompt is
 * the persona name. */
static char g_captured_personas[CAP_MAX][128];
static int g_captured_require_tool[CAP_MAX];

/* Stub: the engine composes a panelist persona into the system prompt. Return a
 * heap copy of the name so the caller's free() is balanced and the captured
 * system_prompt is the persona name. */
char *persona_compose_delegate_prompt(const char *name, const char *cwd, const char *base_prompt)
{
   (void)cwd;
   (void)base_prompt;
   if (!name)
      return NULL;
   return strdup(name);
}

int model_capability_get(const char *provider, const char *model_id, model_capability_t *out)
{
   (void)provider;
   if (!out || !model_id)
      return 0;
   /* The unified cost path (token_estimate_cost) looks models up by id with the
    * provider inferred, so key this stub on the model id rather than requiring an
    * explicit provider — matching how the real registry resolves these. */
   if (strncmp(model_id, "priced-", 7) == 0)
   {
      memset(out, 0, sizeof(*out));
      snprintf(out->provider, sizeof(out->provider), "%s", "priced");
      snprintf(out->model_id, sizeof(out->model_id), "%s", model_id);
      out->cost_in_per_mtok = 1.0;
      out->cost_out_per_mtok = 3.0;
      return 1;
   }
   /* A registry entry that resolves but is priced 0/0 — "unknown", not "free". */
   if (strncmp(model_id, "zero-", 5) == 0)
   {
      memset(out, 0, sizeof(*out));
      snprintf(out->model_id, sizeof(out->model_id), "%s", model_id);
      return 1;
   }
   return 0;
}

int agent_run_parallel(agent_config_t *cfg, agent_task_t *tasks, int count, agent_result_t *out,
                       int deadline_ms)
{
   (void)cfg;
   (void)deadline_ms;
   g_parallel_calls++;
   snprintf(g_last_parallel_prompt, sizeof(g_last_parallel_prompt), "%s",
            count > 0 && tasks[0].user_prompt ? tasks[0].user_prompt : "");
   g_captured_count = count < CAP_MAX ? count : CAP_MAX;
   for (int i = 0; i < g_captured_count; i++)
   {
      snprintf(g_captured_agents[i], sizeof(g_captured_agents[i]), "%s",
               tasks[i].agent ? tasks[i].agent : "(null)");
      snprintf(g_captured_personas[i], sizeof(g_captured_personas[i]), "%s",
               tasks[i].system_prompt ? tasks[i].system_prompt : "(null)");
      g_captured_require_tool[i] = tasks[i].require_initial_tool_call;
   }
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
      if ((g_parallel_mode == 9 && i == count - 1) || (g_parallel_mode == 10 && i == 0))
         continue;
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
      else if ((g_parallel_mode == 6 || g_parallel_mode == 10) && tasks[i].role &&
               strcmp(tasks[i].role, "review") == 0)
         out[i].response = strdup("{\"items\":[],\"overall\":\"ok\"}");
      else if (g_parallel_mode == 8 && tasks[i].role && strcmp(tasks[i].role, "review") == 0)
         /* review JSON wrapped in a markdown code fence + prose, as a persona'd
          * panelist actually returns it — the lenient parser must still extract it. */
         out[i].response =
             strdup("Here is my review:\n```json\n"
                    "{\"items\":[{\"severity\":\"blocking\",\"category\":\"correctness\","
                    "\"location\":\"src/a.c:1\",\"summary\":\"subtracts instead of adding\","
                    "\"recommendation\":\"use +\"}],\"overall\":\"block\"}\n```\n");
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
      out[i].tool_calls = tasks[i].require_initial_tool_call ? 1 : 0;
      out[i].successful_tool_calls =
          !g_suppress_successful_tool_calls && tasks[i].require_initial_tool_call ? 1 : 0;
      out[i].success = 1;
   }
   return count;
}

static void fill_aggregator_response(agent_result_t *out, const char *who);
static int is_synthesis_prompt(const char *p);

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
   memset(out, 0, sizeof(*out));
   /* A bare-name aggregator is dispatched here with the synthesis prompt; it is
    * the aggregator, not a panel participant, so it is not a "named" call. */
   if (is_synthesis_prompt(user_prompt))
   {
      fill_aggregator_response(out, name);
      return 0;
   }
   g_named_calls++;
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

int agent_run_named_with_tools(agent_config_t *cfg, const char *name, const char *role,
                               const char *system_prompt, const char *user_prompt, int max_tokens,
                               double temperature, agent_result_t *out)
{
   int rc =
       agent_run_named(cfg, name, role, system_prompt, user_prompt, max_tokens, temperature, out);
   if (rc == 0 && g_named_require_initial_tool)
   {
      out->tool_calls = 1;
      out->successful_tool_calls = 1;
   }
   return rc;
}

/* The aggregator/synthesis step runs without tools: by NAME (agent_run_named)
 * when the aggregator is a bare agent name (a name is not a role, so role-
 * routing can't reach it), else by role (agent_run_ex). Both land here. */
static void fill_aggregator_response(agent_result_t *out, const char *who)
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
   else if (g_aggregator_mode == 3)
   {
      /* The primary aggregator returns empty (fails); a fallback panelist
       * synthesizes. Records who actually produced the artifact. */
      out->response = g_aggregator_calls == 1 ? NULL : strdup("fallback synthesized artifact");
      snprintf(g_aggregator_fallback_who, sizeof(g_aggregator_fallback_who), "%s", who ? who : "");
   }
   else
   {
      snprintf(buf, sizeof(buf), "synthesized answer %d", g_aggregator_calls);
      out->response = strdup(buf);
   }
   out->prompt_tokens = 200;
   out->completion_tokens = 100;
   snprintf(out->agent_name, sizeof(out->agent_name), "%s", who ? who : "");
   out->success = 1;
}

/* True for the aggregator/synthesis prompt (vs a panel round or a repair). */
static int is_synthesis_prompt(const char *p)
{
   return p && (strstr(p, "synthesis aggregator") || strstr(p, "consolidating"));
}

int agent_run_ex(agent_config_t *cfg, const char *role, const char *system_prompt,
                 const char *user_prompt, int max_tokens, double temperature, agent_result_t *out)
{
   (void)cfg;
   (void)system_prompt;
   (void)user_prompt;
   (void)max_tokens;
   (void)temperature;
   memset(out, 0, sizeof(*out));
   fill_aggregator_response(out, role);
   return 0;
}

/* Stubs for the delegate-run core's economics deps. agent_find returns NULL so
 * the (server-side cred pool) lease path is skipped — the panel agents are
 * client-held and have no pool — and the run falls through to the agent_run
 * stubs above; cost-fold is a no-op in the test (no parent session bound). */
agent_t *agent_find(agent_config_t *cfg, const char *name)
{
   /* Real linear search (matches the production resolver). make_acfg() has zero
    * agents so existing tests still resolve to NULL (lease path skipped); the
    * panel-authorization test populates agents to exercise the filter. */
   if (!cfg || !name)
      return NULL;
   for (int i = 0; i < cfg->agent_count; i++)
      if (strcmp(cfg->agents[i].name, name) == 0)
         return &cfg->agents[i];
   return NULL;
}
int agent_has_role(const agent_t *agent, const char *role)
{
   if (!agent || !role)
      return 0;
   for (int i = 0; i < agent->role_count; i++)
      if (strcmp(agent->roles[i], role) == 0 || strcmp(agent->roles[i], "all") == 0)
         return 1;
   return 0;
}
int agent_is_exec_role(const agent_t *agent, const char *role)
{
   if (!agent || !role)
      return 0;
   for (int i = 0; i < agent->exec_role_count; i++)
      if (strcmp(agent->exec_roles[i], role) == 0 || strcmp(agent->exec_roles[i], "all") == 0)
         return 1;
   return 0;
}
/* Stub: treat the agent literally named "claude" as the CLI-only agent. */
int agent_is_claude_cli(const agent_t *agent)
{
   return agent && strcmp(agent->name, "claude") == 0;
}
/* Stub: a bearer HTTP agent with no resolvable credentials is "unkeyed" -> not
 * available (the case the availability filter must drop); everything else is. */
int agent_is_available_for_routing(const agent_t *agent)
{
   if (!agent || !agent->enabled)
      return 0;
   if (strcmp(agent->auth_type, "bearer") == 0 && agent->credential_count == 0)
      return 0;
   return 1;
}
agent_route_block_t agent_routing_block_reason(const agent_t *agent, char *detail, size_t detail_sz)
{
   if (detail && detail_sz)
      detail[0] = '\0';
   if (!agent || !agent->enabled)
      return AGENT_ROUTE_POLICY_EXCLUDED;
   if (strcmp(agent->auth_type, "bearer") == 0 && agent->credential_count == 0)
      return AGENT_ROUTE_NO_CREDENTIALS;
   return AGENT_ROUTE_OK;
}
/* Stubs for the seat resolver pulled in via ensemble_resolve_random_seats. These
 * test seats are all specific model names (never "$random"), so rt_seat_is_random
 * returns 0 and the resolver passes them through unchanged; delegate_pick_for_role
 * is only reached for a "$random" seat and is never called here. */
int rt_seat_is_random(const char *model)
{
   return (!model || !model[0]) ? 1 : (strcmp(model, "$random") == 0);
}
int delegate_pick_for_role(agent_config_t *cfg, const char *role, const char *const exclude[],
                           int nexclude)
{
   (void)cfg;
   (void)role;
   (void)exclude;
   (void)nexclude;
   return -1;
}
static int g_cost_fold_calls = 0;
static double g_cost_fold_total = 0.0;
int db1_cost_fold_record(const char *parent_sid, const char *child_sid, double cost,
                         const char *source)
{
   (void)child_sid;
   (void)source;
   if (parent_sid && parent_sid[0])
   {
      g_cost_fold_calls++;
      g_cost_fold_total += cost;
   }
   return 0;
}
int delegate_credentials_load_file(const char *path, time_t now_epoch)
{
   (void)path;
   (void)now_epoch;
   return 0;
}
int delegate_credentials_acquire(const char *principal, const char *agent_name,
                                 const agent_credential_t *creds, int count, char *cred_name,
                                 size_t cred_name_sz, char *env, size_t env_sz)
{
   (void)principal;
   (void)agent_name;
   (void)creds;
   (void)count;
   (void)cred_name;
   (void)cred_name_sz;
   (void)env;
   (void)env_sz;
   return -1;
}
void delegate_credentials_release(const char *principal, const char *agent_name,
                                  const char *cred_name)
{
   (void)principal;
   (void)agent_name;
   (void)cred_name;
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
      if (strstr(user_prompt ? user_prompt : "", "Score this roundtable artifact"))
         g_scorer_calls++;
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

static ensemble_panel_t make_cfg(int enabled, int min_ok, double max_cost)
{
   ensemble_panel_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   (void)enabled;
   cfg.min_successful = min_ok;
   cfg.max_cost_usd = max_cost;
   cfg.reference_count = 3;
   snprintf(cfg.reference_models[0], 128, "model-a");
   snprintf(cfg.reference_models[1], 128, "model-b");
   snprintf(cfg.reference_models[2], 128, "model-c");
   snprintf(cfg.aggregator, sizeof(cfg.aggregator), "review");
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
   g_scorer_calls = 0;
   g_repair_mode = 0;
   g_parallel_calls = 0;
   g_named_calls = 0;
   g_aggregator_calls = 0;
   g_cancel_after_checks = -1;
   g_suppress_successful_tool_calls = 0;
   g_named_require_initial_tool = 0;
   g_last_parallel_prompt[0] = '\0';
}

/* --- tests --- */

static void test_ensemble_basic(void)
{
   reset_modes();
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   delegate_ensemble_result_t result;
   int rc = delegate_ensemble_run(&acfg, &cfg, "what is 2+2?", &result);
   assert(rc == 0);
   assert(result.success == 1);
   assert(!result.degraded);
   assert(!result.cost_capped);
   assert(result.response[0] != '\0');
   assert(delegate_ensemble_cost_usd(&result) > 0.0);
   /* Partial-failure metadata: all 3 participants succeeded. */
   assert(result.participants_total == 3);
   assert(result.participants_failed == 0);
   printf("  test_ensemble_basic: ok\n");
}

static void test_ensemble_min_successful_degradation(void)
{
   g_parallel_mode = 1; /* only first ref succeeds */
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   delegate_ensemble_result_t result;
   int rc = delegate_ensemble_run(&acfg, &cfg, "hard question?", &result);
   assert(rc == 0);
   assert(result.degraded == 1);
   assert(!result.cost_capped);
   /* Partial-failure metadata: only 1 of 3 participants returned a response. */
   assert(result.participants_total == 3);
   assert(result.participants_failed == 2);
   g_parallel_mode = 0;
   printf("  test_ensemble_min_successful_degradation: ok\n");
}

static void test_ensemble_cost_cap(void)
{
   g_parallel_mode = 0;
   /* default: 3 refs * (50+50) tokens * $0.000015 = $0.0045 > $0.001 cap */
   ensemble_panel_t cfg = make_cfg(1, 2, 0.001);
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
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
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

static void test_delegate_cost_estimate_uses_token_tracker(void)
{
   /* Real models priced in the shared token_tracker authority must be billed
    * through it (same source agent_log_call uses), not a divergent calculator. */
   double in_cost = delegate_cost_estimate_usd("anthropic", "claude-3-5-sonnet", 1000000, 0);
   assert(in_cost > 2.99 && in_cost < 3.01); /* $3.00/MTok input */
   double out_cost = delegate_cost_estimate_usd(NULL, "gpt-4o", 0, 1000000);
   assert(out_cost > 9.99 && out_cost < 10.01); /* $10.00/MTok output */

   /* A model the token_tracker table does not cover but the registry does
    * still prices via the registry fallback (no coverage regression). */
   model_capability_t cap;
   if (model_capability_get("gemini", "gemini-1.5-pro", &cap) && cap.cost_in_per_mtok > 0.0)
   {
      double g = delegate_cost_estimate_usd("gemini", "gemini-1.5-pro", 1000000, 0);
      assert(g > 0.0);
   }

   /* A genuinely unknown model falls back to a non-zero flat estimate. */
   double unknown = delegate_cost_estimate_usd(NULL, "totally-unknown-model-xyz", 1000, 1000);
   assert(unknown > 0.0);

   /* A registry entry priced 0/0 is "unknown", not "free": it must fall back to
    * the flat estimate rather than recording $0. */
   double zero = delegate_cost_estimate_usd(NULL, "zero-priced-model", 1000, 1000);
   assert(zero > 0.0);
   printf("  test_delegate_cost_estimate_uses_token_tracker: ok\n");
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
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
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

/* Regression: a large review TARGET (the incompressible task) must not trip the
 * carryover-compression path. Before the fix, `strlen(artifact)+strlen(peer_notes)
 * +strlen(task) > 22000` counted the task, so any single-shot review whose target
 * exceeded ~22 KB ran summarize_forward on round 1 (empty carryover) and flagged
 * the whole run truncated+degraded (and starved the panel). The trigger now counts
 * carryover only, so a big task passes through clean. */
static void test_roundtable_large_task_no_size_degrade(void)
{
   reset_modes();
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   /* DRAFT mode isolates the SIZE path: the mock panel + aggregator succeed
    * cleanly (they return a fixed "synthesized answer"), so the only thing a large
    * task can change is whether the size-triggered summarize_forward at
    * delegate_ensemble.c fires — which the mode-agnostic 22 KB check governs. */
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1; /* single-shot: no carryover to compress */
   opts.converge_threshold = 0;
   opts.deadline_ms = 0;

   size_t n = 30000; /* > 22 KB, so the OLD code summarized + flagged truncated */
   char *task = malloc(n + 1);
   assert(task);
   for (size_t i = 0; i < n; i++)
      task[i] = (i % 64 == 63) ? '\n' : 'x';
   task[n] = '\0';

   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, task, &opts, &result);
   free(task);
   assert(rc == 0);
   /* A large task alone (no carryover on round 1) must NOT trip the compression
    * path, so the run is neither truncated nor degraded by size. */
   assert(!result.truncated);
   assert(!result.degraded);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_large_task_no_size_degrade: ok\n");
}

static void test_roundtable_parallel_basic(void)
{
   reset_modes();
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
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
   /* Partial-failure metadata: full panel, no failures. */
   assert(result.participants_total == 3);
   assert(result.participants_failed == 0);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_parallel_basic: ok\n");
}

/* The ensemble runs through the delegate path: each billable panel + aggregator
 * run folds its cost onto the originating session (db1_cost_fold_record), and
 * only when a parent session is set. */
static void test_roundtable_folds_cost_to_parent_session(void)
{
   reset_modes();
   g_cost_fold_calls = 0;
   g_cost_fold_total = 0.0;
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   opts.parent_session_id = "parent-sess";
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft a short proposal", &opts, &result);
   assert(rc == 0);
   /* >= 2 panel participants + the aggregator all folded onto the parent. */
   assert(g_cost_fold_calls >= 3);
   assert(g_cost_fold_total > 0.0);
   delegate_roundtable_result_free(&result);

   /* No parent session -> no fold (the other tests run with parent unset). */
   g_cost_fold_calls = 0;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   rc = delegate_roundtable_run(&acfg, &cfg, "draft again", &opts, &result);
   assert(rc == 0);
   assert(g_cost_fold_calls == 0);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_folds_cost_to_parent_session: ok\n");
}

static void test_roundtable_sequential_uses_named_agents(void)
{
   reset_modes();
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
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
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
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
   /* Partial-failure metadata: panel of 3, only 1 participant responded. */
   assert(result.participants_total == 3);
   assert(result.participants_failed == 2);
   assert(result.participants_required_failed == 2);
   delegate_roundtable_result_free(&result);
   g_parallel_mode = 0;
   printf("  test_roundtable_degrades_on_min_success: ok\n");
}

static void test_roundtable_optional_failure_is_reported_without_degrading(void)
{
   reset_modes();
   g_parallel_mode = 9;
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   opts.required_participants = 2;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft with one unavailable optional seat", &opts,
                                    &result);
   assert(rc == 0);
   assert(result.degraded == 0);
   assert(result.participants_total == 3);
   assert(result.participants_failed == 1);
   assert(result.participants_required_failed == 0);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_optional_failure_is_reported_without_degrading: ok\n");
}

static void test_roundtable_required_failure_degrades_even_with_optional_success(void)
{
   reset_modes();
   g_parallel_mode = 10;
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   opts.required_participants = 2;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft missing one required seat", &opts, &result);
   assert(rc == 0);
   assert(result.degraded == 1);
   assert(result.participants_required_failed == 1);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_required_failure_degrades_even_with_optional_success: ok\n");
}

static void test_roundtable_required_malformed_review_degrades_when_repair_fails(void)
{
   reset_modes();
   g_parallel_mode = 3;
   g_repair_mode = 2;
   ensemble_panel_t cfg = make_cfg(1, 1, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   opts.required_participants = 1;
   roundtable_result_t result;
   int rc =
       delegate_roundtable_run(&acfg, &cfg, "review with malformed required seat", &opts, &result);
   assert(rc == 0);
   assert(result.degraded == 1);
   assert(result.participants_failed == 1);
   assert(result.participants_required_failed == 1);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_required_malformed_review_degrades_when_repair_fails: ok\n");
}

static void test_roundtable_rejects_required_prefix_larger_than_panel(void)
{
   reset_modes();
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   opts.required_participants = 4;
   roundtable_result_t result;
   assert(delegate_roundtable_run(&acfg, &cfg, "invalid required prefix", &opts, &result) == -1);
   assert(g_parallel_calls == 0);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_rejects_required_prefix_larger_than_panel: ok\n");
}

static void test_roundtable_preflight_cap_warns_observed_cap_stops(void)
{
   reset_modes();
   ensemble_panel_t cfg = make_cfg(1, 2, 0.0001);
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
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
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
   ensemble_panel_t cfg = make_cfg(1, 2, 0.015);
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
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
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
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
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
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
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

static void test_panel_eligibility_excludes_client_claude(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   acfg.agent_count = 4;
   acfg.agents[0].enabled = 1;
   snprintf(acfg.agents[0].name, MAX_AGENT_NAME, "mistral");
   snprintf(acfg.agents[0].roles[0], sizeof(acfg.agents[0].roles[0]), "review");
   acfg.agents[0].role_count = 1;
   acfg.agents[1].enabled = 1;
   snprintf(acfg.agents[1].name, MAX_AGENT_NAME, "claude"); /* CLI-only per stub */
   snprintf(acfg.agents[1].roles[0], sizeof(acfg.agents[1].roles[0]), "review");
   acfg.agents[1].role_count = 1;
   acfg.agents[2].enabled = 1;
   snprintf(acfg.agents[2].name, MAX_AGENT_NAME, "codex");
   snprintf(acfg.agents[2].roles[0], sizeof(acfg.agents[2].roles[0]), "review");
   acfg.agents[2].role_count = 1;
   acfg.agents[3].enabled = 1;
   snprintf(acfg.agents[3].name, MAX_AGENT_NAME, "code-only");
   snprintf(acfg.agents[3].roles[0], sizeof(acfg.agents[3].roles[0]), "code");
   acfg.agents[3].role_count = 1;

   ensemble_panel_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   assert(ensemble_panelist_eligible(&acfg.agents[0]) == 1);
   assert(ensemble_panelist_eligible(&acfg.agents[1]) == 0);
   assert(ensemble_panelist_eligible(&acfg.agents[2]) == 1);
   assert(ensemble_panelist_eligible(&acfg.agents[3]) == 0);

   /* claude needs BOTH authorization (NOT primary_only) AND server-hosting to be
    * seated; neither alone is enough. Authorization is now the per-agent
    * `primary_only` flag (0 = delegate-eligible), which replaced the global
    * claude_cli_delegate_enabled opt-in. */

   /* (a) authorized (primary_only=0) but client-only (not server-hosted) -> still
    * excluded */
   acfg.agents[1].primary_only = 0;
   assert(ensemble_panelist_eligible(&acfg.agents[1]) == 0);

   /* (b) server-hosted but NOT authorized (primary_only=1) -> still excluded (the
    * key invariant: a server-side OAuth setup is not authorization to act as a
    * panelist) */
   acfg.agents[1].is_server_hosted = 1;
   acfg.agents[1].primary_only = 1;
   assert(ensemble_panelist_eligible(&acfg.agents[1]) == 0);

   /* (c) authorized (primary_only=0) AND server-hosted -> seated */
   acfg.agents[1].primary_only = 0;
   assert(ensemble_panelist_eligible(&acfg.agents[1]) == 1);

   /* (d) disabled claude is never seated, even authorized + server-hosted */
   acfg.agents[1].enabled = 0;
   assert(ensemble_panelist_eligible(&acfg.agents[1]) == 0);
   printf("  test_panel_eligibility_excludes_client_claude: ok\n");
}

static void test_panel_filter_drops_unauthorized_claude(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   acfg.agent_count = 2;
   acfg.agents[0].enabled = 1;
   snprintf(acfg.agents[0].name, MAX_AGENT_NAME, "mistral");
   snprintf(acfg.agents[0].roles[0], sizeof(acfg.agents[0].roles[0]), "review");
   acfg.agents[0].role_count = 1;
   acfg.agents[1].enabled = 1;
   snprintf(acfg.agents[1].name, MAX_AGENT_NAME, "claude"); /* claude-CLI per stub */
   snprintf(acfg.agents[1].roles[0], sizeof(acfg.agents[1].roles[0]), "review");
   acfg.agents[1].role_count = 1;

   /* A positive pin must name a configured eligible agent. Unauthorized and
    * ad-hoc names are both dropped, and the aggregator is repointed. */
   ensemble_panel_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.reference_count = 3;
   snprintf(cfg.reference_models[0], 128, "mistral");
   snprintf(cfg.reference_models[1], 128, "claude");
   snprintf(cfg.reference_models[2], 128, "adhoc-model"); /* not an agent -> dropped */
   snprintf(cfg.aggregator, sizeof(cfg.aggregator), "claude");
   ensemble_filter_panel_authorization(&cfg, &acfg);
   assert(cfg.reference_count == 1);
   assert(strcmp(cfg.reference_models[0], "mistral") == 0);
   assert(strcmp(cfg.aggregator, "mistral") == 0); /* repointed off the dropped claude */

   /* Authorized (primary_only=0) + server-hosted claude survives the
    * explicit-list filter. */
   acfg.agents[1].is_server_hosted = 1;
   acfg.agents[1].primary_only = 0;
   ensemble_panel_t cfg2;
   memset(&cfg2, 0, sizeof(cfg2));
   cfg2.reference_count = 2;
   snprintf(cfg2.reference_models[0], 128, "mistral");
   snprintf(cfg2.reference_models[1], 128, "claude");
   ensemble_filter_panel_authorization(&cfg2, &acfg);
   assert(cfg2.reference_count == 2);
   printf("  test_panel_filter_drops_unauthorized_claude: ok\n");
}

static void test_panel_does_not_implicitly_exclude_primary(void)
{
   /* Primary-provider identity is not a hidden negative roster. If the agents
    * are enabled, review-capable, and not primary_only, they remain eligible. */
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   acfg.agent_count = 3;
   acfg.agents[0].enabled = 1;
   snprintf(acfg.agents[0].name, MAX_AGENT_NAME, "codex");
   snprintf(acfg.agents[0].provider, sizeof(acfg.agents[0].provider), "openai");
   snprintf(acfg.agents[0].roles[0], sizeof(acfg.agents[0].roles[0]), "review");
   acfg.agents[0].role_count = 1;
   acfg.agents[1].enabled = 1;
   acfg.agents[1].is_server_hosted = 1; /* would otherwise be an authorized claude */
   snprintf(acfg.agents[1].name, MAX_AGENT_NAME, "claude"); /* the primary passthrough */
   snprintf(acfg.agents[1].roles[0], sizeof(acfg.agents[1].roles[0]), "review");
   acfg.agents[1].role_count = 1;
   acfg.agents[2].enabled = 1;
   snprintf(acfg.agents[2].name, MAX_AGENT_NAME, "claude-api"); /* different name ... */
   snprintf(acfg.agents[2].provider, sizeof(acfg.agents[2].provider),
            "claude"); /* ... primary provider */
   snprintf(acfg.agents[2].roles[0], sizeof(acfg.agents[2].roles[0]), "review");
   acfg.agents[2].role_count = 1;

   ensemble_panel_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   /* claude is authorized by default (primary_only=0). The panel filter reads
    * agent authorization, not the primary provider, so no config setup is needed. */

   /* per-agent predicate */
   assert(ensemble_panelist_eligible(&acfg.agents[0]) == 1); /* codex/openai — seated */
   assert(ensemble_panelist_eligible(&acfg.agents[1]) == 1);
   assert(ensemble_panelist_eligible(&acfg.agents[2]) == 1);

   /* Explicit positive pins remain intact. */
   cfg.reference_count = 3;
   snprintf(cfg.reference_models[0], 128, "codex");
   snprintf(cfg.reference_models[1], 128, "claude");
   snprintf(cfg.reference_models[2], 128, "claude-api");
   snprintf(cfg.aggregator, sizeof(cfg.aggregator), "claude");
   ensemble_filter_panel_authorization(&cfg, &acfg);
   assert(cfg.reference_count == 3);
   assert(strcmp(cfg.reference_models[0], "codex") == 0);
   assert(strcmp(cfg.aggregator, "claude") == 0);

   /* An unconfigured/direct panel is bounded to two diverse agents. */
   ensemble_panel_t seed_cfg;
   memset(&seed_cfg, 0, sizeof(seed_cfg));
   /* The implicit panel is built from agent availability, not the primary
    * provider, so the panel needs no provider seed. */
   ensemble_fill_implicit_panel(&seed_cfg, &acfg);
   assert(seed_cfg.reference_count == 2);
   assert(strcmp(seed_cfg.reference_models[0], "codex") == 0);

   printf("  test_panel_does_not_implicitly_exclude_primary: ok\n");
}

static void test_panel_filter_drops_unavailable(void)
{
   /* A configured panelist that is enabled but NOT runtime-usable is dropped;
    * an ad-hoc name is not a configured agent and is dropped too. */
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   acfg.agent_count = 1;
   acfg.agents[0].enabled = 1;
   snprintf(acfg.agents[0].name, MAX_AGENT_NAME, "unkeyed");
   snprintf(acfg.agents[0].auth_type, sizeof(acfg.agents[0].auth_type), "bearer"); /* needs a key */

   ensemble_panel_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.reference_count = 2;
   snprintf(cfg.reference_models[0], 128, "unkeyed");     /* configured + no key -> drop */
   snprintf(cfg.reference_models[1], 128, "adhoc-model"); /* not an agent -> dropped */
   snprintf(cfg.aggregator, sizeof(cfg.aggregator), "unkeyed");
   ensemble_filter_panel_availability(&cfg, &acfg);
   assert(cfg.reference_count == 0);
   assert(cfg.aggregator[0] == '\0');
   printf("  test_panel_filter_drops_unavailable: ok\n");
}

static void test_specific_panel_pin_is_hard_requirement(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   acfg.agent_count = 1;
   acfg.agents[0].enabled = 1;
   snprintf(acfg.agents[0].name, sizeof(acfg.agents[0].name), "unkeyed");
   snprintf(acfg.agents[0].roles[0], sizeof(acfg.agents[0].roles[0]), "review");
   acfg.agents[0].role_count = 1;
   snprintf(acfg.agents[0].auth_type, sizeof(acfg.agents[0].auth_type), "bearer");

   ensemble_panel_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.reference_count = 1;
   snprintf(cfg.reference_models[0], sizeof(cfg.reference_models[0]), "unkeyed");
   char err[256];
   assert(ensemble_validate_panel_pins(&cfg, &acfg, err, sizeof(err)) == -1);
   assert(strstr(err, "required roundtable agent 'unkeyed'") != NULL);

   snprintf(cfg.reference_models[0], sizeof(cfg.reference_models[0]), "$random");
   assert(ensemble_validate_panel_pins(&cfg, &acfg, err, sizeof(err)) == 0);

   /* Repeated concrete pins are distinct must-use invocation seats, but cannot
    * promise more simultaneous invocations than the agent itself permits. */
   snprintf(acfg.agents[0].auth_type, sizeof(acfg.agents[0].auth_type), "none");
   acfg.agents[0].max_parallel = 1;
   cfg.reference_count = 2;
   snprintf(cfg.reference_models[0], sizeof(cfg.reference_models[0]), "unkeyed");
   snprintf(cfg.reference_models[1], sizeof(cfg.reference_models[1]), "unkeyed");
   assert(ensemble_validate_panel_pins(&cfg, &acfg, err, sizeof(err)) == -1);
   assert(strstr(err, "max_parallel") != NULL);
   printf("  test_specific_panel_pin_is_hard_requirement: ok\n");
}

static void test_implicit_panel_ignores_legacy_roster_and_caps_two(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   acfg.agent_count = 2;
   agent_t *codex = &acfg.agents[0];
   codex->enabled = 1;
   codex->max_parallel = 10;
   snprintf(codex->name, sizeof(codex->name), "codex");
   snprintf(codex->provider, sizeof(codex->provider), "chatgpt");
   snprintf(codex->roles[0], sizeof(codex->roles[0]), "review");
   codex->role_count = 1;
   agent_t *minimax = &acfg.agents[1];
   minimax->enabled = 1;
   minimax->max_parallel = 4;
   snprintf(minimax->name, sizeof(minimax->name), "MiniMax-M3");
   snprintf(minimax->provider, sizeof(minimax->provider), "anthropic");
   snprintf(minimax->roles[0], sizeof(minimax->roles[0]), "review");
   minimax->role_count = 1;

   ensemble_panel_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   /* Legacy ensemble fields do not authorize a larger/direct panel. */
   cfg.reference_count = 1;
   cfg.reference_persona_count = 1;
   snprintf(cfg.reference_models[0], sizeof(cfg.reference_models[0]), "MiniMax-M3");
   snprintf(cfg.reference_personas[0], sizeof(cfg.reference_personas[0]), "security");
   snprintf(cfg.aggregator, sizeof(cfg.aggregator), "MiniMax-M3");

   ensemble_fill_implicit_panel(&cfg, &acfg);

   assert(cfg.reference_count == 2);
   assert(strcmp(cfg.reference_models[0], "codex") == 0);
   assert(strcmp(cfg.reference_models[1], "MiniMax-M3") == 0);
   assert(strcmp(cfg.aggregator, "codex") == 0);

   reset_modes();
   delegate_ensemble_result_t result;
   assert(delegate_ensemble_run(&acfg, &cfg, "exercise every filled panel seat", &result) == 0);
   assert(result.participants_total == 2);
   assert(result.participants_failed == 0);
   printf("  test_implicit_panel_ignores_legacy_roster_and_caps_two: ok\n");
}

static void test_configured_random_seats_fill_balanced_capacity(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof acfg);
   acfg.agent_count = 2;
   const char *names[] = {"codex", "minimax"};
   const char *providers[] = {"openai", "minimax"};
   const int capacities[] = {3, 2};
   for (int i = 0; i < acfg.agent_count; i++)
   {
      agent_t *ag = &acfg.agents[i];
      ag->enabled = 1;
      ag->max_parallel = capacities[i];
      snprintf(ag->name, sizeof ag->name, "%s", names[i]);
      snprintf(ag->provider, sizeof ag->provider, "%s", providers[i]);
      snprintf(ag->roles[0], sizeof ag->roles[0], "review");
      ag->role_count = 1;
   }

   ensemble_panel_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.reference_count = 5;
   cfg.reference_persona_count = 5;
   for (int i = 0; i < cfg.reference_count; i++)
      snprintf(cfg.reference_models[i], sizeof cfg.reference_models[i], "$random");

   ensemble_resolve_random_seats(&cfg, &acfg);

   assert(cfg.reference_count == 5);
   assert(strcmp(cfg.reference_models[0], "codex") == 0);
   assert(strcmp(cfg.reference_models[1], "minimax") == 0);
   assert(strcmp(cfg.reference_models[2], "codex") == 0);
   assert(strcmp(cfg.reference_models[3], "minimax") == 0);
   assert(strcmp(cfg.reference_models[4], "codex") == 0);
   printf("  test_configured_random_seats_fill_balanced_capacity: ok\n");
}

static void test_panel_prioritizes_distinct_providers(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   acfg.agent_count = 3;
   const char *names[] = {"anthropic-a", "anthropic-b", "codex"};
   const char *providers[] = {"anthropic", "anthropic", "chatgpt"};
   for (int i = 0; i < acfg.agent_count; i++)
   {
      agent_t *ag = &acfg.agents[i];
      ag->enabled = 1;
      ag->max_parallel = 1;
      snprintf(ag->name, sizeof(ag->name), "%s", names[i]);
      snprintf(ag->provider, sizeof(ag->provider), "%s", providers[i]);
      snprintf(ag->roles[0], sizeof(ag->roles[0]), "review");
      ag->role_count = 1;
   }

   ensemble_panel_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   ensemble_fill_implicit_panel(&cfg, &acfg);
   assert(cfg.reference_count == 2);
   assert(strcmp(cfg.reference_models[0], "anthropic-a") == 0);
   assert(strcmp(cfg.reference_models[1], "codex") == 0);
   printf("  test_panel_prioritizes_distinct_providers: ok\n");
}

static void test_panel_treats_providerless_agents_as_distinct(void)
{
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   acfg.agent_count = 3;
   const char *names[] = {"legacy-a", "legacy-b", "codex"};
   for (int i = 0; i < acfg.agent_count; i++)
   {
      agent_t *ag = &acfg.agents[i];
      ag->enabled = 1;
      ag->max_parallel = 1;
      snprintf(ag->name, sizeof(ag->name), "%s", names[i]);
      snprintf(ag->roles[0], sizeof(ag->roles[0]), "review");
      ag->role_count = 1;
   }
   snprintf(acfg.agents[2].provider, sizeof(acfg.agents[2].provider), "chatgpt");

   ensemble_panel_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   ensemble_fill_implicit_panel(&cfg, &acfg);
   assert(cfg.reference_count == 2);
   /* An absent provider is scoped to the agent name, so unrelated legacy
    * agents are both represented during the provider-diversity pass. */
   assert(strcmp(cfg.reference_models[0], "legacy-a") == 0);
   assert(strcmp(cfg.reference_models[1], "legacy-b") == 0);
   printf("  test_panel_treats_providerless_agents_as_distinct: ok\n");
}

static void test_panel_persona_name_assignment(void)
{
   /* Drives the pure resolution table directly: panel_persona_name() now reads
    * live config, so exercising it would mean writing a config file between
    * every assertion. panel_persona_for_slot takes the two config-derived
    * inputs as parameters, which is the same coverage with no I/O. */

   /* REVIEW mode: round-robin the diverse default lineup keyed on the model
    * index (stable, independent of any sequential shuffle). */
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_REVIEW, 0, NULL, ""), "original-request") == 0);
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_REVIEW, 1, NULL, ""), "security") == 0);
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_REVIEW, 2, NULL, ""), "architect") == 0);
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_REVIEW, 3, NULL, ""), "qa") == 0);
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_REVIEW, 4, NULL, ""), "reviewer") == 0);
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_REVIEW, 5, NULL, ""), "reviewer-constructive") ==
          0);

   /* DRAFT authors every panelist as the configured default persona; an unset
    * default_persona falls back to the built-in `engineer`. */
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_DRAFT, 0, NULL, ""), "engineer") == 0);
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_DRAFT, 4, NULL, ""), "engineer") == 0);
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_DRAFT, 0, NULL, "architect"), "architect") == 0);
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_DRAFT, 4, NULL, "architect"), "architect") == 0);

   /* the configured default persona does not disturb the REVIEW lineup */
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_REVIEW, 0, NULL, "architect"),
                 "original-request") == 0);

   /* a configured persona pins to its model slot; an empty entry within the
    * configured range still falls back to the mode default */
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_REVIEW, 1, "security", ""), "security") == 0);
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_REVIEW, 0, "", ""), "original-request") == 0);
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_REVIEW, 3, NULL, ""), "qa") == 0);

   /* the per-slot override also binds in DRAFT; empty/beyond slots fall to engineer */
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_DRAFT, 1, "security", ""), "security") == 0);
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_DRAFT, 0, "", ""), "engineer") == 0);
   assert(strcmp(panel_persona_for_slot(ROUNDTABLE_DRAFT, 3, NULL, ""), "engineer") == 0);
   printf("  test_panel_persona_name_assignment: ok\n");
}

static void test_roundtable_review_assigns_personas(void)
{
   reset_modes();
   g_parallel_mode = 6; /* each review panelist returns {"items":[],"overall":"ok"} */
   memset(g_captured_personas, 0, sizeof(g_captured_personas));
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0); /* 3 reference models, no configured personas */
   cfg.require_evidence = 1;
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "review a change", &opts, &result);
   assert(rc == 0);
   /* the engine wired a distinct default-lineup persona onto each panelist's
    * system prompt (the compose stub echoes the persona name) */
   assert(strcmp(g_captured_personas[0], "original-request") == 0);
   assert(strcmp(g_captured_personas[1], "security") == 0);
   assert(strcmp(g_captured_personas[2], "architect") == 0);
   assert(g_captured_require_tool[0] == 1);
   assert(g_captured_require_tool[1] == 1);
   assert(g_captured_require_tool[2] == 1);
   assert(result.participants_tool_used == 3);
   assert(result.participant_tool_calls == 3);
   assert(result.participant_successful_tool_calls == 3);
   assert(result.evidence_coverage_incomplete == 0);
   /* A tool-capable panel must verify production wiring against what actually
    * ships, rather than treating a source-diff call site as runtime evidence. */
   assert(strstr(g_last_parallel_prompt, "which build target and implementation actually ship") !=
          NULL);
   assert(strstr(g_last_parallel_prompt, "label the claim unverified") != NULL);
   assert(strstr(g_last_parallel_prompt, "Do not infer shipped behavior from a diff") != NULL);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_review_assigns_personas: ok\n");
}

static void test_roundtable_no_tool_use_is_visible_degradation(void)
{
   reset_modes();
   g_parallel_mode = 6;
   g_suppress_successful_tool_calls = 1; /* provider called a tool, but it failed */
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   cfg.require_evidence = 1;
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   roundtable_result_t result;
   assert(delegate_roundtable_run(&acfg, &cfg, "audit production wiring", &opts, &result) == 0);
   assert(result.degraded == 1);
   assert(result.participants_tool_used == 0);
   assert(result.participant_tool_calls == 3);
   assert(result.participant_successful_tool_calls == 0);
   assert(result.evidence_coverage_incomplete == 1);
   assert(strstr(result.artifact, "Production-wiring and shipped-artifact conclusions remain") !=
          NULL);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_no_tool_use_is_visible_degradation: ok\n");
}

static void test_roundtable_failed_seat_is_incomplete_evidence_coverage(void)
{
   reset_modes();
   g_parallel_mode = 10; /* first configured seat produces no response or tool evidence */
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   cfg.require_evidence = 1;
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   roundtable_result_t result;
   assert(delegate_roundtable_run(&acfg, &cfg, "audit production wiring", &opts, &result) == 0);
   assert(result.participants_total == 3);
   assert(result.participants_failed == 1);
   assert(result.participants_tool_used == 2);
   assert(result.evidence_coverage_incomplete == 1);
   assert(result.degraded == 1);
   assert(strstr(result.artifact, "2/3 configured seats used") != NULL);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_failed_seat_is_incomplete_evidence_coverage: ok\n");
}

static void test_roundtable_captures_original_request_alignment(void)
{
   agent_result_t results[3];
   memset(results, 0, sizeof results);
   snprintf(results[0].agent_name, sizeof results[0].agent_name, "codex");
   results[0].response =
       "{\"original_request_alignment\":{\"status\":\"aligned\",\"summary\":\"implements "
       "the requested scheduler\"},\"items\":[]}";
   snprintf(results[1].agent_name, sizeof results[1].agent_name, "minimax");
   results[1].response =
       "{\"original_request_alignment\":{\"status\":\"drifted\",\"summary\":\"builds an "
       "unrequested dashboard instead\"},\"items\":[]}";
   snprintf(results[2].agent_name, sizeof results[2].agent_name, "kimi");
   results[2].response = "{\"items\":[]}";
   roundtable_result_t out;
   memset(&out, 0, sizeof out);
   capture_round_review_items(results, 3, &out, 2);
   assert(strcmp(out.original_request_alignment, "drifted") == 0);
   assert(strstr(out.original_request_alignment_summary, "unrequested dashboard") != NULL);
   assert(strcmp(out.original_request_alignment_sources, "minimax") == 0);

   results[1].response =
       "{\"original_request_alignment\":{\"status\":\"aligned\",\"summary\":\"\"},"
       "\"items\":[]}";
   memset(&out, 0, sizeof out);
   capture_round_review_items(results, 2, &out, 2);
   assert(strcmp(out.original_request_alignment, "aligned") == 0);
   assert(strstr(out.original_request_alignment_summary, "requested scheduler") != NULL);

   /* A stricter verdict without its required rationale remains stricter and
    * fails closed; it must not inherit an earlier aligned explanation. */
   results[1].response =
       "{\"original_request_alignment\":{\"status\":\"drifted\",\"summary\":\"\"},"
       "\"items\":[]}";
   memset(&out, 0, sizeof out);
   capture_round_review_items(results, 2, &out, 2);
   assert(strcmp(out.original_request_alignment, "drifted") == 0);
   assert(strstr(out.original_request_alignment_summary, "no explanation") != NULL);

   memset(&out, 0, sizeof out);
   capture_round_review_items(&results[2], 1, &out, 3);
   assert(strcmp(out.original_request_alignment, "unclear") == 0);
   assert(strstr(out.original_request_alignment_summary, "No panelist") != NULL);

   results[0].response = "{\"items\":[{\"severity\":\"suggestion\",\"category\":\"correctness\","
                         "\"location\":\"src/a.c:1\",\"summary\":\"same grounded fact\"}]}";
   results[0].successful_tool_calls = 0;
   memset(&out, 0, sizeof out);
   capture_round_review_items(results, 1, &out, 4);
   assert(out.item_count == 1);
   assert(out.items[0].tool_grounded == 0);

   results[1].response = results[0].response;
   results[1].successful_tool_calls = 1;
   memset(&out, 0, sizeof out);
   capture_round_review_items(results, 2, &out, 4);
   assert(out.item_count == 1);
   assert(out.items[0].tool_grounded == 1); /* grounded duplicate upgrades provenance */
   printf("  test_roundtable_captures_original_request_alignment: ok\n");
}

static void test_roundtable_aggregator_fallback_synthesizes(void)
{
   reset_modes();
   g_aggregator_mode = 3; /* primary aggregator returns empty; a fallback panelist synthesizes */
   g_aggregator_fallback_who[0] = '\0';
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 2;
   opts.converge_threshold = 0;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft needing synthesis", &opts, &result);
   assert(rc == 0);
   /* The primary aggregator returned empty on its first call; a fallback panelist
    * synthesized rather than collapsing the round to an empty artifact. */
   assert(result.artifact && result.artifact[0]);
   assert(g_aggregator_calls >= 2); /* primary (empty) + at least one fallback attempt */
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_aggregator_fallback_synthesizes: ok\n");
}

/* The proximate cause of empty reviews on large diffs: a panelist returns a PROSE
 * review that also contains `{...}` code snippets, wrapping the real
 * {"items":[...]} object. The old first-'{'-to-last-'}' slice spanned unrelated
 * braces and failed; the balanced-brace scan must still find the items object. */
static void test_parse_lenient_prose_with_code_braces(void)
{
   const char *resp =
       "Reviewing kb_graph_cycles:\n"
       "The DFS `if (w == start && dsp >= 1) { emit(); }` looks correct, and the\n"
       "struct `typedef struct { int a; } t;` is fine. Here is my verdict:\n"
       "{\"items\":[{\"severity\":\"blocking\",\"category\":\"correctness\","
       "\"location\":\"x.c:10\",\"summary\":\"off by one\",\"recommendation\":\"fix\"}],"
       "\"overall\":\"one issue\"}\n"
       "Note: the helper `free_all() { }` is also fine.\n";
   cJSON *root = parse_model_json_lenient(resp);
   assert(root != NULL);
   cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
   assert(cJSON_IsArray(items) && cJSON_GetArraySize(items) == 1);
   cJSON *it0 = cJSON_GetArrayItem(items, 0);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(it0, "severity")->valuestring, "blocking") == 0);
   cJSON_Delete(root);

   /* A brace inside a JSON string must not confuse the balance matcher. */
   cJSON *r2 = parse_model_json_lenient("prose {\"items\":[],\"overall\":\"has } brace\"} tail");
   assert(r2 != NULL);
   assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r2, "items")));
   cJSON_Delete(r2);

   /* Bare JSON still parses; pure prose with no object yields NULL. */
   cJSON *r3 = parse_model_json_lenient("{\"items\":[],\"overall\":\"ok\"}");
   assert(r3 != NULL);
   cJSON_Delete(r3);
   assert(parse_model_json_lenient("no json here at all") == NULL);
   printf("  test_parse_lenient_prose_with_code_braces: ok\n");
}

static void test_roundtable_review_parses_fenced_json(void)
{
   reset_modes();
   g_parallel_mode = 8; /* panelists return review JSON wrapped in a ```json fence + prose */
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "review fenced", &opts, &result);
   assert(rc == 0);
   /* the lenient parser strips the markdown fence/prose, so the review items are
    * captured instead of being dropped (which left artifact empty + degraded). */
   assert(result.item_count >= 1);
   assert(strcmp(result.items[0].severity, "blocking") == 0);
   assert(strstr(result.items[0].summary, "subtract") != NULL);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_review_parses_fenced_json: ok\n");
}

static void test_roundtable_single_round_skips_scorer(void)
{
   /* The cross-round quality scorer (an extra serial LLM call) is pure overhead
    * for a rounds:1 run and is skipped; multi-round still uses it. */
   reset_modes();
   g_parallel_mode = 5; /* panel returns valid review items */
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "review once", &opts, &result);
   assert(rc == 0);
   assert(g_scorer_calls == 0);     /* no cross-round scorer call */
   assert(g_aggregator_calls == 0); /* no synthesis LLM call — assembled from items */
   assert(result.item_count >= 1);
   assert(result.artifact && strstr(result.artifact, "authorization")); /* built from the items */
   delegate_roundtable_result_free(&result);

   /* multi-round still scores to pick the best round (regression guard). */
   reset_modes();
   g_aggregator_mode = 1;
   g_reason_mode = 1;
   ensemble_panel_t cfg2 = make_cfg(1, 2, 10.0);
   roundtable_opts_t opts2;
   memset(&opts2, 0, sizeof(opts2));
   opts2.mode = ROUNDTABLE_DRAFT;
   opts2.turns = ROUNDTABLE_PARALLEL;
   opts2.max_rounds = 2;
   opts2.converge_threshold = 0;
   roundtable_result_t result2;
   rc = delegate_roundtable_run(&acfg, &cfg2, "draft twice", &opts2, &result2);
   assert(rc == 0);
   assert(g_scorer_calls > 0);
   delegate_roundtable_result_free(&result2);
   printf("  test_roundtable_single_round_skips_scorer: ok\n");
}

static void test_roundtable_cost_capped_skips_question_pass(void)
{
   reset_modes();
   g_parallel_mode = 7;
   ensemble_panel_t cfg = make_cfg(1, 2, 0.001); /* tiny cap trips after round 1 */
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

static void test_roundtable_partial_question_answers_report_gaps(void)
{
   reset_modes();
   g_parallel_mode = 7;
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   /* The reason mock answers only "does auth hold?"; the second question is left
    * unanswered. The engine must still return one entry per asked question and
    * report the second as a coverage gap (not silently drop it). */
   const char *questions[] = {"does auth hold?", "is the cache safe?"};
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   opts.questions = questions;
   opts.question_count = 2;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "review two questions", &opts, &result);
   assert(rc == 0);
   assert(result.answered_question_count == 2);
   assert(strcmp(result.answered_questions[0].question, "does auth hold?") == 0);
   assert(result.answered_questions[0].answered == 1);
   assert(strcmp(result.answered_questions[1].question, "is the cache safe?") == 0);
   assert(result.answered_questions[1].answered == 0);
   assert(result.coverage_gap_count == 1);
   assert(strcmp(result.coverage_gaps[0], "is the cache safe?") == 0);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_partial_question_answers_report_gaps: ok\n");
}

static void test_roundtable_draft_brief_questions_not_answered(void)
{
   reset_modes();
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   /* Questions are a review-mode concept. A draft run that happens to carry a
    * brief with questions must not trigger the reason-role question pass or
    * return answered_questions/items (it would otherwise pay for a review-only
    * feature and emit an inconsistent draft contract). */
   const char *questions[] = {"does auth hold?"};
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   opts.converge_threshold = 0;
   opts.brief = "focus:\n- auth checks\n";
   opts.questions = questions;
   opts.question_count = 1;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "draft a short proposal", &opts, &result);
   assert(rc == 0);
   assert(result.answered_question_count == 0);
   assert(result.coverage_gap_count == 0);
   assert(result.item_count == 0);
   delegate_roundtable_result_free(&result);
   printf("  test_roundtable_draft_brief_questions_not_answered: ok\n");
}

static void test_roundtable_review_summary_fallback_key_converges(void)
{
   reset_modes();
   g_parallel_mode = 5;
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
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
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
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
   ensemble_panel_t cfg = make_cfg(1, 3, 10.0);
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
   ensemble_panel_t cfg = make_cfg(1, 3, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_REVIEW;
   opts.turns = ROUNDTABLE_PARALLEL;
   opts.max_rounds = 1;
   opts.required_participants = 1;
   roundtable_result_t result;
   int rc = delegate_roundtable_run(&acfg, &cfg, "review malformed json repair", &opts, &result);
   assert(rc == 0);
   assert(result.degraded == 0);
   assert(result.participants_required_failed == 0);
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
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
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
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   roundtable_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.mode = ROUNDTABLE_DRAFT;
   opts.turns = ROUNDTABLE_PARALLEL;
   /* One round exercises the post-panel deadline capture: there is no next
    * loop-top check to set deadline_hit for us. */
   opts.max_rounds = 1;
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

/* A ONE-SEAT PANEL MUST NOT RUN THE CHAIR PASS.
 *
 * The chair pass arbitrates between seats that disagree; with one reviewer it is
 * an extra delegate call and an extra failure mode. Measured: a one-seat
 * completeness review returned a correct blocking finding, the chair then died
 * on "unknown persona 'chairman'", and roundtable_status reported the whole run
 * FAILED -- a caller polling it discards findings that were right.
 *
 * Gated at the USE SITE, not at config load: a preset overlay is applied after
 * ensemble_panel_from_config and would put chair_synthesis back. */
static void test_one_seat_panel_skips_chair_pass(void)
{
   ensemble_panel_t p;
   memset(&p, 0, sizeof(p));
   p.chair_synthesis = 1;

   p.reference_count = 1;
   assert(!(p.chair_synthesis && p.reference_count > 1));

   p.reference_count = 2;
   assert(p.chair_synthesis && p.reference_count > 1);

   /* Off stays off regardless of seat count. */
   p.chair_synthesis = 0;
   p.reference_count = 5;
   assert(!(p.chair_synthesis && p.reference_count > 1));
   printf("  PASS: one-seat panel skips the chair pass\n");
}

int main(void)
{
   printf("delegate_ensemble tests\n");
   test_one_seat_panel_skips_chair_pass();
   test_ensemble_null_args();
   test_ensemble_basic();
   test_ensemble_cost_cap();
   test_ensemble_cost_uses_model_registry_prices();
   test_delegate_cost_estimate_uses_token_tracker();
   test_ensemble_min_successful_degradation();
   test_ensemble_routes_to_distinct_agents();
   test_roundtable_parallel_basic();
   test_roundtable_large_task_no_size_degrade();
   test_roundtable_folds_cost_to_parent_session();
   test_roundtable_sequential_uses_named_agents();
   test_roundtable_degrades_on_min_success();
   test_roundtable_optional_failure_is_reported_without_degrading();
   test_roundtable_required_failure_degrades_even_with_optional_success();
   test_roundtable_required_malformed_review_degrades_when_repair_fails();
   test_roundtable_rejects_required_prefix_larger_than_panel();
   test_roundtable_preflight_cap_warns_observed_cap_stops();
   test_roundtable_keep_best_not_last();
   test_roundtable_post_fanout_cap_keeps_prior_best();
   test_roundtable_summarize_forward_sets_truncated();
   test_roundtable_review_saturation_converges();
   test_roundtable_review_brief_and_items_return();
   test_panel_eligibility_excludes_client_claude();
   test_panel_filter_drops_unauthorized_claude();
   test_panel_does_not_implicitly_exclude_primary();
   test_panel_filter_drops_unavailable();
   test_specific_panel_pin_is_hard_requirement();
   test_implicit_panel_ignores_legacy_roster_and_caps_two();
   test_configured_random_seats_fill_balanced_capacity();
   test_panel_prioritizes_distinct_providers();
   test_panel_treats_providerless_agents_as_distinct();
   test_panel_persona_name_assignment();
   test_roundtable_review_assigns_personas();
   test_roundtable_no_tool_use_is_visible_degradation();
   test_roundtable_failed_seat_is_incomplete_evidence_coverage();
   test_roundtable_captures_original_request_alignment();
   test_roundtable_aggregator_fallback_synthesizes();
   test_parse_lenient_prose_with_code_braces();
   test_roundtable_review_parses_fenced_json();
   test_roundtable_single_round_skips_scorer();
   test_roundtable_cost_capped_skips_question_pass();
   test_roundtable_partial_question_answers_report_gaps();
   test_roundtable_draft_brief_questions_not_answered();
   test_roundtable_review_summary_fallback_key_converges();
   test_roundtable_review_clean_round_converges();
   test_roundtable_malformed_review_json_counts_failed();
   test_roundtable_malformed_review_json_repair_counts_successful();
   test_roundtable_cancellation_stops();
   test_roundtable_deadline_returns_best_so_far();
   printf("all tests passed\n");
   return 0;
}
