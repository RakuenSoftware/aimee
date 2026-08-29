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
#include "server_http_internal.h" /* request capability context */
#include "server.h"               /* server_active_project_from_cwd */
#include "kb_client.h"
#include "config.h"
#include "working_profile.h" /* working_profile_autoobserve_from_feedback */
#include "agent_config.h"
#include "agent_types.h"
#include <aimee/workspace/workspace.h>
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* The runtime is the thin client's sole discovery endpoint. Fold the KB's
 * independently registered one-surface modules into that projection. */
static char *kb_agent_surfaces_provider(void)
{
   return kb_client_agent_surfaces_json();
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
 * A missing file is a first-run 404, while an existing config that cannot be
 * loaded is a 503. Neither is an upstream gateway failure. */
static char *agents_provider(void)
{
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
   {
      const char *path = agent_config_path();
      errno = 0;
      int missing = path && access(path, F_OK) != 0 && errno == ENOENT;
      cJSON *err = cJSON_CreateObject();
      if (!err)
         return NULL;
      cJSON_AddStringToObject(err, "status", "error");
      cJSON_AddStringToObject(
          err, "message",
          missing ? "no agents are configured yet: choose a provider in the setup wizard"
                  : "agent configuration exists but could not be loaded");
      cJSON_AddStringToObject(err, "kind", missing ? "not_found" : "unavailable");
      cJSON_AddNumberToObject(err, "http_status", missing ? 404 : 503);
      char *s = cJSON_PrintUnformatted(err);
      cJSON_Delete(err);
      return s;
   }
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

/* POST /v1/kb/search: parse {query, project?|cwd?|scope, max_results?, format?} and run a
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
   const cJSON *jc = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   const cJSON *js = cJSON_GetObjectItemCaseSensitive(req, "scope");
   const char *scope = cJSON_IsString(js) ? js->valuestring : "current";
   char resolved_project[MAX_PATH_LEN] = "";
   const char *project = (cJSON_IsString(jp) && jp->valuestring[0]) ? jp->valuestring : NULL;
   int all_projects = strcmp(scope, "all") == 0;
   if ((js && !cJSON_IsString(js)) || (strcmp(scope, "current") != 0 && strcmp(scope, "all") != 0))
   {
      cJSON_Delete(req);
      snprintf(resp, (size_t)cap,
               "{\"error\":{\"message\":\"scope must be current or all\","
               "\"type\":\"invalid_scope\"}}");
      return 400;
   }
   if (all_projects)
   {
      if ((g_rpc_conn_caps & CAP_CROSS_SCOPE_READ) == 0)
      {
         cJSON_Delete(req);
         snprintf(resp, (size_t)cap,
                  "{\"error\":{\"message\":\"scope=all requires operator authority\","
                  "\"type\":\"forbidden\"}}");
         return 403;
      }
      if (!project && cJSON_IsString(jc) && jc->valuestring[0] &&
          server_active_project_from_cwd(jc->valuestring, resolved_project,
                                         sizeof(resolved_project)) == 0)
         project = resolved_project;
   }
   else if (!project)
   {
      if (!cJSON_IsString(jc) || !jc->valuestring[0] ||
          server_active_project_from_cwd(jc->valuestring, resolved_project,
                                         sizeof(resolved_project)) != 0)
      {
         cJSON_Delete(req);
         snprintf(resp, (size_t)cap,
                  "{\"error\":{\"message\":\"no active project; pass project, cwd, or "
                  "scope=all\",\"type\":\"scope_required\"}}");
         return 409;
      }
      project = resolved_project;
   }
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
   const char *emb = config_embedder_command_field()[0] ? config_embedder_command_field() : NULL;

   char *j = kb_client_search_json_scoped_ex(project, all_projects, jq->valuestring, emb,
                                             max_results, format, NULL);
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

   /* Learn how to work with THIS user from their own turns: the UserPromptSubmit
    * hook posts each turn's prompt here as task_hint, and this handler runs in the
    * DB1-owning aimee-server, so it is the right seam to observe interaction
    * preferences (e.g. "be terse", "ask first") into the DB1-local working
    * profile. Personal and strictly local — nothing leaves for the shared KB.
    * Only real user turns (not the session-start context fetch); best-effort, and
    * never affects the recall response. Gated by the working-profile switch. */
   if (!session_start)
   {
      if (config_identity_working_profile_injection_enabled())
         (void)working_profile_autoobserve_from_feedback(jh->valuestring);
   }

   /* Graph-code fusion is always on for recall. */
   char project[MAX_PATH_LEN] = "";
   char workspace[MAX_PATH_LEN] = "";
   const cJSON *jp = cJSON_GetObjectItemCaseSensitive(req, "project");
   const cJSON *jw = cJSON_GetObjectItemCaseSensitive(req, "workspace");
   const cJSON *jc = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   const cJSON *jscope = cJSON_GetObjectItemCaseSensitive(req, "scope");
   int include_all = cJSON_IsString(jscope) && strcmp(jscope->valuestring, "all") == 0;
   if (include_all && (g_rpc_conn_caps & CAP_CROSS_SCOPE_READ) == 0)
   {
      cJSON_Delete(req);
      snprintf(resp, (size_t)cap,
               "{\"error\":{\"message\":\"scope=all requires operator authority\","
               "\"type\":\"forbidden\"}}");
      return 403;
   }
   if (cJSON_IsString(jp))
      snprintf(project, sizeof(project), "%s", jp->valuestring);
   if (cJSON_IsString(jw))
      snprintf(workspace, sizeof(workspace), "%s", jw->valuestring);
   if ((!project[0] || !workspace[0]) && cJSON_IsString(jc) && jc->valuestring[0])
   {
      char resolved_project[MAX_PATH_LEN] = "";
      char resolved_workspace[MAX_PATH_LEN] = "";
      if (workspace_repo_identity(jc->valuestring, resolved_project, sizeof(resolved_project),
                                  resolved_workspace, sizeof(resolved_workspace)) == 0)
      {
         if (!project[0])
            snprintf(project, sizeof(project), "%s", resolved_project);
         if (!workspace[0])
            snprintf(workspace, sizeof(workspace), "%s", resolved_workspace);
      }
   }
   if (!include_all && !project[0] && !workspace[0])
   {
      cJSON_Delete(req);
      snprintf(resp, (size_t)cap,
               "{\"error\":{\"message\":\"an authorized project or workspace is required\","
               "\"type\":\"scope_required\"}}");
      return 409;
   }
   kb_client_memory_scope_context_set(workspace, project, include_all);
   char *j = kb_client_memory_recall_json_ex(jh->valuestring, limit_tokens, session_start, "on");
   kb_client_memory_scope_context_clear();
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
   server_http_set_kb_agent_surfaces_provider(kb_agent_surfaces_provider);
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
   /* Readiness samples db1 + aimee-kb on a background interval and registers
    * its own provider; see server/server_ready.c. */
   server_ready_register();
}
