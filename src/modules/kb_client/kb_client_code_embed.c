/* kb_client_code_embed.c: KB RPC wrappers for Phase 5 code embedding refresh.
 *
 * All calls go through kb_v1_action_request (defined in kb_client.c).
 * Server/CLI must use this module; direct KB_STORE/pgvector access is forbidden. */

#include "kb_client.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* Forward declaration from kb_client.c. */
char *kb_v1_action_request(const char *action, cJSON *req);

/* --- Graph code projection / explain (server -> KB boundary) --- */

int kb_client_graph_sync_code(const char *project, kb_client_graph_sync_result_t *out)
{
   if (!project || !project[0] || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "project", project);
   char *json = kb_v1_action_request("graph.sync_code", req);
   if (!json)
      return -1;

   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (ok)
   {
      out->ok = 1;
      cJSON *g = cJSON_GetObjectItemCaseSensitive(resp, "generation_id");
      cJSON *e = cJSON_GetObjectItemCaseSensitive(resp, "edge_count");
      cJSON *p = cJSON_GetObjectItemCaseSensitive(resp, "project");
      if (cJSON_IsNumber(g))
         out->generation_id = (int64_t)g->valuedouble;
      if (cJSON_IsNumber(e))
         out->edge_count = (int64_t)e->valuedouble;
      if (cJSON_IsString(p))
         snprintf(out->project, sizeof(out->project), "%s", p->valuestring);
   }
   cJSON_Delete(resp);
   return ok ? 0 : -1;
}

char *kb_client_graph_explain_json(const char *entity, int limit)
{
   if (!entity || !entity[0])
      return NULL;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "entity", entity);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return kb_v1_action_request("graph.explain", req);
}
