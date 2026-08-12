/* server_state_index.c: server-side handlers for the code-index families that
 * proxy aimee-kb /v1/code routes. Split out of server_state.c to keep that
 * translation unit under the 2000-line build-integrity limit. */
#include "server.h"
#include "aimee.h"
#include "kb_client.h"
#include "code_span.h"
#include "config_accessors.h"
#include "log.h" /* aimee_log — name the real KB failure in the server log */
#include <aimee/workspace/workspace.h>
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
   {
      /* Same misattribution as the MCP twin: say which dependency failed, and
       * leave a log line so a healthy kb is not the first thing suspected. */
      aimee_log(LOG_WARN, "index.find",
                "index_find_scoped failed: status=%s project=%s all_projects=%d",
                kb_client_result_status_name(kb_client_last_result_status()),
                project[0] ? project : "(none)", all_projects);
      return send_and_free(
          conn, kb_last_result_object("code index lookup failed; see result_status for whether the "
                                      "knowledge service was unreachable, unauthorized, or the "
                                      "scope did not resolve"));
   }

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
      if (!cwd || server_active_project_from_cwd(cwd, project_buf, sizeof(project_buf)) != 0)
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
      if (!cwd || server_active_project_from_cwd(cwd, project_buf, sizeof(project_buf)) != 0)
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

/* An indexed project's root directory. code_span_read needs it and the KB only
 * reports it in the project list, so every surface that reads a span has to look
 * it up this way. 0 on success. */
static int index_project_root(const char *project, char *out, size_t cap)
{
   const int max_projs = 256;
   project_info_t *projs = calloc((size_t)max_projs, sizeof(*projs));
   if (!projs)
      return -1;
   int np = kb_client_index_list(projs, max_projs);
   int found = -1;
   for (int i = 0; i < np; i++)
      if (strcmp(projs[i].name, project) == 0 && projs[i].root[0])
      {
         snprintf(out, cap, "%s", projs[i].root);
         found = 0;
         break;
      }
   free(projs);
   return found;
}

/* Read an exact line range, as a COMMAND rather than only as an MCP tool.
 *
 * Reading a range was reachable only through the MCP tool surface
 * (code_span_get / index command=span), and an MCP call cannot be chained: one
 * call, one turn, the whole conversation re-sent. Measured on the benchmark,
 * reading files is the operation an agent reaches for most, so the most common
 * thing it does was also the thing it could only do one turn at a time. As a
 * command it joins with && into a shell call the agent is already making. */
int handle_index_span(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *file_path;
   if (jo_need_str(req, "file_path", &file_path) < 0 || !file_path[0])
      return server_send_error(conn, "missing file_path", NULL);

   int line_start = jo_int(req, "line_start", 1);
   if (line_start < 1)
      line_start = 1;
   /* Default to the single line, matching the MCP tool: line_end defaults to
    * line_start rather than to end-of-file, so a mistyped range cannot silently
    * pull a whole file into context. */
   int line_end = jo_int(req, "line_end", line_start);
   if (line_end < line_start)
      line_end = line_start;

   char project_buf[MAX_PATH_LEN] = "";
   const char *project = jo_str(req, "project", NULL);
   if (!project || !project[0])
   {
      const char *cwd = jo_str(req, "cwd", NULL);
      if (!cwd || server_active_project_from_cwd(cwd, project_buf, sizeof(project_buf)) != 0)
         return server_send_error(conn, "scope_required: no active project", NULL);
      project = project_buf;
   }

   char root[MAX_PATH_LEN] = "";
   if (index_project_root(project, root, sizeof(root)) != 0)
      return server_send_error(conn, "unknown project (no indexed root)", NULL);

   int max_lines = config_code_span_max_lines() > 0 ? config_code_span_max_lines() : 400;
   cJSON *span = code_span_read(project, root, file_path, line_start, line_end, max_lines);
   if (!span)
      return server_send_error(conn, "out of memory", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "span", span);
   return send_and_free(conn, resp);
}

/* One fused lexical+semantic query against the index. */
static cJSON *hybrid_row(const char *query, const char *symbol, const char *project,
                         int all_projects, int max_results)
{
   cJSON *row = cJSON_CreateObject();
   if (!row)
      return NULL;
   jo_add_str(row, "query", query);
   int st = -1;
   char *j = kb_client_code_hybrid_scoped(query, symbol, project, all_projects, max_results, &st);
   if (j)
   {
      cJSON *parsed = cJSON_Parse(j);
      if (parsed)
         cJSON_AddItemToObject(row, "result", parsed);
      else
         jo_add_str(row, "result_raw", j);
      free(j);
   }
   else
      cJSON_AddNumberToObject(row, "error_status", st);
   return row;
}

/* Search the index for a PHRASE rather than a symbol -- an error string, a
 * config key, a concept -- as a command.
 *
 * The MCP twin additionally does §3 cite-capture, which observes retrieved paths
 * against an MCP session id so a re-cited source earns trust across turns. That
 * is deliberately NOT mirrored here: it is keyed on a session this path does not
 * have, and inventing one would write trust records against an identity that
 * does not exist. The retrieval itself is the same kb_client call, so the
 * ANSWERS match; only the side-effect is absent.
 *
 *   aimee index hybrid "connection pool lease" "retry backoff"
 */
int handle_index_hybrid(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *queries = cJSON_GetObjectItemCaseSensitive(req, "queries");
   int batch = cJSON_IsArray(queries) && cJSON_GetArraySize(queries) > 0;
   const char *single = jo_str(req, "query", NULL);
   if (!batch && (!single || !single[0]))
      return server_send_error(conn, "missing query", NULL);

   const char *symbol = jo_str(req, "symbol", NULL);
   const char *scope = jo_str(req, "scope", NULL);
   if (scope && strcmp(scope, "all") != 0 && strcmp(scope, "current") != 0)
      return server_send_error(conn, "index.hybrid scope must be current or all", NULL);
   int all_projects = (scope && strcmp(scope, "all") == 0) ? 1 : 0;

   char project_buf[MAX_PATH_LEN] = "";
   const char *project = jo_str(req, "project", NULL);
   if (!project || !project[0])
   {
      const char *cwd = jo_str(req, "cwd", NULL);
      if (cwd)
         (void)server_active_project_from_cwd(cwd, project_buf, sizeof(project_buf));
      project = project_buf;
   }
   if (!all_projects && (!project || !project[0]))
      return server_send_error(
          conn, "scope_required: no active project; pass --scope all explicitly", NULL);

   int max_results = jo_int(req, "max_results", 20);
   if (max_results < 1 || max_results > 100)
      max_results = 20;

   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_AddArrayToObject(resp, "results");
   if (batch)
   {
      cJSON *e;
      cJSON_ArrayForEach(e, queries)
      {
         if (!cJSON_IsString(e) || !e->valuestring[0])
            continue; /* skip the malformed entry; the rest of the batch still answers */
         cJSON *row = hybrid_row(e->valuestring, symbol, project, all_projects, max_results);
         if (row)
            cJSON_AddItemToArray(arr, row);
      }
   }
   else
   {
      cJSON *row = hybrid_row(single, symbol, project, all_projects, max_results);
      if (row)
         cJSON_AddItemToArray(arr, row);
   }
   return send_and_free(conn, resp);
}

/* One question, answered from the index with the code attached. */
static cJSON *investigate_one(const char *query, const char *symbol, const char *project)
{
   cJSON *row = cJSON_CreateObject();
   if (!row)
      return NULL;
   jo_add_str(row, "query", query);
   int st = -1;
   char *j = kb_client_code_context(query, symbol, project, &st);
   if (j)
   {
      cJSON *parsed = cJSON_Parse(j);
      if (parsed)
         cJSON_AddItemToObject(row, "result", parsed);
      else
         jo_add_str(row, "result_raw", j);
      free(j);
   }
   else
      cJSON_AddNumberToObject(row, "error_status", st);
   return row;
}

/* Ask the index a plain-words question, as a COMMAND rather than only a tool.
 *
 * This is the call an agent makes FIRST on unfamiliar code -- it returns ranked
 * evidence with the code already attached instead of a list of paths to go read.
 * It was reachable only over MCP, where one call is one turn, so the opening
 * move of every task was also the one that could not be combined with anything
 * else. As a command it chains:
 *
 *   aimee index investigate "how are profiles cached" "what invalidates them"
 *
 * Several questions in ONE invocation, and that invocation folds into a shell
 * call the agent was already making. */
int handle_index_investigate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *queries = cJSON_GetObjectItemCaseSensitive(req, "queries");
   int batch = cJSON_IsArray(queries) && cJSON_GetArraySize(queries) > 0;
   const char *single = jo_str(req, "query", NULL);
   if (!batch && (!single || !single[0]))
      return server_send_error(conn, "missing query", NULL);

   const char *symbol = jo_str(req, "symbol", NULL);
   char project_buf[MAX_PATH_LEN] = "";
   const char *project = jo_str(req, "project", NULL);
   if (!project || !project[0])
   {
      const char *cwd = jo_str(req, "cwd", NULL);
      if (!cwd || server_active_project_from_cwd(cwd, project_buf, sizeof(project_buf)) != 0)
         return server_send_error(conn, "scope_required: no active project", NULL);
      project = project_buf;
   }

   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_AddArrayToObject(resp, "results");
   if (batch)
   {
      cJSON *e;
      cJSON_ArrayForEach(e, queries)
      {
         if (!cJSON_IsString(e) || !e->valuestring[0])
            continue; /* skip the malformed entry; the rest of the batch still answers */
         cJSON *row = investigate_one(e->valuestring, symbol, project);
         if (row)
            cJSON_AddItemToArray(arr, row);
      }
   }
   else
   {
      cJSON *row = investigate_one(single, symbol, project);
      if (row)
         cJSON_AddItemToArray(arr, row);
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
         if (cwd && server_active_project_from_cwd(cwd, project_buf, sizeof(project_buf)) == 0)
            project = project_buf;
      }
   }
   else if (!project || !project[0])
   {
      const char *cwd = jo_str(req, "cwd", NULL);
      if (!cwd || server_active_project_from_cwd(cwd, project_buf, sizeof(project_buf)) != 0)
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
