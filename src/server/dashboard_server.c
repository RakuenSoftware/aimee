/* dashboard_server.c: dashboard JSON helpers that aimee-server's request
 * handlers call directly.
 *
 * api_metrics, api_delegations, and api_vector_status are split out of
 * dashboard.c so the daemon link can drop CMD_OBJS / DB2_OBJS: the rest
 * of dashboard.c (api_memory_stats, api_traces, api_token_audit,
 * api_plans, api_logs, api_dashboard_*, etc.) reads DB2 directly and
 * cannot link into a daemon that doesn't carry libpq. The three helpers
 * here read DB1 (db1_agent_log_*) and route through kb_client for the
 * one pgvector-bound piece (vector status), so they're daemon-safe.
 *
 * Future evolution: when the daemon's dashboard handlers route the
 * full dashboard payload through aimee-kb (so dashboard.c lives entirely
 * on the kb side), this file can be deleted. */
#include "aimee.h"
#include "commands.h"
#include "cJSON.h"
#include "db1_client/db1.h"
#include "kb_client.h"
#include "dashboard.h"
#include "headers/payload_rewrite.h"
#include "memory.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DASHBOARD_MAX_AUDIT_EVENTS 256

#if defined(__GNUC__)
#define SERVER_DASHBOARD_FALLBACK __attribute__((weak))
#else
#define SERVER_DASHBOARD_FALLBACK
#endif

char *api_delegations(void)
{
   db1_agent_log_display_t rows[50];
   int n = db1_agent_log_list_recent(rows, 50);
   if (n < 0)
      return strdup("[]");

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "agent", rows[i].agent_name);
      cJSON_AddStringToObject(obj, "role", rows[i].role);
      cJSON_AddBoolToObject(obj, "success", rows[i].success);
      cJSON_AddNumberToObject(obj, "turns", rows[i].turns);
      cJSON_AddNumberToObject(obj, "tool_calls", rows[i].tool_calls);
      cJSON_AddNumberToObject(obj, "latency_ms", rows[i].latency_ms);
      cJSON_AddNumberToObject(obj, "confidence", rows[i].confidence);
      cJSON_AddStringToObject(obj, "created_at", rows[i].created_at);
      cJSON_AddItemToArray(arr, obj);
   }
   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : strdup("[]");
}

char *api_metrics(void)
{
   db1_agent_log_metric_t rows[64];
   int n = db1_agent_log_metrics_by_role(rows, 64);
   if (n < 0)
      return strdup("[]");

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "role", rows[i].role);
      cJSON_AddNumberToObject(obj, "total", rows[i].total);
      cJSON_AddNumberToObject(obj, "successes", rows[i].successes);
      cJSON_AddNumberToObject(obj, "avg_latency_ms", rows[i].avg_latency_ms);
      cJSON_AddNumberToObject(obj, "tokens", (double)rows[i].tokens);
      cJSON_AddNumberToObject(obj, "cache_write_tokens", (double)rows[i].cache_write_tokens);
      cJSON_AddNumberToObject(obj, "cache_read_tokens", (double)rows[i].cache_read_tokens);
      cJSON_AddNumberToObject(obj, "estimated_cost_usd", rows[i].estimated_cost_usd);
      cJSON_AddItemToArray(arr, obj);
   }
   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : strdup("[]");
}

char *api_vector_status(void)
{
   return kb_client_vector_status_json();
}

SERVER_DASHBOARD_FALLBACK char *api_token_audit(void)
{
   db1_token_audit_dashboard_row_t rows[100];
   int count = db1_token_audit_list_dashboard(rows, 100);
   if (count < 0)
      return strdup("[]");

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "tool_name", rows[i].tool_name);
      cJSON_AddStringToObject(obj, "role", rows[i].role);
      cJSON_AddNumberToObject(obj, "prompt_tokens", (double)rows[i].prompt_tokens);
      cJSON_AddNumberToObject(obj, "completion_tokens", (double)rows[i].completion_tokens);
      cJSON_AddNumberToObject(obj, "cache_write_tokens", (double)rows[i].cache_write_tokens);
      cJSON_AddNumberToObject(obj, "cache_read_tokens", (double)rows[i].cache_read_tokens);
      cJSON_AddNumberToObject(obj, "estimated_cost_usd", rows[i].estimated_cost_usd);
      /* These rows are REALIZED spend (the reader filters out estimated/avoided/
       * partial); declare it so consumers report realized separately (§7). */
      cJSON_AddStringToObject(obj, "usage_kind", "realized");
      cJSON_AddNumberToObject(obj, "call_count", rows[i].call_count);
      cJSON_AddStringToObject(obj, "last_seen", rows[i].last_seen);
      cJSON_AddItemToArray(arr, obj);
   }

   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : strdup("[]");
}

SERVER_DASHBOARD_FALLBACK char *api_traces(void)
{
   db1_execution_trace_tool_call_t rows[100];
   int count = db1_execution_trace_list_tool_calls(rows, 100);
   if (count < 0)
      return strdup("[]");

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "turn", rows[i].turn);
      cJSON_AddStringToObject(obj, "direction", rows[i].direction);
      if (rows[i].tool_name[0])
         cJSON_AddStringToObject(obj, "tool_name", rows[i].tool_name);
      else
         cJSON_AddNullToObject(obj, "tool_name");
      cJSON_AddItemToArray(arr, obj);
   }

   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : strdup("[]");
}

SERVER_DASHBOARD_FALLBACK char *api_plans(void)
{
   db1_execution_plan_summary_t plans[20];
   int count = db1_execution_plan_list_recent_summaries(plans, 20);
   if (count < 0)
      return strdup("[]");

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "id", plans[i].id);
      cJSON_AddStringToObject(obj, "agent", plans[i].agent_name);
      cJSON_AddStringToObject(obj, "task", plans[i].task);
      cJSON_AddStringToObject(obj, "status", plans[i].status);
      cJSON_AddStringToObject(obj, "created_at", plans[i].created_at);
      cJSON_AddNumberToObject(obj, "total_steps", plans[i].total_steps);
      cJSON_AddNumberToObject(obj, "done_steps", plans[i].done_steps);
      cJSON_AddItemToArray(arr, obj);
   }

   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : strdup("[]");
}

SERVER_DASHBOARD_FALLBACK char *api_dashboard_maintenance(void)
{
   cJSON *obj = cJSON_CreateObject();

   memory_maintenance_summary_t last;
   if (memory_maintenance_last_summary(&last) == 0)
      cJSON_AddItemToObject(obj, "last", memory_maintenance_summary_to_json(&last));
   else
      cJSON_AddNullToObject(obj, "last");

   int64_t runs_total = 0, skips_total = 0, changes_total = 0;
   double ms_avg = 0.0, ms_max = 0.0;
   memory_maintenance_metrics(&runs_total, &skips_total, &changes_total, &ms_avg, &ms_max);
   cJSON *metrics = cJSON_AddObjectToObject(obj, "metrics");
   cJSON_AddNumberToObject(metrics, "runs_total", (double)runs_total);
   cJSON_AddNumberToObject(metrics, "skips_total", (double)skips_total);
   cJSON_AddNumberToObject(metrics, "changes_total", (double)changes_total);
   cJSON_AddNumberToObject(metrics, "ms_avg", ms_avg);
   cJSON_AddNumberToObject(metrics, "ms_max", ms_max);

   cJSON *cfg_obj = cJSON_AddObjectToObject(obj, "config");
   cJSON_AddBoolToObject(cfg_obj, "enabled", config_memory_maintenance_enabled() ? 1 : 0);
   int interval = config_memory_maintenance_interval_seconds() > 0
                      ? config_memory_maintenance_interval_seconds()
                      : MEMORY_MAINTENANCE_DEFAULT_INTERVAL_SECS;
   cJSON_AddNumberToObject(cfg_obj, "interval_seconds", interval);
   cJSON_AddBoolToObject(cfg_obj, "summarize_enabled",
                         config_memory_maintenance_summarize_enabled() ? 1 : 0);

   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{}");
}

SERVER_DASHBOARD_FALLBACK char *api_dashboard_identity(void)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return strdup("{}");
   cJSON_AddItemToObject(obj, "charter", identity_charter_json());
   cJSON_AddItemToObject(obj, "local_operator", identity_local_operator_json());
   cJSON_AddItemToObject(obj, "working_profile", identity_working_profile_json());
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{}");
}

SERVER_DASHBOARD_FALLBACK char *api_dashboard_onboard(void)
{
   return strdup("{\"error\":\"onboard report is not available from the server-hosted webchat\"}");
}

SERVER_DASHBOARD_FALLBACK char *api_doctor(void)
{
   return strdup("{\"error\":\"doctor report is not available from the server-hosted webchat\"}");
}

char *api_payload_rewrite_status(void)
{
   cJSON *obj = tool_payload_rewrite_status(NULL);
   if (!obj)
      return strdup("{}");
   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{}");
}
