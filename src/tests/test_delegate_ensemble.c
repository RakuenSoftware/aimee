#include "support/ir_text_request.h"
/* test_delegate_ensemble.c: unit tests for MoA ensemble fan-out and synthesis. */
#include "aimee.h"
#include "delegate_ensemble.h"
#include "ensemble_request.h"
#include "delegate_ensemble_internal.h" /* parse_model_json_lenient */
#include "cJSON.h"
#include <aimee/ir/session_event.h>
#include "model_registry.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- stubs for agent exec functions --- */

/* Deadline for test_roundtable_deadline_returns_best_so_far. Paired with the
 * g_parallel_mode == 4 sleep below, which must be comfortably longer. */
#define ROUNDTABLE_DEADLINE_TEST_MS 200

static int g_peer_blocks_seen, g_fixture_response_id;
static int inspect_model_sources(const aimee_request_t *request)
{
   int sources = 0;
   for (int m = 0; m < request->n_messages; m++)
      for (int b = 0; b < request->messages[m].n_blocks; b++)
      {
         const aimee_block_t *block = &request->messages[m].blocks[b];
         if (!block->thinking_signature ||
             strcmp(block->thinking_signature, "typed-fixture-signature"))
            continue;
         assert(!strcmp(request->messages[m].role, "assistant"));
         assert(b == 1 && request->messages[m].n_blocks == 2 &&
                request->messages[m].blocks[0].type == AIMEE_BLK_TEXT);
         assert(!strcmp(
             request->messages[m].blocks[0].text,
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block->raw, "source_text"))));
         for (int previous = 0; previous < m; previous++)
            for (int k = 0; k < request->messages[previous].n_blocks; k++)
            {
               const aimee_block_t *prior = &request->messages[previous].blocks[k];
               assert(!prior->thinking_id || strcmp(prior->thinking_id, block->thinking_id));
            }
         assert(block->type == AIMEE_BLK_THINKING &&
                !strcmp(block->text, "private fixture reasoning") &&
                !strncmp(block->thinking_id, "typed-fixture-id-", 17) &&
                !strcmp(block->thinking_encrypted_content, "typed-fixture-opaque"));
         assert(block->context.origin == AIMEE_CTX_ORIGIN_MODEL &&
                block->context.authority == AIMEE_CTX_AUTH_EVIDENCE &&
                block->context.trust == AIMEE_CTX_TRUST_UNVERIFIED &&
                block->context.sensitivity == AIMEE_CTX_SENS_CONFIDENTIAL &&
                block->context.model_visible == 0 &&
                !strcmp(block->context.revision_domain, "fixture-owner") &&
                !strcmp(block->context.revision_scope, "fixture-scope") &&
                block->context.revision_epoch == 7);
         const aimee_context_meta_t *text_context = &request->messages[m].blocks[0].context;
         assert(text_context->origin == AIMEE_CTX_ORIGIN_MODEL &&
                text_context->authority == AIMEE_CTX_AUTH_EVIDENCE &&
                text_context->trust == AIMEE_CTX_TRUST_UNVERIFIED &&
                text_context->model_visible == 1 && text_context->revision_epoch == 7);
         assert(block->raw &&
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(block->raw, "fixture_response")));
         sources++;
      }
   g_peer_blocks_seen += sources;
   return sources;
}

static char *g_fixture_request_view;
static const char *fixture_request_view(const aimee_request_t *request)
{
   size_t size = 1;
   for (int i = 0; i < request->n_messages; i++)
      for (int j = 0; j < request->messages[i].n_blocks; j++)
         if (request->messages[i].blocks[j].text)
            size += strlen(request->messages[i].blocks[j].text) + 2;
   free(g_fixture_request_view);
   g_fixture_request_view = malloc(size);
   assert(g_fixture_request_view);
   int sources = inspect_model_sources(request);
   g_fixture_request_view[0] = 0;
   for (int i = 0; i < request->n_messages; i++)
      for (int j = 0; j < request->messages[i].n_blocks; j++)
      {
         const aimee_block_t *block = &request->messages[i].blocks[j];
         if (block->type == AIMEE_BLK_TEXT && block->text)
         {
            strcat(g_fixture_request_view, block->text);
            strcat(g_fixture_request_view, "\n");
         }
      }
   if (strstr(g_fixture_request_view, "You are a synthesis aggregator") ||
       strstr(g_fixture_request_view, "You are consolidating") ||
       strstr(g_fixture_request_view, "Score this roundtable artifact") ||
       strstr(g_fixture_request_view, "Repair this malformed roundtable review") ||
       strstr(g_fixture_request_view, "Summarize this roundtable context"))
      assert(sources > 0);
   if (strstr(g_fixture_request_view, "Judge whether the current roundtable artifact"))
      assert(sources >= 2);
   return g_fixture_request_view;
}

static int fixture_ensemble_run(agent_config_t *agents, const ensemble_panel_t *panel,
                                const char *text, delegate_ensemble_result_t *out)
{
   if (!text)
      return delegate_ensemble_run(agents, panel, NULL, out);
   aimee_request_t request = {0};
   assert(!aimee_ir_request_from_text(&request, NULL, text, 0, 0.3));
   int rc = delegate_ensemble_run(agents, panel, &request, out);
   aimee_request_free(&request);
   return rc;
}

static const aimee_request_t *g_expected_task;
static int g_task_copies, g_task_syntheses, g_expected_continuation;
static void assert_task_custody(const aimee_request_t *request, int synthesis)
{
   if (!g_expected_task)
      return;
   synthesis |= g_expected_continuation;
   assert(!aimee_ir_request_admit(request));
   assert(request->has_temperature && request->temperature == 0.0);
   assert(request->has_max_tokens && request->max_tokens == 128);
   assert(request->stream == g_expected_task->stream);
   assert((!request->model && !g_expected_task->model) ||
          !strcmp(request->model, g_expected_task->model));
   assert(request->n_system >= g_expected_task->n_system);
   for (int i = 0; i < g_expected_task->n_system; i++)
      assert(!strcmp(request->system[i].text, g_expected_task->system[i].text));
   for (int i = g_expected_task->n_system; i < request->n_system; i++)
      assert(request->system[i].context.origin == AIMEE_CTX_ORIGIN_PLATFORM &&
             request->system[i].context.authority == AIMEE_CTX_AUTH_POLICY);
   assert(request->n_messages >= g_expected_task->n_messages);
   for (int m = 0; m < g_expected_task->n_messages; m++)
   {
      const aimee_message_t *got = &request->messages[m], *want = &g_expected_task->messages[m];
      assert(!strcmp(got->role, want->role));
      assert(got->content_scalar == want->content_scalar);
      assert(got->n_blocks == want->n_blocks);
      for (int i = 0; i < want->n_blocks; i++)
      {
         aimee_message_t left = {.role = got->role, .blocks = &got->blocks[i], .n_blocks = 1};
         aimee_message_t right = {.role = want->role, .blocks = &want->blocks[i], .n_blocks = 1};
         cJSON *a = aimee_ir_message_json(&left), *b = aimee_ir_message_json(&right);
         assert(a && b && cJSON_Compare(a, b, 1));
         assert((!got->blocks[i].raw && !want->blocks[i].raw) ||
                cJSON_Compare(got->blocks[i].raw, want->blocks[i].raw, 1));
         cJSON_Delete(a);
         cJSON_Delete(b);
      }
   }
   if (synthesis)
   {
      const aimee_message_t *tail = &request->messages[request->n_messages - 1];
      const aimee_block_t *notes = &tail->blocks[tail->n_blocks - 1];
      assert(notes->context.origin == AIMEE_CTX_ORIGIN_PLATFORM &&
             notes->context.authority == AIMEE_CTX_AUTH_EVIDENCE &&
             notes->context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
      g_task_syntheses++;
   }
   else
      g_task_copies++;
}

static int
    g_usage_mode; /* 1=panel unknown, 2=synthesis unknown, 3=observed zero, 4=scorer unknown */
static int g_parallel_mode = 0; /* 0=all, 1=only-first, 9=last-fails, 10=first-fails,
                                 * 11=first seat fails only on the first fan-out */
static int g_aggregator_mode = 0;
static int g_reason_mode = 0;
static int g_tiebreak_calls;
static int g_scorer_calls = 0; /* counts run_quality_scorer ("reason" + score prompt) invocations */
static int g_repair_mode = 0;
static int g_parallel_calls = 0;
static int g_named_calls = 0;
static int g_aggregator_calls = 0;
static char g_aggregator_fallback_who[128] = "";
static int g_cancel_after_checks = -1;
static int g_cancel_after_score, g_score_delay, g_question_calls;
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

/* Scripted provider boundary, not a production display fallback. */
static void fixture_observation(agent_result_t *out, char *text)
{
   if (!text)
      return;
   assert(fixture_agent_result_from_text(out, text) == 0);
   free(text);
   aimee_response_t *response = &out->canonical_response;
   aimee_ir_block_set_context(&response->content[0], AIMEE_CTX_ORIGIN_MODEL,
                              AIMEE_CTX_AUTH_EVIDENCE, AIMEE_CTX_TRUST_UNVERIFIED,
                              AIMEE_CTX_SENS_INTERNAL, 1, "fixture-owner", "fixture-scope", 7);
   char identity[64];
   snprintf(identity, sizeof identity, "typed-fixture-id-%d", ++g_fixture_response_id);
   response->content = realloc(response->content, 2 * sizeof *response->content);
   assert(response->content);
   response->content[1] =
       (aimee_block_t){.type = AIMEE_BLK_THINKING,
                       .text = strdup("private fixture reasoning"),
                       .thinking_signature = strdup("typed-fixture-signature"),
                       .thinking_id = strdup(identity),
                       .thinking_encrypted_content = strdup("typed-fixture-opaque"),
                       .raw = cJSON_Parse("{\"fixture_response\":true}")};
   assert(
       cJSON_AddStringToObject(response->content[1].raw, "source_text", response->content[0].text));
   response->n_content = 2;
   aimee_ir_block_set_context(&response->content[1], AIMEE_CTX_ORIGIN_MODEL,
                              AIMEE_CTX_AUTH_EVIDENCE, AIMEE_CTX_TRUST_UNVERIFIED,
                              AIMEE_CTX_SENS_CONFIDENTIAL, 0, "fixture-owner", "fixture-scope", 7);

   int unknown =
       (g_usage_mode == 1 && out->prompt_tokens < 200) ||
       (g_usage_mode == 2 && out->prompt_tokens == 200 && strcmp(out->agent_name, "reason")) ||
       (g_usage_mode == 4 && !strcmp(out->agent_name, "reason")) ||
       (g_usage_mode == 5 && out->prompt_tokens == 200 && g_aggregator_calls == 2 &&
        strcmp(out->agent_name, "reason"));
   if (unknown || g_usage_mode == 3)
      out->prompt_tokens = out->completion_tokens = 0;
   out->canonical_response.usage_unobserved = unknown && g_usage_mode != 2;
   out->usage_unobserved = unknown && g_usage_mode == 2;
   out->canonical_response.usage_in = out->prompt_tokens;
   out->canonical_response.usage_out = out->completion_tokens;
   if (g_parallel_mode == 12)
      out->canonical_response.stop_reason = AIMEE_STOP_CONTENT_FILTER;
   if (g_parallel_mode == 13)
      out->success = 0;
}

int agent_run_parallel(agent_config_t *cfg, agent_task_t *tasks, int count, agent_result_t *out,
                       int deadline_ms)
{
   (void)cfg;
   (void)deadline_ms;
   g_parallel_calls++;
   snprintf(g_last_parallel_prompt, sizeof(g_last_parallel_prompt), "%s",
            count > 0 ? fixture_request_view(&tasks[0].request) : "");
   g_captured_count = count < CAP_MAX ? count : CAP_MAX;
   for (int i = 0; i < g_captured_count; i++)
   {
      assert_task_custody(&tasks[i].request, 0);
      snprintf(g_captured_agents[i], sizeof(g_captured_agents[i]), "%s",
               tasks[i].agent ? tasks[i].agent : "(null)");
      snprintf(g_captured_personas[i], sizeof(g_captured_personas[i]), "%s",
               tasks[i].request.n_system ? tasks[i].request.system[0].text : "(null)");
      g_captured_require_tool[i] = tasks[i].require_initial_tool_call;
   }
   for (int i = 0; i < count; i++)
      memset(&out[i], 0, sizeof(out[i]));
   if (g_parallel_mode == 1)
   {

      out[0].prompt_tokens = 50;
      out[0].completion_tokens = 50;
      out[0].success = 1;
      fixture_observation(&out[0], strdup("only one answer"));
      return 1;
   }
   if (g_parallel_mode == 4)
   {
      /* Must outlast ROUNDTABLE_DEADLINE_TEST_MS by enough that the post-panel
       * deadline check cannot miss, however the scheduler behaves. See the
       * comment on that constant. */
      struct timespec ts = {0, 400000000};
      nanosleep(&ts, NULL);
   }
   for (int i = 0; i < count; i++)
   {
      char buf[128];
      char *text = NULL;
      snprintf(out[i].agent_name, sizeof(out[i].agent_name), "%s",
               tasks[i].agent ? tasks[i].agent : "");
      if ((g_parallel_mode == 9 && i == count - 1) || (g_parallel_mode == 10 && i == 0))
         continue;
      if (g_parallel_mode == 2 && tasks[i].role && strcmp(tasks[i].role, "review") == 0)
         text = strdup("{\"issues\":[{\"severity\":\"blocking\",\"category\":\"api\","
                       "\"location\":\"src/a.c:10\",\"summary\":\"same bug\","
                       "\"recommendation\":\"fix it\"}],\"overall\":\"block\"}");
      else if (g_parallel_mode == 3 && tasks[i].role && strcmp(tasks[i].role, "review") == 0)
         text = strdup(i == 0 ? "not json" : "{\"issues\":[],\"overall\":\"ok\"}");
      else if (g_parallel_mode == 5 && tasks[i].role && strcmp(tasks[i].role, "review") == 0)
         text = strdup(
             "{\"items\":[{\"severity\":\"blocking\",\"category\":\"security\","
             "\"summary\":\"missing authorization check before write\"}],\"overall\":\"block\"}");
      else if ((g_parallel_mode == 6 || g_parallel_mode == 10) && tasks[i].role &&
               strcmp(tasks[i].role, "review") == 0)
         text = strdup("{\"items\":[],\"overall\":\"ok\"}");
      else if (g_parallel_mode == 8 && tasks[i].role && strcmp(tasks[i].role, "review") == 0)
         /* review JSON wrapped in a markdown code fence + prose, as a persona'd
          * panelist actually returns it — the lenient parser must still extract it. */
         text = strdup("Here is my review:\n```json\n"
                       "{\"items\":[{\"severity\":\"blocking\",\"category\":\"correctness\","
                       "\"location\":\"src/a.c:1\",\"summary\":\"subtracts instead of adding\","
                       "\"recommendation\":\"use +\"}],\"overall\":\"block\"}\n```\n");
      else if (g_parallel_mode == 7 && tasks[i].role && strcmp(tasks[i].role, "review") == 0)
         text =
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
         text = strdup(buf);
      }
      out[i].prompt_tokens = 50;
      out[i].completion_tokens = 50;
      out[i].tool_calls = tasks[i].require_initial_tool_call ? 1 : 0;
      out[i].successful_tool_calls =
          !g_suppress_successful_tool_calls && tasks[i].require_initial_tool_call ? 1 : 0;
      out[i].success = 1;
      fixture_observation(&out[i], text);
   }
   return count;
}

static void fill_aggregator_response(agent_result_t *out, const char *who);
static int is_synthesis_prompt(const char *p);

int agent_run_named(agent_config_t *cfg, const char *name, const char *role,
                    const aimee_request_t *request, agent_result_t *out)
{
   char *text = NULL;
   assert_task_custody(request, 1);
   const char *system_prompt = request->n_system ? request->system[0].text : NULL;
   const char *user_prompt = fixture_request_view(request);
   int max_tokens = request->has_max_tokens ? request->max_tokens : 0;
   double temperature = request->has_temperature ? request->temperature : 0.3;

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
      text =
          strdup(g_repair_mode == 1 ? "{\"items\":[{\"severity\":\"blocking\","
                                      "\"category\":\"correctness\",\"location\":\"src/fixed.c:9\","
                                      "\"summary\":\"fixed malformed review\"}]}"
                                    : "still not json");
   }
   else if (role && strcmp(role, "review") == 0)
      text = strdup("{\"items\":[],\"overall\":\"ok\"}");
   else
   {
      char buf[128];
      snprintf(buf, sizeof(buf), "named response from %s", name ? name : "missing");
      text = strdup(buf);
   }
   out->prompt_tokens = 40;
   out->completion_tokens = 20;
   snprintf(out->agent_name, sizeof(out->agent_name), "%s", name ? name : "");
   out->success = 1;
   fixture_observation(out, text);
   return 0;
}

int agent_run_named_with_tools(agent_config_t *cfg, const char *name, const char *role,
                               const aimee_request_t *request, agent_result_t *out)
{

   int rc = agent_run_named(cfg, name, role, request, out);
   if (rc == 0 && g_named_require_initial_tool)
   {
      out->tool_calls = 1;
      out->successful_tool_calls = 1;
   }
   return rc;
}

/* The aggregator/synthesis step runs without tools: by NAME (agent_run_named)
 * when the aggregator is a bare agent name (a name is not a role, so role-
 * routing can't reach it), else by role (agent_run). Both land here. */
static void fill_aggregator_response(agent_result_t *out, const char *who)
{
   char *text = NULL;
   char buf[128];
   g_aggregator_calls++;
   if (g_aggregator_mode == 1)
      text = strdup(g_aggregator_calls == 1 ? "synthesized answer 1" : "inferior final artifact");
   else if (g_aggregator_mode == 2)
   {
      size_t n = 26000;
      text = malloc(n);
      memset(text, 'a', n - 2);
      text[n - 2] = '\n';
      text[n - 1] = '\0';
   }
   else if (g_aggregator_mode == 3)
   {
      /* The primary aggregator returns empty (fails); a fallback panelist
       * synthesizes. Records who actually produced the artifact. */
      text = g_aggregator_calls == 1 ? NULL : strdup("fallback synthesized artifact");
      snprintf(g_aggregator_fallback_who, sizeof(g_aggregator_fallback_who), "%s", who ? who : "");
   }
   else
   {
      snprintf(buf, sizeof(buf), "synthesized answer %d", g_aggregator_calls);
      text = strdup(buf);
   }
   out->prompt_tokens = 200;
   out->completion_tokens = 100;
   snprintf(out->agent_name, sizeof(out->agent_name), "%s", who ? who : "");
   out->success = 1;
   fixture_observation(out, text);
}

/* True for the aggregator/synthesis prompt (vs a panel round or a repair). */
static int is_synthesis_prompt(const char *p)
{
   return p && (strstr(p, "synthesis aggregator") || strstr(p, "consolidating"));
}

int agent_run(agent_config_t *cfg, const char *role, const aimee_request_t *request,
              agent_result_t *out)
{
   assert_task_custody(request, 1);
   const char *system_prompt = request->n_system ? request->system[0].text : NULL;
   const char *user_prompt = fixture_request_view(request);
   int max_tokens = request->has_max_tokens ? request->max_tokens : 0;
   double temperature = request->has_temperature ? request->temperature : 0.3;

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
int agent_is_available_for_execution(const agent_t *agent, agent_route_execution_t execution)
{
   assert(execution == AGENT_ROUTE_EXECUTION_HOST);
   return agent_is_available_for_routing(agent);
}
agent_route_block_t agent_routing_block_reason_for_execution(const agent_t *agent,
                                                             agent_route_execution_t execution,
                                                             char *detail, size_t detail_sz)
{
   assert(execution == AGENT_ROUTE_EXECUTION_HOST);
   return agent_routing_block_reason(agent, detail, detail_sz);
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
                                       const aimee_request_t *request, int enforce_writes,
                                       agent_result_t *out)
{
   char *text = NULL;
   assert_task_custody(request, 1);
   const char *system_prompt = request->n_system ? request->system[0].text : NULL;
   const char *user_prompt = fixture_request_view(request);
   int max_tokens = request->has_max_tokens ? request->max_tokens : 0;

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
      {
         g_scorer_calls++;
         if (g_score_delay)
         {
            struct timespec delay = {0, 400000000};
            nanosleep(&delay, NULL);
         }
      }
      if (strstr(user_prompt ? user_prompt : "", "Answer the caller's roundtable review questions"))
         g_question_calls++;
      if (strstr(user_prompt ? user_prompt : "", "Answer the caller's roundtable review questions"))
         text = strdup("{\"answered_questions\":[{\"question\":\"does auth hold?\","
                       "\"answer\":\"yes\",\"evidence\":\"review mentions auth\","
                       "\"answered\":true}],\"coverage_gaps\":[]}");
      else if (strstr(user_prompt ? user_prompt : "", "{\"completion\":N}"))
      {
         g_tiebreak_calls++;
         text = strdup("{\"completion\":95}");
      }
      else if (g_reason_mode == 1)
         text =
             strdup(strstr(user_prompt ? user_prompt : "", "synthesized answer 1") ? "95" : "10");
      else
         text = strdup("80");
   }
   else
   {
      char buf[128];
      g_aggregator_calls++;
      if (g_aggregator_mode == 1)
         text =
             strdup(g_aggregator_calls == 1 ? "synthesized answer 1" : "inferior final artifact");
      else if (g_aggregator_mode == 2)
      {
         size_t n = 26000;
         text = malloc(n);
         memset(text, 'a', n - 2);
         text[n - 2] = '\n';
         text[n - 1] = '\0';
      }
      else
      {
         snprintf(buf, sizeof(buf), "synthesized answer %d", g_aggregator_calls);
         text = strdup(buf);
      }
   }
   out->prompt_tokens = 200;
   out->completion_tokens = 100;
   snprintf(out->agent_name, sizeof(out->agent_name), "%s", role ? role : "");
   out->success = 1;
   fixture_observation(out, text);
   return 0;
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
   g_usage_mode = 0;
   g_parallel_mode = 0;
   g_aggregator_mode = 0;
   g_reason_mode = 0;
   g_scorer_calls = 0;
   g_tiebreak_calls = 0;
   g_repair_mode = 0;
   g_parallel_calls = 0;
   g_named_calls = 0;
   g_aggregator_calls = 0;
   g_cancel_after_checks = -1;
   g_cancel_after_score = g_score_delay = g_question_calls = 0;
   g_suppress_successful_tool_calls = 0;
   g_named_require_initial_tool = 0;
   g_last_parallel_prompt[0] = '\0';
}

/* --- tests --- */

static void test_ensemble_min_successful_degradation(void)
{
   g_parallel_mode = 1; /* only first ref succeeds */
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_acfg();
   delegate_ensemble_result_t result = {0};
   int rc = fixture_ensemble_run(&acfg, &cfg, "hard question?", &result);
   assert(rc == 0);
   assert(result.degraded == 1);
   assert(!result.cost_capped);
   /* Partial-failure metadata: only 1 of 3 participants returned a response. */
   assert(result.participants_total == 3);
   assert(result.participants_failed == 2);
   g_parallel_mode = 0;
   printf("  test_ensemble_min_successful_degradation: ok\n");
   delegate_ensemble_result_free(&result);
}

static void test_ensemble_cost_cap(void)
{
   g_parallel_mode = 0;
   /* default: 3 refs * (50+50) tokens * $0.000015 = $0.0045 > $0.001 cap */
   ensemble_panel_t cfg = make_cfg(1, 2, 0.001);
   agent_config_t acfg = make_acfg();
   delegate_ensemble_result_t result = {0};
   int rc = fixture_ensemble_run(&acfg, &cfg, "expensive question", &result);
   assert(rc == 0);
   assert(result.cost_capped == 1);
   assert(!result.degraded);
   printf("  test_ensemble_cost_cap: ok\n");
   delegate_ensemble_result_free(&result);
}

static void test_ensemble_cost_uses_model_registry_prices(void)
{
   reset_modes();
   ensemble_panel_t cfg = make_cfg(1, 2, 10.0);
   agent_config_t acfg = make_priced_acfg();
   delegate_ensemble_result_t result = {0};
   int rc = fixture_ensemble_run(&acfg, &cfg, "priced question", &result);
   assert(rc == 0);
   /* 3 refs at 50 input + 50 output each, plus aggregator at 200 input + 100 output:
    * (350 * $1/MTok) + (250 * $3/MTok) = $0.0011. The old flat fallback would be
    * 600 * $15/MTok = $0.009, so this catches regressions to global pricing. */
   assert(result.cost_usd > 0.00109 && result.cost_usd < 0.00111);
   printf("  test_ensemble_cost_uses_model_registry_prices: ok\n");
   delegate_ensemble_result_free(&result);
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
   delegate_ensemble_result_t result = {0};
   int rc = fixture_ensemble_run(&acfg, &cfg, "route check", &result);
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
   delegate_ensemble_result_free(&result);
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
   delegate_ensemble_result_t result = {0};
   assert(fixture_ensemble_run(&acfg, &cfg, "exercise every filled panel seat", &result) == 0);
   assert(result.participants_total == 2);
   assert(result.participants_failed == 0);
   printf("  test_implicit_panel_ignores_legacy_roster_and_caps_two: ok\n");
   delegate_ensemble_result_free(&result);
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

static void test_roundtable_captures_original_request_alignment(void)
{
   agent_result_t results[3];
   memset(results, 0, sizeof results);
   snprintf(results[0].agent_name, sizeof results[0].agent_name, "codex");
   assert(!fixture_agent_result_from_text(
       &results[0],
       "{\"original_request_alignment\":{\"status\":\"aligned\",\"summary\":\"implements "
       "the requested scheduler\"},\"items\":[]}"));
   results[0].success = 1;
   snprintf(results[1].agent_name, sizeof results[1].agent_name, "minimax");
   assert(!fixture_agent_result_from_text(
       &results[1],
       "{\"original_request_alignment\":{\"status\":\"drifted\",\"summary\":\"builds an "
       "unrequested dashboard instead\"},\"items\":[]}"));
   results[1].success = 1;
   snprintf(results[2].agent_name, sizeof results[2].agent_name, "kimi");
   assert(!fixture_agent_result_from_text(&results[2], "{\"items\":[]}"));
   results[2].success = 1;
   roundtable_result_t out;
   memset(&out, 0, sizeof out);
   capture_round_review_items(results, 3, &out, 2);
   assert(strcmp(out.original_request_alignment, "drifted") == 0);
   assert(strstr(out.original_request_alignment_summary, "unrequested dashboard") != NULL);
   assert(strcmp(out.original_request_alignment_sources, "minimax") == 0);

   assert(!fixture_agent_result_from_text(
       &results[1], "{\"original_request_alignment\":{\"status\":\"aligned\",\"summary\":\"\"},"
                    "\"items\":[]}"));
   results[1].success = 1;
   memset(&out, 0, sizeof out);
   capture_round_review_items(results, 2, &out, 2);
   assert(strcmp(out.original_request_alignment, "aligned") == 0);
   assert(strstr(out.original_request_alignment_summary, "requested scheduler") != NULL);

   /* A stricter verdict without its required rationale remains stricter and
    * fails closed; it must not inherit an earlier aligned explanation. */
   assert(!fixture_agent_result_from_text(
       &results[1], "{\"original_request_alignment\":{\"status\":\"drifted\",\"summary\":\"\"},"
                    "\"items\":[]}"));
   results[1].success = 1;
   memset(&out, 0, sizeof out);
   capture_round_review_items(results, 2, &out, 2);
   assert(strcmp(out.original_request_alignment, "drifted") == 0);
   assert(strstr(out.original_request_alignment_summary, "no explanation") != NULL);

   memset(&out, 0, sizeof out);
   capture_round_review_items(&results[2], 1, &out, 3);
   assert(strcmp(out.original_request_alignment, "unclear") == 0);
   assert(strstr(out.original_request_alignment_summary, "No panelist") != NULL);

   assert(!fixture_agent_result_from_text(
       &results[0], "{\"items\":[{\"severity\":\"suggestion\",\"category\":\"correctness\","
                    "\"location\":\"src/a.c:1\",\"summary\":\"same grounded fact\"}]}"));
   results[0].success = 1;
   results[0].successful_tool_calls = 0;
   memset(&out, 0, sizeof out);
   capture_round_review_items(results, 1, &out, 4);
   assert(out.item_count == 1);
   assert(out.items[0].tool_grounded == 0);

   agent_result_clear_response(&results[1]);
   assert(!aimee_ir_response_clone(&results[0].canonical_response, &results[1].canonical_response));
   results[1].success = 1;
   results[1].successful_tool_calls = 1;
   memset(&out, 0, sizeof out);
   capture_round_review_items(results, 2, &out, 4);
   assert(out.item_count == 1);
   assert(out.items[0].tool_grounded == 1); /* grounded duplicate upgrades provenance */
   for (int i = 0; i < 3; i++)
      agent_result_clear_response(&results[i]);
   printf("  test_roundtable_captures_original_request_alignment: ok\n");
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

static void test_refused_native_participants_cannot_synthesize_success(void)
{
   reset_modes();
   g_parallel_mode = 12;
   ensemble_panel_t panel = make_cfg(1, 2, 10.0);
   agent_config_t config = make_acfg();
   delegate_ensemble_result_t result = {0};
   int rc = fixture_ensemble_run(&config, &panel, "review", &result);
   assert(rc == 0);
   assert(!result.success && !result.canonical_response && result.participants_failed == 3);
   assert(g_aggregator_calls == 0);
   delegate_ensemble_result_free(&result);
   reset_modes();
   g_parallel_mode = 13; /* complete model bytes, failed capture/execution */
   rc = fixture_ensemble_run(&config, &panel, "review", &result);
   assert(rc == 0 && !result.success && !result.canonical_response);
   assert(g_aggregator_calls == 0 && result.participants_failed == 3);
   delegate_ensemble_result_free(&result);
   reset_modes();
}

static void test_review_items_require_canonical_success(void)
{
   reset_modes();
   agent_result_t source = {0};
   char *text = strdup("{\"original_request_alignment\":{\"status\":\"aligned\",\"summary\":"
                       "\"canonical\"},\"items\":[]}");
   source.success = 1;
   fixture_observation(&source, text);
   roundtable_result_t output = {0};
   capture_round_review_items(&source, 1, &output, 1);
   assert(!strcmp(output.original_request_alignment, "aligned"));
   source.canonical_response.stop_reason = AIMEE_STOP_CONTENT_FILTER;
   capture_round_review_items(&source, 1, &output, 1);
   assert(!strcmp(output.original_request_alignment, "unclear"));
   source.canonical_response.stop_reason = AIMEE_STOP_END_TURN;
   source.success = 0;
   capture_round_review_items(&source, 1, &output, 1);
   assert(!strcmp(output.original_request_alignment, "unclear"));
   agent_result_clear_response(&source);
}

int main(void)
{
   test_refused_native_participants_cannot_synthesize_success();
   test_review_items_require_canonical_success();
   printf("delegate_ensemble tests\n");
   test_one_seat_panel_skips_chair_pass();
   test_ensemble_cost_cap();
   test_ensemble_cost_uses_model_registry_prices();
   test_delegate_cost_estimate_uses_token_tracker();
   test_ensemble_min_successful_degradation();
   test_ensemble_routes_to_distinct_agents();
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
   test_roundtable_captures_original_request_alignment();
   test_parse_lenient_prose_with_code_braces();
   assert(g_peer_blocks_seen > 0);
   free(g_fixture_request_view);
   printf("all tests passed\n");
   return 0;
}
