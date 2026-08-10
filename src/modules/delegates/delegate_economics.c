/* delegate_economics.c: the seam to the delegates module's run economics, plus
 * the JSON rendering of the report it produces.
 *
 * Judging what a coordinated run cost the SUPERVISOR -- which rows count as
 * delegate runs, which tier they sat on, whether a handoff could be believed,
 * and what all of that implies -- is a decision, so it is now
 * server-go/modules/delegates/economics.go. Moving it also removed a bus round
 * trip per task: the handoff rule already lives in that module, so the report
 * builder calls it directly instead of asking back out through C.
 *
 * Fails closed as an EMPTY report with an "unclear" verdict. With no answer the
 * honest statement is that nothing was established, and "unclear" is exactly
 * the verdict the rule itself gives a run it cannot judge. Reporting a win or a
 * loss instead would be an unearned claim about how a team spent its attention.
 */
#include <aimee/delegates/delegate_economics.h>
#include "cmd_agent_delegate_impl.h"

#include <stdio.h>
#include <string.h>

static delegate_economics_provider_fn g_economics_provider;

void delegate_register_economics_provider(delegate_economics_provider_fn provider)
{
   g_economics_provider = provider;
}

void delegate_economics_build_report(const db1_coord_job_t *job, const db1_coord_task_t *tasks,
                                     int task_count, const agent_config_t *cfg,
                                     delegate_economics_report_t *out)
{
   (void)job;
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   snprintf(out->verdict, sizeof(out->verdict), "%s", "unclear");
   snprintf(out->recommendation, sizeof(out->recommendation), "%s",
            "Delegate selectively: supervisor cost savings are unclear from this run.");
   snprintf(out->verdict_label, sizeof(out->verdict_label), "%s",
            "unclear supervisor-token outcome");
   snprintf(out->cost_model_label, sizeof(out->cost_model_label), "%s",
            "free delegates, expensive supervisor");
   if (!g_economics_provider)
      return;
   g_economics_provider(tasks, task_count, cfg, out);
}

void delegate_economics_add_json(cJSON *obj, const delegate_economics_report_t *report)
{
   if (!obj || !report)
      return;
   cJSON_AddStringToObject(obj, "delegate_cost_model", DELEGATE_ECONOMICS_COST_MODEL);
   cJSON_AddStringToObject(obj, "delegate_cost_model_label", report->cost_model_label);
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
   cJSON_AddStringToObject(obj, "delegate_economics_verdict_label", report->verdict_label);
   cJSON_AddStringToObject(obj, "delegate_economics_recommendation", report->recommendation);
}

int delegate_economics_is_tier0_heavy(const delegate_economics_report_t *report)
{
   return report && report->delegate_count > 0 &&
          report->tier_counts[0] * 2 >= report->delegate_count;
}

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
                                 "free delegates, expensive supervisor");
   }
   if (result)
   {
      int tokens = result->prompt_tokens + result->completion_tokens;
      cJSON_AddNumberToObject(obj, "delegate_tokens_estimated", tokens);
      cJSON_AddNumberToObject(obj, "delegate_cache_read_tokens", result->cache_read_tokens);
      cJSON_AddNumberToObject(obj, "delegate_cache_write_tokens", result->cache_write_tokens);
   }
}
