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

/* Did the index actually answer, or did it hand back a shaped nothing? */
static int investigate_result_answerable(const cJSON *result)
{
   if (!cJSON_IsObject(result))
      return 0;
   const cJSON *status = cJSON_GetObjectItemCaseSensitive(result, "status");
   const char *status_name = cJSON_IsString(status) ? status->valuestring : NULL;
   if (status_name &&
       (strcmp(status_name, "abstained") == 0 || strcmp(status_name, "no_answer") == 0 ||
        strcmp(status_name, "empty") == 0 || strcmp(status_name, "error") == 0))
      return 0;
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "no_answer")))
      return 0;
   const cJSON *answerability = cJSON_GetObjectItemCaseSensitive(result, "answerability");
   const cJSON *decision = cJSON_IsObject(answerability)
                               ? cJSON_GetObjectItemCaseSensitive(answerability, "decision")
                               : NULL;
   if (cJSON_IsString(decision) && (strcmp(decision->valuestring, "no_answer") == 0 ||
                                    strcmp(decision->valuestring, "abstained") == 0))
      return 0;
   const cJSON *results = cJSON_GetObjectItemCaseSensitive(result, "results");
   return !cJSON_IsArray(results) || cJSON_GetArraySize(results) != 0;
}

static void investigate_collect_paths(const cJSON *node, char paths[][MAX_PATH_LEN], int *count,
                                      int max)
{
   if (!node || !count || *count >= max)
      return;
   if (cJSON_IsObject(node))
   {
      const cJSON *fp = cJSON_GetObjectItemCaseSensitive(node, "file_path");
      if (!cJSON_IsString(fp))
         fp = cJSON_GetObjectItemCaseSensitive(node, "path");
      if (cJSON_IsString(fp) && fp->valuestring[0])
      {
         int duplicate = 0;
         for (int i = 0; i < *count; i++)
            if (strcmp(paths[i], fp->valuestring) == 0)
               duplicate = 1;
         if (!duplicate)
            snprintf(paths[(*count)++], MAX_PATH_LEN, "%s", fp->valuestring);
      }
   }
   const cJSON *child = NULL;
   cJSON_ArrayForEach(child, node) investigate_collect_paths(child, paths, count, max);
}

static int investigate_path_is_test(const char *path)
{
   return path && (strstr(path, "/test") || strstr(path, "tests/") || strstr(path, "_test.") ||
                   strstr(path, ".spec.") || strstr(path, ".test."));
}

static int investigate_path_is_boundary(const char *path)
{
   return path && (strstr(path, "auth") || strstr(path, "guard") || strstr(path, "policy") ||
                   strstr(path, "valid") || strstr(path, "security") || strstr(path, "schema"));
}

static void investigate_attach_code(cJSON *result, const char *project)
{
   cJSON *results = result ? cJSON_GetObjectItemCaseSensitive(result, "results") : NULL;
   if (!cJSON_IsArray(results))
      return;
   char root[MAX_PATH_LEN] = "";
   if (index_project_root(project, root, sizeof(root)) != 0)
      return;
   enum
   {
      MAX_ITEMS = 4,
      WINDOW = 60
   };
   int attached = 0;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, results)
   {
      if (attached >= MAX_ITEMS)
         break;
      cJSON *fp = cJSON_GetObjectItemCaseSensitive(item, "file_path");
      cJSON *span = cJSON_GetObjectItemCaseSensitive(item, "span");
      if (!cJSON_IsString(fp) || !cJSON_IsObject(span))
         continue;
      cJSON *line = cJSON_GetObjectItemCaseSensitive(span, "line_start");
      int anchor = cJSON_IsNumber(line) ? line->valueint : 1;
      int from = anchor > WINDOW / 2 ? anchor - WINDOW / 2 : 1;
      cJSON *read = code_span_read(project, root, fp->valuestring, from, from + WINDOW - 1, WINDOW);
      if (!read)
         continue;
      cJSON *content = cJSON_DetachItemFromObjectCaseSensitive(read, "content");
      if (content)
      {
         cJSON_AddItemToObject(item, "code", content);
         cJSON_AddNumberToObject(item, "code_line_start", from);
         cJSON_AddNumberToObject(item, "code_line_end", from + WINDOW - 1);
         attached++;
      }
      cJSON_Delete(read);
   }
}

static void investigate_attach_scope(cJSON *row, const cJSON *result, const char *symbol,
                                     const char *project)
{
   char paths[12][MAX_PATH_LEN] = {{0}};
   int path_count = 0;
   investigate_collect_paths(result, paths, &path_count, 12);

   cJSON *scope = cJSON_AddObjectToObject(row, "systemic_scope");
   cJSON *definitions = cJSON_AddArrayToObject(scope, "symbol_definitions");
   if (symbol && symbol[0])
   {
      term_hit_t hits[32];
      int count = kb_client_index_find_project(project, symbol, hits, 32);
      for (int i = 0; i < count; i++)
      {
         cJSON *hit = cJSON_CreateObject();
         cJSON_AddStringToObject(hit, "file_path", hits[i].file_path);
         cJSON_AddNumberToObject(hit, "line", hits[i].line);
         cJSON_AddStringToObject(hit, "kind", hits[i].kind);
         cJSON_AddItemToArray(definitions, hit);
         int duplicate = 0;
         for (int j = 0; j < path_count; j++)
            if (strcmp(paths[j], hits[i].file_path) == 0)
               duplicate = 1;
         if (!duplicate && path_count < 12)
            snprintf(paths[path_count++], MAX_PATH_LEN, "%s", hits[i].file_path);
      }
   }
   cJSON *locations = cJSON_AddArrayToObject(scope, "requested_locations");
   cJSON *analogues = cJSON_AddArrayToObject(scope, "analogous_implementations");
   cJSON *boundaries = cJSON_AddArrayToObject(scope, "shared_boundary_candidates");
   cJSON *same_fix = cJSON_AddArrayToObject(scope, "likely_same_fix");
   cJSON *tests = cJSON_AddArrayToObject(scope, "suggested_test_surface");
   for (int i = 0; i < path_count; i++)
   {
      cJSON_AddItemToArray(locations, cJSON_CreateString(paths[i]));
      if (i > 0)
      {
         cJSON_AddItemToArray(analogues, cJSON_CreateString(paths[i]));
         if (!investigate_path_is_test(paths[i]))
            cJSON_AddItemToArray(same_fix, cJSON_CreateString(paths[i]));
      }
      if (investigate_path_is_boundary(paths[i]))
         cJSON_AddItemToArray(boundaries, cJSON_CreateString(paths[i]));
      if (investigate_path_is_test(paths[i]))
         cJSON_AddItemToArray(tests, cJSON_CreateString(paths[i]));
   }

   cJSON *impact = cJSON_AddArrayToObject(scope, "blast_radius");
   for (int i = 0; i < path_count && i < 4; i++)
   {
      blast_radius_t br;
      if (kb_client_index_blast_radius(project, paths[i], &br) != 0)
         continue;
      cJSON *entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "file_path", paths[i]);
      cJSON *dependencies = cJSON_AddArrayToObject(entry, "dependencies");
      cJSON *dependents = cJSON_AddArrayToObject(entry, "dependents");
      for (int j = 0; j < br.dependency_count; j++)
         cJSON_AddItemToArray(dependencies, cJSON_CreateString(br.dependencies[j]));
      for (int j = 0; j < br.dependent_count; j++)
         cJSON_AddItemToArray(dependents, cJSON_CreateString(br.dependents[j]));
      cJSON_AddItemToArray(impact, entry);
   }

   cJSON *callers = cJSON_AddArrayToObject(scope, "direct_callers");
   if (symbol && symbol[0])
   {
      caller_hit_t hits[32];
      int count = kb_client_index_find_callers(project, symbol, hits, 32);
      for (int i = 0; i < count; i++)
      {
         cJSON *hit = cJSON_CreateObject();
         cJSON_AddStringToObject(hit, "file_path", hits[i].file_path);
         cJSON_AddStringToObject(hit, "caller", hits[i].caller);
         cJSON_AddNumberToObject(hit, "line", hits[i].line);
         cJSON_AddItemToArray(callers, hit);
      }
   }
}

cJSON *server_index_investigate_packet(const char *query, const char *symbol, const char *project,
                                       int include_code, int fallback_enabled)
{
   cJSON *row = cJSON_CreateObject();
   if (!row)
      return NULL;
   jo_add_str(row, "query", query);
   cJSON *path = cJSON_AddArrayToObject(row, "retrieval_path");
   cJSON_AddItemToArray(path, cJSON_CreateString("context"));

   int status = -1;
   char *json = kb_client_code_context(query, symbol, project, &status);
   kb_client_result_status_t kb_status = kb_client_last_result_status();
   cJSON *result = json ? cJSON_Parse(json) : NULL;
   free(json);
   int answerable = investigate_result_answerable(result);
   if (!answerable && fallback_enabled)
   {
      cJSON_Delete(result);
      result = NULL;
      cJSON_AddItemToArray(path, cJSON_CreateString("hybrid"));
      json = kb_client_code_hybrid_scoped(query, symbol, project, 0, 12, &status);
      kb_status = kb_client_last_result_status();
      result = json ? cJSON_Parse(json) : NULL;
      free(json);
      answerable = investigate_result_answerable(result);
      cJSON_AddTrueToObject(row, "fallback_used");
   }
   else
      cJSON_AddFalseToObject(row, "fallback_used");

   /* Say WHY the packet carries nothing. An unreachable knowledge service and an
    * index that simply has no evidence both arrive here as an empty row, and the
    * only thing telling them apart was a bare `error_status` number the caller
    * had to guess at -- so `index investigate`, the call the ingress guidance
    * tells every model to make FIRST, reported an outage as "no evidence" and
    * sent the model off to search the tree by hand. Every sibling handler in
    * this file already publishes the typed kb verdict as result_status;
    * investigate was the one that did not. */
   kb_client_result_status_t row_status = answerable ? KB_CLIENT_RESULT_OK
                                          : kb_status == KB_CLIENT_RESULT_OK
                                              ? KB_CLIENT_RESULT_EMPTY
                                              : kb_status;
   cJSON_AddStringToObject(row, "result_status", kb_client_result_status_name(row_status));

   if (result)
   {
      if (include_code)
         investigate_attach_code(result, project);
      cJSON_AddItemToObject(row, "result", result);
      investigate_attach_scope(row, result, symbol, project);
   }
   else
      cJSON_AddNumberToObject(row, "error_status", status);
   return row;
}

/* The typed verdict the packet stamped on the row. */
static const char *investigate_row_status(const cJSON *row)
{
   const cJSON *s = cJSON_GetObjectItemCaseSensitive(row, "result_status");
   return cJSON_IsString(s) ? s->valuestring : "";
}

/* Only the two verdicts that mean the knowledge service never answered. A
 * stale or abstained result IS an answer and must stay distinct from an
 * outage -- folding those in here would restore the confusion in the other
 * direction. */
static int investigate_status_is_outage(const char *name)
{
   return strcmp(name, "unavailable") == 0 || strcmp(name, "unauthorized") == 0;
}

/* Decide what the whole investigation amounts to.
 *
 * Not one query reaching the knowledge service is an OUTAGE, and saying so is
 * the whole point: answering ok or empty there is what made a dead kb look
 * like a healthy index with nothing to say, and this is the call the ingress
 * guidance tells every model to make FIRST. Refuse the way the sibling index
 * handlers refuse, so one caller-side check covers the family. */
static int investigate_finish(server_conn_t *conn, cJSON *resp, const char *project, int rows,
                              int answered, int outages)
{
   if (rows != 0 && answered == 0 && outages == rows)
   {
      aimee_log(LOG_WARN, "index.investigate",
                "no query reached the knowledge service: status=%s project=%s queries=%d",
                kb_client_result_status_name(kb_client_last_result_status()),
                project && project[0] ? project : "(none)", rows);
      cJSON_Delete(resp);
      return send_and_free(conn,
                           kb_last_result_object("knowledge service unavailable; the code index "
                                                 "was never reached, so this is an outage and "
                                                 "not an index with no evidence"));
   }
   /* Mirrors index.find and index.callers: ok when something answered, empty
    * when the index answered and had nothing. */
   cJSON_AddStringToObject(resp, "result_status", answered != 0 ? "ok" : "empty");
   return send_and_free(conn, resp);
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
   int include_code = jo_bool(req, "include_code", 1);
   int fallback_enabled = jo_bool(req, "fallback", 1);
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
   int rows = 0, answered = 0, outages = 0;
   if (batch)
   {
      cJSON *e;
      cJSON_ArrayForEach(e, queries)
      {
         if (!cJSON_IsString(e) || !e->valuestring[0])
            continue; /* skip the malformed entry; the rest of the batch still answers */
         cJSON *row = server_index_investigate_packet(e->valuestring, symbol, project, include_code,
                                                      fallback_enabled);
         if (!row)
            continue;
         rows++;
         if (strcmp(investigate_row_status(row), "ok") == 0)
            answered++;
         else if (investigate_status_is_outage(investigate_row_status(row)))
            outages++;
         cJSON_AddItemToArray(arr, row);
      }
   }
   else
   {
      cJSON *row =
          server_index_investigate_packet(single, symbol, project, include_code, fallback_enabled);
      if (row)
      {
         rows++;
         if (strcmp(investigate_row_status(row), "ok") == 0)
            answered++;
         else if (investigate_status_is_outage(investigate_row_status(row)))
            outages++;
         cJSON_AddItemToArray(arr, row);
      }
   }
   return investigate_finish(conn, resp, project, rows, answered, outages);
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

   /* Resolve the project the way every sibling index command does. deps names
    * its project as a positional, but an agent working inside a checkout should
    * not have to discover that -- if the caller did not name one, fall back to
    * the active project for the caller cwd. */
   char deps_project[MAX_PATH_LEN] = "";
   const char *project = jo_str(req, "project", NULL);
   if (!project || !project[0])
   {
      const char *cwd = jo_str(req, "cwd", NULL);
      if (!cwd || server_active_project_from_cwd(cwd, deps_project, sizeof(deps_project)) != 0)
         return server_send_error(conn, "scope_required: no active project", NULL);
      project = deps_project;
   }

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

int handle_index_verify(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *project = jo_str(req, "project", NULL);
   const char *root = jo_str(req, "root", NULL);
   if (!project || !project[0] || !root || !root[0])
      return server_send_error(conn, "index.verify requires project and root", NULL);
   int deep = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "deep")) ? 1 : 0;
   int http_status = 0;
   char *json = kb_client_index_verify_json(project, root, deep, &http_status);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service unavailable", NULL);
   cJSON *workspace = cJSON_GetObjectItemCaseSensitive(resp, "workspace_state");
   if (http_status < 200 || http_status >= 300 || !cJSON_IsString(workspace) ||
       strcmp(workspace->valuestring, "matched") != 0)
   {
      cJSON_ReplaceItemInObject(resp, "status", cJSON_CreateString("error"));
      cJSON_AddStringToObject(resp, "message", "canonical index differs from workspace");
   }
   return send_and_free(conn, resp);
}
