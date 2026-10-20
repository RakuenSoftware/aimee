#include "agent_result_cache.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "wire_fence.h"
#include "db1_client/db1.h"
#include <aimee/ir/session_event.h>
#include <stdio.h>

/* Optional result reuse is deliberately narrower than native prompt caching.
 * Opaque credentials/fallbacks need their own lineage proof; never guess one.
 * A captured/user turn always executes through its history owner. */
int agent_result_cache_open(const char *role, const agent_t *agent, const char *body,
                            const char *auth, const char *extra, char key[65])
{
   const char *principal = agent_get_request_vault_principal();
   if (!key)
      return 0;
   key[0] = 0;
   if (!agent || !body || !role || !principal || !principal[0] || agent_get_request_session()[0] ||
       aimee_ir_session_get_sink(NULL) || (auth && auth[0]) || (extra && extra[0]) ||
       agent->fallback_model[0])
      return 0;
   return agent_result_cache_key(principal, role, agent, body, key) ? 0 : 1;
}
int agent_result_cache_read(const char *role, const char *key, agent_result_t *out)
{
   if (!out || !role || !role[0] || !key || !key[0] || wire_fence_revalidate_sources())
      return 0;
   char *wire = db1_agent_cache_get(role, key);
   cJSON *json = wire ? cJSON_ParseWithOpts(wire, NULL, 1) : NULL;
   free(wire);
   aimee_response_t response = {0};
   int valid = json && !aimee_ir_response_parse(json, &response) && response.n_content &&
               !response.error_message && response.stop_reason != AIMEE_STOP_ERROR &&
               response.stop_reason != AIMEE_STOP_TOOL_USE &&
               !aimee_ir_response_has_tool_use(&response);
   cJSON_Delete(json);
   if (!valid || wire_fence_revalidate_sources() || agent_result_adopt_response(out, &response))
   {
      aimee_response_free(&response);
      return 0;
   }
   out->success = 1;
   agent_reject_degenerate_plain_response(out, out->agent_name);
   if (!out->success)
   {
      out->error[0] = 0;
      out->abstained = 0;
      out->abstain_reason[0] = 0;
      return 0;
   }
   out->cache_hit = 1;
   if (out->canonical_response.model)
      snprintf(out->model, sizeof out->model, "%s", out->canonical_response.model);
   snprintf(out->stop_reason, sizeof out->stop_reason, "%s",
            out->canonical_response.raw_stop_reason
                ? out->canonical_response.raw_stop_reason
                : aimee_stop_reason_name(out->canonical_response.stop_reason));
   return 1;
}
void agent_result_cache_write(const char *role, const char *key, const agent_result_t *out)
{
   if (!role || !role[0] || !key || !key[0] || !out || !out->success || out->cache_hit ||
       wire_fence_revalidate_sources() || out->canonical_response.error_message ||
       out->canonical_response.stop_reason == AIMEE_STOP_ERROR ||
       out->canonical_response.stop_reason == AIMEE_STOP_TOOL_USE ||
       aimee_ir_response_has_tool_use(&out->canonical_response))
      return;
   cJSON *json = aimee_ir_response_json(&out->canonical_response);
   char *wire = json ? cJSON_PrintUnformatted(json) : NULL;
   cJSON_Delete(json);
   /* The cache is optional; an oversized projection is never a truncated hit. */
   if (wire && strlen(wire) < 256u * 1024u)
      db1_agent_cache_put(role, key, wire);
   free(wire);
}
