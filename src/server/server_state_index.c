/* server_state_index.c: server-side handlers for the code-index families that
 * proxy aimee-kb /v1/code routes. Split out of server_state.c to keep that
 * translation unit under the 2000-line build-integrity limit. */
#include "server.h"
#include "aimee.h"
#include "kb_client.h"
#include "workspace.h"
#include "cJSON.h"
#include "json_fluent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Local mirror of the send_and_free helper each server TU defines (see
 * server_state.c / server_memory_benchmark.c) — wraps the ok-envelope sender. */
static int send_and_free(server_conn_t *conn, cJSON *resp)
{
   return server_send_ok(conn, resp);
}

static cJSON *kb_last_result_object(const char *message)
{
   char *json = kb_client_last_result_json(message);
   cJSON *result = json ? cJSON_Parse(json) : NULL;
   free(json);
   return result ? result : jo_err(message);
}

int handle_index_find(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *identifier;
   if (jo_need_str(req, "identifier", &identifier) < 0 || !identifier[0])
      return server_send_error(conn, "missing identifier", NULL);

   char project[MAX_PATH_LEN] = "";
   int all_projects = 0;
   const char *project_arg = jo_str(req, "project", NULL);
   const char *scope = jo_str(req, "scope", NULL);
   if (scope && strcmp(scope, "all") != 0 && strcmp(scope, "current") != 0)
      return server_send_error(conn, "index.find scope must be current or all", NULL);
   if (scope && strcmp(scope, "all") == 0)
   {
      all_projects = 1;
      if (project_arg && project_arg[0])
         snprintf(project, sizeof(project), "%s", project_arg);
      else
      {
         const char *cwd = jo_str(req, "cwd", NULL);
         if (cwd)
            (void)server_active_project_from_cwd(cwd, project, sizeof(project));
      }
   }
   else if (project_arg && project_arg[0])
      snprintf(project, sizeof(project), "%s", project_arg);
   else
   {
      const char *cwd = jo_str(req, "cwd", NULL);
      if (!cwd || server_active_project_from_cwd(cwd, project, sizeof(project)) != 0)
         return server_send_error(
             conn, "scope_required: no active project; pass --scope all explicitly", NULL);
   }

   term_hit_t hits[128];
   int count = kb_client_index_find_scoped(project, all_projects, identifier, hits, 128);
   if (count < 0)
      return send_and_free(conn,
                           kb_last_result_object("knowledge service symbol index unavailable"));

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "result_status", count > 0 ? "ok" : "empty");
   cJSON *arr = cJSON_AddArrayToObject(resp, "hits");
   for (int i = 0; i < count; i++)
   {
      cJSON *h = cJSON_CreateObject();
      jo_add_str(h, "project", hits[i].project);
      jo_add_str(h, "file_path", hits[i].file_path);
      cJSON_AddNumberToObject(h, "line", hits[i].line);
      jo_add_str(h, "kind", hits[i].kind);
      cJSON_AddItemToArray(arr, h);
   }
   return send_and_free(conn, resp);
}

int handle_index_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   project_info_t projects[128];
   int count = kb_client_index_list(projects, 128);
   if (count < 0)
      return send_and_free(conn,
                           kb_last_result_object("knowledge service project index unavailable"));

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "result_status", count > 0 ? "ok" : "empty");
   cJSON *arr = cJSON_AddArrayToObject(resp, "projects");
   for (int i = 0; i < count; i++)
   {
      cJSON *p = cJSON_CreateObject();
      jo_add_str(p, "name", projects[i].name);
      jo_add_str(p, "root", projects[i].root);
      jo_add_str(p, "scanned_at", projects[i].scanned_at);
      cJSON_AddItemToArray(arr, p);
   }
   return send_and_free(conn, resp);
}

int handle_index_blast_radius(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *file_path;
   if (jo_need_str(req, "file_path", &file_path) < 0)
      return server_send_error(conn, "missing file_path", NULL);
   char project_buf[MAX_PATH_LEN] = "";
   const char *project = jo_str(req, "project", NULL);
   if (!project || !project[0])
   {
      const char *cwd = jo_str(req, "cwd", NULL);
      if (!cwd || workspace_repo_identity(cwd, project_buf, sizeof(project_buf), NULL, 0) != 0)
         return server_send_error(conn, "scope_required: no active project", NULL);
      project = project_buf;
   }

   blast_radius_t br;
   int rc = kb_client_index_blast_radius(project, file_path, &br);

   cJSON *resp;
   if (rc == 0)
   {
      resp = jo_ok();
      cJSON_AddStringToObject(resp, "result_status", "ok");
      jo_add_str(resp, "file", br.file);

      cJSON *deps = cJSON_CreateArray();
      for (int i = 0; i < br.dependency_count; i++)
         cJSON_AddItemToArray(deps, cJSON_CreateString(br.dependencies[i]));
      cJSON_AddItemToObject(resp, "dependencies", deps);

      cJSON *depts = cJSON_CreateArray();
      for (int i = 0; i < br.dependent_count; i++)
         cJSON_AddItemToArray(depts, cJSON_CreateString(br.dependents[i]));
      cJSON_AddItemToObject(resp, "dependents", depts);
   }
   else
      resp = kb_last_result_object("blast radius lookup failed");
   return send_and_free(conn, resp);
}

int handle_index_structure(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *file_path;
   if (jo_need_str(req, "file_path", &file_path) < 0)
      return server_send_error(conn, "missing file_path", NULL);
   char project_buf[MAX_PATH_LEN] = "";
   const char *project = jo_str(req, "project", NULL);
   if (!project || !project[0])
   {
      const char *cwd = jo_str(req, "cwd", NULL);
      if (!cwd || workspace_repo_identity(cwd, project_buf, sizeof(project_buf), NULL, 0) != 0)
         return server_send_error(conn, "scope_required: no active project", NULL);
      project = project_buf;
   }
   definition_t defs[256];
   int count = kb_client_index_structure(project, file_path, defs, 256);
   if (count < 0)
      return send_and_free(conn,
                           kb_last_result_object("knowledge service structure index unavailable"));
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "result_status", count > 0 ? "ok" : "empty");
   cJSON *arr = cJSON_AddArrayToObject(resp, "definitions");
   for (int i = 0; i < count; i++)
   {
      cJSON *d = cJSON_CreateObject();
      jo_add_str(d, "name", defs[i].name);
      jo_add_str(d, "kind", defs[i].kind);
      cJSON_AddNumberToObject(d, "line", defs[i].line);
      /* Span-propagation parity (#33): emit line_end like the KB/MCP routes do. */
      if (defs[i].line_end)
         cJSON_AddNumberToObject(d, "line_end", defs[i].line_end);
      cJSON_AddItemToArray(arr, d);
   }
   return send_and_free(conn, resp);
}

int handle_index_find_callers(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *symbol;
   if (jo_need_str(req, "symbol", &symbol) < 0 || !symbol[0])
      return server_send_error(conn, "missing symbol", NULL);
   char project_buf[MAX_PATH_LEN] = "";
   const char *project = jo_str(req, "project", NULL);
   const char *scope = jo_str(req, "scope", NULL);
   if (scope && strcmp(scope, "all") != 0 && strcmp(scope, "current") != 0)
      return server_send_error(conn, "index.callers scope must be current or all", NULL);
   if (scope && strcmp(scope, "all") == 0)
   {
      if (!project || !project[0])
      {
         const char *cwd = jo_str(req, "cwd", NULL);
         if (cwd && workspace_repo_identity(cwd, project_buf, sizeof(project_buf), NULL, 0) == 0)
            project = project_buf;
      }
   }
   else if (!project || !project[0])
   {
      const char *cwd = jo_str(req, "cwd", NULL);
      if (!cwd || workspace_repo_identity(cwd, project_buf, sizeof(project_buf), NULL, 0) != 0)
         return server_send_error(
             conn, "scope_required: no active project; pass --scope all explicitly", NULL);
      project = project_buf;
   }

   caller_hit_t hits[128];
   int count = kb_client_index_find_callers_scoped(project, scope && strcmp(scope, "all") == 0,
                                                   symbol, hits, 128);
   if (count < 0)
      return send_and_free(conn,
                           kb_last_result_object("knowledge service caller index unavailable"));

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "result_status", count > 0 ? "ok" : "empty");
   cJSON *arr = cJSON_AddArrayToObject(resp, "hits");
   for (int i = 0; i < count; i++)
   {
      cJSON *h = cJSON_CreateObject();
      jo_add_str(h, "project", hits[i].project);
      jo_add_str(h, "file_path", hits[i].file_path);
      jo_add_str(h, "caller", hits[i].caller);
      cJSON_AddNumberToObject(h, "line", hits[i].line);
      cJSON_AddItemToArray(arr, h);
   }
   return send_and_free(conn, resp);
}

/* S6: index.deps — proxy the kb cross-repo dependency query. The kb response is
 * rich (per-edge evidence + version stamp, or the AMBIGUOUS review queue when
 * status=ambiguous) so we forward it verbatim, same passthrough idiom as
 * handle_graph_explain. Inputs are validated at this public boundary so a direct
 * API client gets an actionable error rather than an opaque 502: direction must be
 * out (default), in (dependents, --reverse), or both. Validation runs before any
 * allocation, so the error paths cannot leak. */
int handle_index_deps(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *project;
   if (jo_need_str(req, "project", &project) < 0 || !project[0])
      return server_send_error(conn, "missing project", NULL);

   const char *direction = jo_str(req, "direction", NULL);
   if (direction && strcmp(direction, "out") != 0 && strcmp(direction, "in") != 0 &&
       strcmp(direction, "both") != 0)
      return server_send_error(conn, "direction must be 'out', 'in', or 'both'", NULL);

   const char *status = jo_str(req, "status", NULL);
   if (status && strcmp(status, "ambiguous") != 0)
      return server_send_error(conn, "status must be 'ambiguous' or omitted", NULL);
   int status_ambiguous = (status != NULL);

   const char *min_tier = jo_str(req, "min_tier", NULL);
   if (min_tier && strcmp(min_tier, "high") != 0 && strcmp(min_tier, "medium") != 0 &&
       strcmp(min_tier, "tentative") != 0)
      return server_send_error(conn, "min_tier must be high, medium, or tentative", NULL);

   cJSON *dr = cJSON_GetObjectItemCaseSensitive(req, "dry_run");
   int dry_run = cJSON_IsTrue(dr);

   char *json = kb_client_index_cross_repo_deps_json(project, direction, min_tier, status_ambiguous,
                                                     dry_run);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service did not return cross-repo deps", NULL);
   return send_and_free(conn, resp);
}

/* S7: repo.trust — proxy the cross-repo trust write to the kb. The server op is
 * gated by CAP_INDEX_ADMIN (method_registry), which over TCP is local/owner-only;
 * the kb re-checks the owner credential. Inputs are validated here so a bad value
 * never reaches the kb, and the kb HTTP status is mapped to a precise client error
 * (404 no-such-project, 403 forbidden) instead of a blanket 502. `actor` is
 * caller-asserted (single-tenant P1); a server-derived principal is a future
 * hardening. */
int handle_repo_trust(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *project;
   if (jo_need_str(req, "project", &project) < 0 || !project[0])
      return server_send_error(conn, "missing project", NULL);
   const char *trust;
   if (jo_need_str(req, "trust", &trust) < 0 ||
       (strcmp(trust, "trusted") != 0 && strcmp(trust, "untrusted") != 0))
      return server_send_error(conn, "trust must be 'trusted' or 'untrusted'", NULL);
   const char *actor = jo_str(req, "actor", NULL);
   const char *request_id = jo_str(req, "request_id", NULL);

   int status = -1;
   char *json = kb_client_repo_trust_json(project, trust, actor, request_id, &status);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (resp)
      return send_and_free(conn, resp);
   if (status == 404)
      return server_send_error(conn, "no such project", NULL);
   if (status == 403)
      return server_send_error(conn, "forbidden: repo trust requires the owner credential", NULL);
   if (status == 400)
      return server_send_error(conn, "invalid trust request", NULL);
   return server_send_error(conn, "knowledge service did not apply the trust change", NULL);
}
