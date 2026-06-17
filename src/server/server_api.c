/* server_api.c: native /v1 REST resource providers for aimee-server.
 *
 * The server_http listener is kept dependency-light: native resources backed
 * by subsystems (kb_client, memory, …) are exposed through JSON-provider seams
 * that this translation unit wires up at startup (server_native_register),
 * keeping those dependency closures out of server_http.c and its unit test.
 *
 * First native resource: GET /v1/rules — the active collaboration rules,
 * proxied from aimee-kb via kb_client. */
#include "server_http.h"
#include "kb_client.h"
#include "config.h"
#include "agent_config.h"
#include "agent_types.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GET /v1/rules provider: the active collab rules as a heap JSON object
 * ({"epoch":N,"rules":[...]}) or NULL when aimee-kb is unreachable. The route
 * frees the returned string. */
static char *rules_provider(void)
{
   return kb_client_collab_rules_list_active_json();
}

/* GET /v1/dashboard/memory provider: memory subsystem stats as a heap JSON
 * object, or NULL when aimee-kb is unreachable. The route frees it. */
static char *dashboard_memory_provider(void)
{
   return kb_client_dashboard_memory_stats_json();
}

/* GET /v1/dashboard/reminders provider: reminders as a heap JSON object. */
static char *dashboard_reminders_provider(void)
{
   return kb_client_dashboard_reminders_json();
}

/* GET /v1/kb/status provider: kb/vector status as a heap JSON object. */
static char *kb_status_provider(void)
{
   return kb_client_status_json();
}

/* GET /v1/kb/curator provider: the curator observability block (§4). */
static char *kb_curator_provider(void)
{
   return kb_client_curator_json();
}

/* GET /v1/roadmap provider: the roadmap list as a heap JSON object. */
static char *roadmap_provider(void)
{
   return kb_client_roadmap_list_json();
}

/* GET /v1/curiosity provider: open curiosity items (all states, capped). */
static char *curiosity_provider(void)
{
   return kb_client_curiosity_list_json(NULL, 50);
}

/* GET /v1/notes provider: notes list (all tags, capped). */
static char *notes_list_provider(void)
{
   return kb_client_note_list_json(NULL, 50);
}

/* GET /v1/agents provider: the configured agents + default, built server-side
 * from agent config (not a kb proxy). Shape:
 *   {"default":"<name>","agents":[{"name","provider","model","enabled","roles":[...]}]}
 * Returns NULL (→ 502) when the agent config can't be loaded. */
static char *agents_provider(void)
{
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
      return NULL;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;
   cJSON_AddStringToObject(root, "default", acfg.default_agent);
   cJSON *arr = cJSON_AddArrayToObject(root, "agents");
   if (!arr)
   {
      cJSON_Delete(root);
      return NULL;
   }
   for (int i = 0; i < acfg.agent_count; i++)
   {
      const agent_t *a = &acfg.agents[i];
      cJSON *o = cJSON_CreateObject();
      if (!o)
         continue;
      cJSON_AddItemToArray(arr, o);
      cJSON_AddStringToObject(o, "name", a->name);
      cJSON_AddStringToObject(o, "provider", a->provider);
      cJSON_AddStringToObject(o, "model", a->model);
      cJSON_AddBoolToObject(o, "enabled", a->enabled);
      cJSON *roles = cJSON_AddArrayToObject(o, "roles");
      for (int r = 0; roles && r < a->role_count; r++)
         cJSON_AddItemToArray(roles, cJSON_CreateString(a->roles[r]));
   }
   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return s;
}

/* POST /v1/kb/search: parse {query, project?, max_results?, format?} and run a
 * knowledge search via aimee-kb. Returns the kb_client JSON envelope verbatim;
 * 400 on a missing query, 502 when aimee-kb is unreachable. */
static int kb_search_handler(const char *body, char *resp, int cap)
{
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   cJSON *jq = req ? cJSON_GetObjectItemCaseSensitive(req, "query") : NULL;
   if (!cJSON_IsString(jq) || !jq->valuestring[0])
   {
      cJSON_Delete(req);
      snprintf(resp, (size_t)cap,
               "{\"error\":{\"message\":\"missing `query`\",\"type\":\"invalid_request_error\"}}");
      return 400;
   }

   const cJSON *jp = cJSON_GetObjectItemCaseSensitive(req, "project");
   const char *project = (cJSON_IsString(jp) && jp->valuestring[0]) ? jp->valuestring : NULL;
   const cJSON *jm = cJSON_GetObjectItemCaseSensitive(req, "max_results");
   int max_results = (cJSON_IsNumber(jm) && jm->valuedouble >= 1.0 && jm->valuedouble <= 100.0)
                         ? (int)jm->valuedouble
                         : 10;
   const cJSON *jf = cJSON_GetObjectItemCaseSensitive(req, "format");
   const char *format =
       (cJSON_IsString(jf) && strcmp(jf->valuestring, "text") == 0) ? "text" : "json";

   /* Let the kb embed the query with ITS OWN configured embedder — the kb owns
    * the corpus and its embedder. Forward the server's embedding_command only
    * when one is explicitly set (co-located deploy, shared config); pass NULL
    * otherwise. NEVER default to "builtin": in a split deploy the server has no
    * embedder, and a 384-dim builtin query vector cannot match a real-embedder
    * corpus (1024/2560-dim) — the dim mismatch yields zero hits even though the
    * corpus is fully embedded. */
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_load(&cfg);
   const char *emb = cfg.embedding_command[0] ? cfg.embedding_command : NULL;

   char *j = kb_client_search_json(project, jq->valuestring, emb, max_results, format);
   cJSON_Delete(req);
   if (!j)
   {
      snprintf(resp, (size_t)cap,
               "{\"error\":{\"message\":\"knowledge backend unavailable\",\"type\":\"upstream_"
               "error\"}}");
      return 502;
   }
   snprintf(resp, (size_t)cap, "%s", j);
   free(j);
   return 200;
}

/* POST /v1/memory/recall: parse {task_hint|query, limit_tokens?, session_start?}
 * and recall relevant memories via aimee-kb. Returns the kb_client JSON
 * envelope verbatim; 400 on a missing hint, 502 when aimee-kb is unreachable. */
static int memory_recall_handler(const char *body, char *resp, int cap)
{
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   /* accept either "task_hint" or "query" as the hint */
   const cJSON *jh = req ? cJSON_GetObjectItemCaseSensitive(req, "task_hint") : NULL;
   if (!cJSON_IsString(jh) || !jh->valuestring[0])
      jh = req ? cJSON_GetObjectItemCaseSensitive(req, "query") : NULL;
   if (!cJSON_IsString(jh) || !jh->valuestring[0])
   {
      cJSON_Delete(req);
      snprintf(resp, (size_t)cap,
               "{\"error\":{\"message\":\"missing `task_hint`\",\"type\":\"invalid_request_"
               "error\"}}");
      return 400;
   }

   const cJSON *jl = cJSON_GetObjectItemCaseSensitive(req, "limit_tokens");
   int limit_tokens = (cJSON_IsNumber(jl) && jl->valuedouble >= 1.0 && jl->valuedouble <= 32768.0)
                          ? (int)jl->valuedouble
                          : 1024;
   const cJSON *js = cJSON_GetObjectItemCaseSensitive(req, "session_start");
   int session_start = cJSON_IsTrue(js) ? 1 : 0;

   /* Graph-code fusion is always on for recall. */
   char *j = kb_client_memory_recall_json_ex(jh->valuestring, limit_tokens, session_start, "on");
   cJSON_Delete(req);
   if (!j)
   {
      snprintf(resp, (size_t)cap,
               "{\"error\":{\"message\":\"memory backend unavailable\",\"type\":\"upstream_"
               "error\"}}");
      return 502;
   }
   snprintf(resp, (size_t)cap, "%s", j);
   free(j);
   return 200;
}

/* POST /v1/notes/search: parse {query, limit?} and search notes via aimee-kb.
 * Returns the kb_client JSON envelope verbatim; 400 on a missing query, 502
 * when aimee-kb is unreachable. */
static int notes_search_handler(const char *body, char *resp, int cap)
{
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   cJSON *jq = req ? cJSON_GetObjectItemCaseSensitive(req, "query") : NULL;
   if (!cJSON_IsString(jq) || !jq->valuestring[0])
   {
      cJSON_Delete(req);
      snprintf(resp, (size_t)cap,
               "{\"error\":{\"message\":\"missing `query`\",\"type\":\"invalid_request_error\"}}");
      return 400;
   }
   const cJSON *jl = cJSON_GetObjectItemCaseSensitive(req, "limit");
   int limit = (cJSON_IsNumber(jl) && jl->valuedouble >= 1.0 && jl->valuedouble <= 100.0)
                   ? (int)jl->valuedouble
                   : 20;

   char *j = kb_client_note_search_json(jq->valuestring, limit);
   cJSON_Delete(req);
   if (!j)
   {
      snprintf(resp, (size_t)cap,
               "{\"error\":{\"message\":\"notes backend unavailable\",\"type\":\"upstream_"
               "error\"}}");
      return 502;
   }
   snprintf(resp, (size_t)cap, "%s", j);
   free(j);
   return 200;
}

void server_native_register(void)
{
   server_http_set_rules_provider(rules_provider);
   server_http_set_dashboard_memory_provider(dashboard_memory_provider);
   server_http_set_dashboard_reminders_provider(dashboard_reminders_provider);
   server_http_set_kb_status_provider(kb_status_provider);
   server_http_set_kb_curator_provider(kb_curator_provider);
   server_http_set_agents_provider(agents_provider);
   server_http_set_roadmap_provider(roadmap_provider);
   server_http_set_curiosity_provider(curiosity_provider);
   server_http_set_notes_list_provider(notes_list_provider);
   server_http_set_kb_search_handler(kb_search_handler);
   server_http_set_memory_recall_handler(memory_recall_handler);
   server_http_set_notes_search_handler(notes_search_handler);
}
