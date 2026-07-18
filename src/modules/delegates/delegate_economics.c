/* delegate_economics.c: aggregate delegate runs around supervisor attention. */
#include "delegate_economics.h"
#include "cmd_agent_delegate_impl.h"

#include <stdio.h>
#include <string.h>

static const agent_t *economics_find_agent(const agent_config_t *cfg, const char *name)
{
   if (!cfg || !name || !name[0])
      return NULL;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      if (strcmp(cfg->agents[i].name, name) == 0)
         return &cfg->agents[i];
   }
   return NULL;
}

static int economics_result_int(cJSON *root, const char *name, int fallback)
{
   cJSON *v = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, name) : NULL;
   return cJSON_IsNumber(v) ? v->valueint : fallback;
}

static int economics_json_array_count(cJSON *arr)
{
   return cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
}

static int economics_result_agent_tier(cJSON *root, const db1_coord_task_t *task,
                                       const agent_config_t *cfg)
{
   cJSON *tier =
       cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "agent_cost_tier") : NULL;
   if (cJSON_IsNumber(tier))
      return tier->valueint;

   if (task && task->claimed_by[0])
   {
      const agent_t *ag = economics_find_agent(cfg, task->claimed_by);
      if (ag)
         return ag->cost_tier;
   }

   cJSON *agent = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "agent") : NULL;
   if (!cJSON_IsString(agent))
      agent = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "agent_name") : NULL;
   if (cJSON_IsString(agent))
   {
      const agent_t *ag = economics_find_agent(cfg, agent->valuestring);
      if (ag)
         return ag->cost_tier;
   }
   return -1;
}

static int economics_handoff_schema(cJSON *obj)
{
   cJSON *schema =
       cJSON_IsObject(obj) ? cJSON_GetObjectItemCaseSensitive(obj, "schema_version") : NULL;
   return cJSON_IsString(schema) && strcmp(schema->valuestring, "delegate_result_v1") == 0;
}

static void economics_find_handoff_text(cJSON *root, const char *raw, const char **handoff_text,
                                        char **owned_handoff_text)
{
   *handoff_text = NULL;
   *owned_handoff_text = NULL;
   if (economics_handoff_schema(root))
   {
      *handoff_text = raw;
      return;
   }

   cJSON *response =
       cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "response") : NULL;
   if (cJSON_IsString(response) && response->valuestring[0])
   {
      *handoff_text = response->valuestring;
      return;
   }
   if (cJSON_IsObject(response))
      *owned_handoff_text = cJSON_PrintUnformatted(response);
   if (*owned_handoff_text)
      *handoff_text = *owned_handoff_text;
}

static cJSON *economics_handoff_object(cJSON *root, const char *handoff_text, cJSON **owned_handoff)
{
   *owned_handoff = NULL;
   if (economics_handoff_schema(root))
      return root;
   if (!handoff_text || !handoff_text[0])
      return NULL;
   *owned_handoff = cJSON_Parse(handoff_text);
   if (economics_handoff_schema(*owned_handoff))
      return *owned_handoff;
   cJSON_Delete(*owned_handoff);
   *owned_handoff = NULL;
   return NULL;
}

static void economics_add_tier(delegate_economics_report_t *report, int tier)
{
   if (tier >= 0 && tier < DELEGATE_ECONOMICS_TIER_BUCKETS)
      report->tier_counts[tier]++;
   else
      report->unknown_tier_count++;
}

static void economics_finalize(delegate_economics_report_t *report)
{
   int tier0_heavy = delegate_economics_is_tier0_heavy(report);
   int expensive_only = report->delegate_count > 0 && report->tier_counts[0] == 0 &&
                        report->tier_counts[1] == 0 && report->unknown_tier_count == 0;
   int high_manual = report->manual_integration_events > report->delegate_count / 2;
   int low_verification = report->delegate_count > 0 &&
                          report->delegates_with_focused_tests * 2 < report->delegate_count;

   if (report->delegate_count <= 0)
      snprintf(report->verdict, sizeof(report->verdict), "%s", "unclear");
   else if (tier0_heavy && !high_manual && report->invalid_handoffs <= report->delegate_count / 2)
      snprintf(report->verdict, sizeof(report->verdict), "%s", "likely_net_win");
   else if (expensive_only && (report->invalid_handoffs > 0 || high_manual || low_verification))
      snprintf(report->verdict, sizeof(report->verdict), "%s", "likely_net_loss");
   else
      snprintf(report->verdict, sizeof(report->verdict), "%s", "unclear");

   if (tier0_heavy)
   {
      snprintf(report->recommendation, sizeof(report->recommendation),
               "Tier-0-heavy run: broader delegation and redundant validation are reasonable "
               "when task risk warrants it.");
   }
   else if (strcmp(report->verdict, "likely_net_loss") == 0)
   {
      snprintf(report->recommendation, sizeof(report->recommendation),
               "Delegate conservatively: expensive delegates plus supervisor follow-up likely "
               "outweighed the saved attention.");
   }
   else
   {
      snprintf(report->recommendation, sizeof(report->recommendation),
               "Delegate selectively: supervisor cost savings are unclear from this run.");
   }
}

static void economics_add_task(delegate_economics_report_t *report, const db1_coord_task_t *task,
                               const agent_config_t *cfg)
{
   if (!task)
      return;
   int is_delegate = task->claimed_by[0] || strcmp(task->status, "done") == 0 ||
                     strcmp(task->status, "failed") == 0;
   if (!is_delegate)
      return;

   report->delegate_count++;

   cJSON *root = task->result[0] ? cJSON_Parse(task->result) : NULL;
   economics_add_tier(report, economics_result_agent_tier(root, task, cfg));

   int prompt_tokens = economics_result_int(root, "prompt_tokens", 0);
   int completion_tokens = economics_result_int(root, "completion_tokens", 0);
   int delegate_tokens = economics_result_int(root, "delegate_tokens_estimated", 0);
   if (delegate_tokens <= 0)
      delegate_tokens = prompt_tokens + completion_tokens;
   if (delegate_tokens > 0)
   {
      report->prompt_tokens_total += prompt_tokens;
      report->completion_tokens_total += completion_tokens;
      report->delegate_tokens_estimated += delegate_tokens;
      report->tokenized_delegate_results++;
   }

   int manual_marker = 0;
   if (strcmp(task->status, "failed") == 0)
   {
      report->supervisor_actions_required++;
      manual_marker = 1;
   }

   const char *handoff_text = NULL;
   char *owned_handoff_text = NULL;
   economics_find_handoff_text(root, task->result, &handoff_text, &owned_handoff_text);

   if (strcmp(task->status, "done") == 0)
   {
      report->handoff_count++;
      delegate_handoff_validation_t v;
      if (delegate_handoff_validate_text(handoff_text, task->files, 1, &v) == 0)
      {
         report->valid_handoffs++;
         report->focused_tests_run_by_delegates += v.passed_tests;
         if (v.passed_tests > 0)
            report->delegates_with_focused_tests++;
         if (v.outside_ownership_count > 0 || v.needs_supervisor_review)
            manual_marker = 1;
      }
      else
      {
         report->invalid_handoffs++;
         manual_marker = 1;
      }
   }

   cJSON *owned_handoff = NULL;
   cJSON *handoff = economics_handoff_object(root, handoff_text, &owned_handoff);
   cJSON *actions = cJSON_IsObject(handoff)
                        ? cJSON_GetObjectItemCaseSensitive(handoff, "supervisor_actions")
                        : NULL;
   int action_count = economics_json_array_count(actions);
   if (action_count > 0)
   {
      report->supervisor_actions_required += action_count;
      manual_marker = 1;
   }
   report->reviewer_findings_blocking +=
       economics_result_int(root, "reviewer_findings_blocking", 0);

   if (manual_marker)
      report->manual_integration_events++;

   cJSON_Delete(owned_handoff);
   free(owned_handoff_text);
   cJSON_Delete(root);
}

void delegate_economics_build_report(const db1_coord_job_t *job, const db1_coord_task_t *tasks,
                                     int task_count, const agent_config_t *cfg,
                                     delegate_economics_report_t *out)
{
   (void)job;
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   if (tasks && task_count > 0)
   {
      for (int i = 0; i < task_count; i++)
         economics_add_task(out, &tasks[i], cfg);
   }

   out->supervisor_prompt_tokens_estimated =
       out->delegate_count * 300 + out->manual_integration_events * 600 +
       out->invalid_handoffs * 800 + out->reviewer_findings_blocking * 500;
   economics_finalize(out);
}

void delegate_economics_add_json(cJSON *obj, const delegate_economics_report_t *report)
{
   if (!obj || !report)
      return;
   cJSON_AddStringToObject(obj, "delegate_cost_model", DELEGATE_ECONOMICS_COST_MODEL);
   cJSON_AddStringToObject(obj, "delegate_cost_model_label", delegate_economics_cost_model_label());
   cJSON_AddNumberToObject(obj, "delegate_count", report->delegate_count);
   cJSON_AddNumberToObject(obj, "delegate_tier0_count", report->tier_counts[0]);
   cJSON_AddNumberToObject(obj, "delegate_tier1_count", report->tier_counts[1]);
   cJSON_AddNumberToObject(obj, "delegate_tier2_count", report->tier_counts[2]);
   cJSON_AddNumberToObject(obj, "delegate_tier3_count", report->tier_counts[3]);
   cJSON_AddNumberToObject(obj, "delegate_tier_unknown_count", report->unknown_tier_count);
   cJSON_AddNumberToObject(obj, "delegate_prompt_tokens_estimated", report->prompt_tokens_total);
   cJSON_AddNumberToObject(obj, "delegate_completion_tokens_estimated",
                           report->completion_tokens_total);
   cJSON_AddNumberToObject(obj, "delegate_tokens_estimated", report->delegate_tokens_estimated);
   cJSON_AddNumberToObject(obj, "delegate_tokenized_results", report->tokenized_delegate_results);
   cJSON_AddNumberToObject(obj, "supervisor_prompt_tokens_estimated",
                           report->supervisor_prompt_tokens_estimated);
   cJSON_AddNumberToObject(obj, "delegate_handoff_count", report->handoff_count);
   cJSON_AddNumberToObject(obj, "delegate_handoffs_valid", report->valid_handoffs);
   cJSON_AddNumberToObject(obj, "delegate_invalid_handoffs", report->invalid_handoffs);
   cJSON_AddNumberToObject(obj, "delegate_focused_tests_run",
                           report->focused_tests_run_by_delegates);
   cJSON_AddNumberToObject(obj, "delegate_tasks_with_focused_tests",
                           report->delegates_with_focused_tests);
   cJSON_AddNumberToObject(obj, "delegate_manual_integration_events",
                           report->manual_integration_events);
   cJSON_AddNumberToObject(obj, "delegate_supervisor_actions_required",
                           report->supervisor_actions_required);
   cJSON_AddNumberToObject(obj, "delegate_reviewer_blocking_findings",
                           report->reviewer_findings_blocking);
   cJSON_AddStringToObject(obj, "delegate_economics_verdict", report->verdict);
   cJSON_AddStringToObject(obj, "delegate_economics_verdict_label",
                           delegate_economics_verdict_text(report->verdict));
   cJSON_AddStringToObject(obj, "delegate_economics_recommendation", report->recommendation);
}

const char *delegate_economics_cost_model_label(void)
{
   return "free delegates, expensive supervisor";
}

const char *delegate_economics_verdict_text(const char *verdict)
{
   if (verdict && strcmp(verdict, "likely_net_win") == 0)
      return "likely net supervisor-token win";
   if (verdict && strcmp(verdict, "likely_net_loss") == 0)
      return "likely net supervisor-token loss";
   return "unclear supervisor-token outcome";
}

int delegate_economics_is_tier0_heavy(const delegate_economics_report_t *report)
{
   return report && report->delegate_count > 0 &&
          report->tier_counts[0] * 2 >= report->delegate_count;
}

void delegate_economics_add_agent_result_json(cJSON *obj, const agent_config_t *cfg,
                                              const char *role, const agent_result_t *result,
                                              const agent_t *fallback_agent)
{
   if (!obj)
      return;
   const agent_t *agent = NULL;
   if (result && result->agent_name[0])
      agent = economics_find_agent(cfg, result->agent_name);
   if (!agent && fallback_agent)
      agent = fallback_agent;

   if (!cJSON_GetObjectItemCaseSensitive(obj, "agent") && result && result->agent_name[0])
      cJSON_AddStringToObject(obj, "agent", result->agent_name);
   else if (!cJSON_GetObjectItemCaseSensitive(obj, "agent") && agent && agent->name[0])
      cJSON_AddStringToObject(obj, "agent", agent->name);
   if (role && role[0])
      cJSON_AddStringToObject(obj, "delegate_role", role);
   if (agent)
   {
      cJSON_AddNumberToObject(obj, "agent_cost_tier", agent->cost_tier);
      cJSON_AddStringToObject(obj, "delegate_cost_model",
                              agent->cost_tier == 0 ? DELEGATE_ECONOMICS_COST_MODEL
                                                    : "tiered_delegate_cost");
      if (agent->cost_tier == 0)
         cJSON_AddStringToObject(obj, "delegate_cost_model_label",
                                 delegate_economics_cost_model_label());
   }
   if (result)
   {
      int tokens = result->prompt_tokens + result->completion_tokens;
      cJSON_AddNumberToObject(obj, "delegate_tokens_estimated", tokens);
      cJSON_AddNumberToObject(obj, "delegate_cache_read_tokens", result->cache_read_tokens);
      cJSON_AddNumberToObject(obj, "delegate_cache_write_tokens", result->cache_write_tokens);
   }
}
