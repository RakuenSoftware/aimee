/* kb_client_code_embed.c: KB RPC wrappers for Phase 5 code embedding refresh.
 *
 * All calls go through kb_v1_action_request (defined in kb_client.c).
 * Server/CLI must use this module; direct DB2/pgvector access is forbidden. */

#include "kb_client.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* Forward declaration from kb_client.c. */
char *kb_v1_action_request(const char *action, cJSON *req);

static void parse_code_embed_result(const char *json, kb_client_code_embed_result_t *out)
{
   if (!json || !out)
      return;
   cJSON *resp = cJSON_Parse(json);
   if (!resp)
      return;
   cJSON *v;
   v = cJSON_GetObjectItemCaseSensitive(resp, "accepted");
   if (cJSON_IsBool(v))
      out->accepted = cJSON_IsTrue(v) ? 1 : 0;
   v = cJSON_GetObjectItemCaseSensitive(resp, "job_id");
   if (cJSON_IsString(v))
      snprintf(out->job_id, sizeof(out->job_id), "%s", v->valuestring);
   v = cJSON_GetObjectItemCaseSensitive(resp, "project");
   if (cJSON_IsString(v))
      snprintf(out->project, sizeof(out->project), "%s", v->valuestring);
   v = cJSON_GetObjectItemCaseSensitive(resp, "scope");
   if (cJSON_IsString(v))
      snprintf(out->scope, sizeof(out->scope), "%s", v->valuestring);
   v = cJSON_GetObjectItemCaseSensitive(resp, "estimated_points");
   if (cJSON_IsNumber(v))
      out->estimated_points = (int64_t)v->valuedouble;
   v = cJSON_GetObjectItemCaseSensitive(resp, "skipped_unchanged");
   if (cJSON_IsNumber(v))
      out->skipped_unchanged = (int64_t)v->valuedouble;
   v = cJSON_GetObjectItemCaseSensitive(resp, "embedded");
   if (cJSON_IsNumber(v))
      out->embedded = (int64_t)v->valuedouble;
   v = cJSON_GetObjectItemCaseSensitive(resp, "dry_run");
   if (cJSON_IsBool(v))
      out->dry_run = cJSON_IsTrue(v) ? 1 : 0;
   v = cJSON_GetObjectItemCaseSensitive(resp, "writer");
   if (cJSON_IsString(v))
      snprintf(out->writer, sizeof(out->writer), "%s", v->valuestring);
   cJSON_Delete(resp);
}

int kb_client_code_embeddings_refresh(const char *project, const char *scope, const char **paths,
                                      int path_count, int batch_size, int max_points, int dry_run,
                                      kb_client_code_embed_result_t *out)
{
   if (!project || !*project || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "project", project);
   cJSON_AddStringToObject(req, "scope", scope ? scope : "changed_files");
   if (batch_size > 0)
      cJSON_AddNumberToObject(req, "batch_size", batch_size);
   if (max_points > 0)
      cJSON_AddNumberToObject(req, "max_points", max_points);
   if (dry_run)
      cJSON_AddTrueToObject(req, "dry_run");
   if (path_count > 0 && paths)
   {
      cJSON *arr = cJSON_AddArrayToObject(req, "paths");
      for (int i = 0; i < path_count; i++)
         if (paths[i])
            cJSON_AddItemToArray(arr, cJSON_CreateString(paths[i]));
   }

   char *json = kb_v1_action_request("code_embeddings_refresh", req);
   if (!json)
      return -1;
   parse_code_embed_result(json, out);
   free(json);
   return 0;
}

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
