#include "aimee.h"
#include "db1.h"
#include "hud.h"
#include "kb_client.h"
#include "config.h"
#include <inttypes.h>
#include <math.h>
#include <string.h>

int hud_gather(hud_status_t *out)
{
   if (!out)
      return -1;

   memset(out, 0, sizeof(*out));
   snprintf(out->mode, sizeof(out->mode), "%s", "unknown");

   session_state_t state;
   session_state_load(&state, session_id());
   if (state.session_mode[0])
      snprintf(out->mode, sizeof(out->mode), "%s", state.session_mode);

   /* agent_log aggregates (DB1). */
   db1_agent_log_hud_t hud;
   if (db1_agent_log_hud_summary(&hud, 300) == 0)
   {
      out->total_calls = hud.total_calls;
      out->successful_calls = hud.successful_calls;
      out->failed_calls = hud.failed_calls;
      out->total_prompt_tokens = hud.total_prompt_tokens;
      out->total_completion_tokens = hud.total_completion_tokens;
      out->total_turns = hud.total_turns;
      out->total_tool_calls = hud.total_tool_calls;
      out->avg_latency_ms = hud.avg_latency_ms;
      out->recent_calls = hud.recent_calls;
      out->recent_successes = hud.recent_successes;
   }

   db1_token_audit_totals_t totals;
   if (db1_token_audit_totals(0, &totals) == 0)
   {
      out->total_cache_write_tokens = totals.cache_write_tokens;
      out->total_cache_read_tokens = totals.cache_read_tokens;
      out->total_estimated_cost_usd = totals.estimated_cost_usd;
   }

   /* Realized-vs-estimated-vs-avoided-vs-partial spend breakdown (§7). */
   db1_token_audit_spend_t spend;
   if (db1_token_audit_spend_breakdown(0, &spend) == 0)
   {
      out->spend_realized_usd = spend.realized_cost_usd;
      out->spend_estimated_usd = spend.estimated_cost_usd;
      out->spend_avoided_usd = spend.avoided_cost_usd;
      out->spend_partial_usd = spend.partial_cost_usd;
   }

   if (db1_guardrail_event_session_advisory_count(session_id(), &out->semantic_warn_count) != 0)
      out->semantic_warn_count = 0;

   /* KB health — best-effort, 200 ms timeout, never blocks HUD. */
   kb_health_t kbh;
   if (kb_client_health(&kbh) == 0)
   {
      out->kb_health_checked = 1;
      if (!kbh.pgvec_ok || !kbh.pgvec_collection_ok || kbh.freshness_days > 30)
         out->kb_health_fail = 1;
      else if (kbh.freshness_days > 7 ||
               (kbh.chunk_count > 0 && kbh.embedding_count < kbh.chunk_count * 9 / 10))
         out->kb_health_warn = 1;
   }

   return 0;
}

void hud_print(const hud_status_t *s)
{
   if (!s)
      return;

   printf("Session Status\n");
   printf("  Mode:        %s\n", s->mode[0] ? s->mode : "unknown");
   printf("  Delegates:   %d total  (%d ok, %d failed)\n", s->total_calls, s->successful_calls,
          s->failed_calls);
   printf("  Tokens:      %lld prompt / %lld completion", s->total_prompt_tokens,
          s->total_completion_tokens);
   if (s->total_cache_write_tokens > 0 || s->total_cache_read_tokens > 0)
      printf(" | cache: %lldw / %lldr", s->total_cache_write_tokens, s->total_cache_read_tokens);
   if (s->spend_realized_usd > 0.0)
      printf(" | realized ~$%.4f", s->spend_realized_usd);
   else if (s->total_estimated_cost_usd > 0.0)
      printf(" | ~$%.4f", s->total_estimated_cost_usd);
   if (s->spend_avoided_usd > 0.0)
      printf(" (avoided ~$%.4f)", s->spend_avoided_usd);
   printf("\n");
   printf("  Turns:       %d  |  Tool calls: %d\n", s->total_turns, s->total_tool_calls);
   printf("  Avg latency: %.0f ms\n", s->avg_latency_ms);
   printf("  Recent (5m): %d calls  (%d ok)\n", s->recent_calls, s->recent_successes);
   if (s->semantic_warn_count > 0)
      printf("  semantic warnings: %d this session\n", s->semantic_warn_count);
   if (s->ensemble_call_count > 0)
      printf("  Ensemble:    %d calls  ~$%.4f\n", s->ensemble_call_count, s->ensemble_cost_usd);
   if (s->kb_health_checked)
   {
      if (s->kb_health_fail)
         printf("  KB:          [KB:down]\n");
      else if (s->kb_health_warn)
         printf("  KB:          [KB:warn]\n");
   }
}

char *hud_json(const hud_status_t *s)
{
   if (!s)
      return NULL;

   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;

   cJSON_AddStringToObject(obj, "mode", s->mode);
   cJSON_AddNumberToObject(obj, "total_calls", s->total_calls);
   cJSON_AddNumberToObject(obj, "successful_calls", s->successful_calls);
   cJSON_AddNumberToObject(obj, "failed_calls", s->failed_calls);
   cJSON_AddNumberToObject(obj, "total_prompt_tokens", (double)s->total_prompt_tokens);
   cJSON_AddNumberToObject(obj, "total_completion_tokens", (double)s->total_completion_tokens);
   cJSON_AddNumberToObject(obj, "total_cache_write_tokens", (double)s->total_cache_write_tokens);
   cJSON_AddNumberToObject(obj, "total_cache_read_tokens", (double)s->total_cache_read_tokens);
   cJSON_AddNumberToObject(obj, "total_estimated_cost_usd", s->total_estimated_cost_usd);
   /* Spend by usage_kind (§7): realized billable spend reported separately from
    * estimated, avoided, and partial. `spend.total` is realized-only. */
   {
      cJSON *spend = cJSON_AddObjectToObject(obj, "spend");
      cJSON_AddNumberToObject(spend, "realized", s->spend_realized_usd);
      cJSON_AddNumberToObject(spend, "estimated", s->spend_estimated_usd);
      cJSON_AddNumberToObject(spend, "avoided", s->spend_avoided_usd);
      cJSON_AddNumberToObject(spend, "partial", s->spend_partial_usd);
      cJSON_AddNumberToObject(spend, "total", s->spend_realized_usd);
   }
   cJSON_AddNumberToObject(obj, "total_turns", s->total_turns);
   cJSON_AddNumberToObject(obj, "total_tool_calls", s->total_tool_calls);
   cJSON_AddNumberToObject(obj, "avg_latency_ms", s->avg_latency_ms);
   cJSON_AddNumberToObject(obj, "recent_calls", s->recent_calls);
   cJSON_AddNumberToObject(obj, "recent_successes", s->recent_successes);
   cJSON_AddNumberToObject(obj, "semantic_warn_count", s->semantic_warn_count);
   cJSON_AddNumberToObject(obj, "ensemble_call_count", s->ensemble_call_count);
   cJSON_AddNumberToObject(obj, "ensemble_cost_usd", s->ensemble_cost_usd);

   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json;
}
