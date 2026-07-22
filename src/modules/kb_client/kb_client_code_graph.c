/* kb_client_code_graph.c: server-side client for the KB code-graph retrieval +
 * analytics routes (/v1/code/hybrid, /v1/code/graph/hubs).
 *
 * Like kb_client_pdf.c, these routes return purpose-built JSON — the hybrid
 * route's fused {results[], why[]} and the hubs route's ranked {hubs[]} — that
 * the agent consumes directly, so we forward the route's body VERBATIM rather
 * than round-tripping through flat C structs (which would drop the nested shape).
 * Each function returns the malloc'd JSON string the caller frees, or NULL on a
 * parameter/transport/non-2xx failure; *status_out (when non-NULL) carries the
 * route's HTTP status so the caller can craft a useful message. Every
 * caller-supplied query-string value is URL-escaped (mirrors kb_client_index.c). */
#include "kb_client.h"
#include "kb_client_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KB_CLIENT_CODE_GRAPH_READ_TIMEOUT_MS (8 * 1000)

char *kb_client_code_hybrid(const char *query, const char *symbol, const char *project,
                            int max_results, int *status_out)
{
   if (status_out)
      *status_out = -1;
   if (!query || !query[0])
      return NULL;

   char *query_q = kb_client_query_escape(query);
   char *symbol_q = (symbol && symbol[0]) ? kb_client_query_escape(symbol) : NULL;
   char *project_q = (project && project[0]) ? kb_client_query_escape(project) : NULL;
   if (!query_q || ((symbol && symbol[0]) && !symbol_q) || ((project && project[0]) && !project_q))
   {
      free(query_q);
      free(symbol_q);
      free(project_q);
      return NULL;
   }
   if (max_results < 1)
      max_results = 1;

   size_t cap = strlen("/v1/code/hybrid?query=&max_results=&symbol=&project=") + strlen(query_q) +
                (symbol_q ? strlen(symbol_q) : 0) + (project_q ? strlen(project_q) : 0) + 32;
   char *path = malloc(cap);
   if (!path)
   {
      free(query_q);
      free(symbol_q);
      free(project_q);
      return NULL;
   }
   int off = snprintf(path, cap, "/v1/code/hybrid?query=%s&max_results=%d", query_q, max_results);
   if (symbol_q)
      off += snprintf(path + off, cap - (size_t)off, "&symbol=%s", symbol_q);
   if (project_q)
      snprintf(path + off, cap - (size_t)off, "&project=%s", project_q);
   free(query_q);
   free(symbol_q);
   free(project_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_CODE_GRAPH_READ_TIMEOUT_MS, status_out);
   free(path);
   return json;
}

char *kb_client_code_graph_hubs(const char *project, int max_results, int *status_out)
{
   if (status_out)
      *status_out = -1;
   if (!project || !project[0])
      return NULL;

   char *project_q = kb_client_query_escape(project);
   if (!project_q)
      return NULL;
   if (max_results < 1)
      max_results = 1;

   size_t cap = strlen("/v1/code/graph/hubs?project=&max_results=") + strlen(project_q) + 32;
   char *path = malloc(cap);
   if (!path)
   {
      free(project_q);
      return NULL;
   }
   snprintf(path, cap, "/v1/code/graph/hubs?project=%s&max_results=%d", project_q, max_results);
   free(project_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_CODE_GRAPH_READ_TIMEOUT_MS, status_out);
   free(path);
   return json;
}

int kb_client_code_lessons_observe(const char *project, const char *session_id,
                                   const char *const *node_ids, int n_nodes)
{
   if (!project || !project[0] || !session_id || !session_id[0] || !node_ids || n_nodes <= 0)
      return -1;
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return -1;
   cJSON_AddStringToObject(req, "project", project);
   cJSON_AddStringToObject(req, "session_id", session_id);
   cJSON *arr = cJSON_AddArrayToObject(req, "node_ids");
   for (int i = 0; arr && i < n_nodes; i++)
      if (node_ids[i] && node_ids[i][0])
         cJSON_AddItemToArray(arr, cJSON_CreateString(node_ids[i]));
   int status = -1;
   char *resp = kb_client_v1_post_json("/v1/code/lessons/observe", req,
                                       KB_CLIENT_CODE_GRAPH_READ_TIMEOUT_MS, &status);
   cJSON_Delete(req);
   free(resp);
   return (status >= 200 && status < 300) ? 0 : -1;
}

char *kb_client_code_lessons(const char *project, int *status_out)
{
   if (status_out)
      *status_out = -1;
   if (!project || !project[0])
      return NULL;
   char *project_q = kb_client_query_escape(project);
   if (!project_q)
      return NULL;
   size_t cap = strlen("/v1/code/lessons?project=") + strlen(project_q) + 8;
   char *path = malloc(cap);
   if (!path)
   {
      free(project_q);
      return NULL;
   }
   snprintf(path, cap, "/v1/code/lessons?project=%s", project_q);
   free(project_q);
   char *json = kb_client_v1_get_json(path, KB_CLIENT_CODE_GRAPH_READ_TIMEOUT_MS, status_out);
   free(path);
   return json;
}

char *kb_client_code_graph_audit(const char *project, int max_findings, int *status_out)
{
   if (status_out)
      *status_out = -1;
   if (!project || !project[0])
      return NULL;

   char *project_q = kb_client_query_escape(project);
   if (!project_q)
      return NULL;
   if (max_findings < 1)
      max_findings = 1;

   size_t cap = strlen("/v1/code/graph/audit?project=&max_findings=") + strlen(project_q) + 32;
   char *path = malloc(cap);
   if (!path)
   {
      free(project_q);
      return NULL;
   }
   snprintf(path, cap, "/v1/code/graph/audit?project=%s&max_findings=%d", project_q, max_findings);
   free(project_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_CODE_GRAPH_READ_TIMEOUT_MS, status_out);
   free(path);
   return json;
}

char *kb_client_code_graph_diff(const char *project, const char *from_gen, const char *to_gen,
                                int force, int *status_out)
{
   if (status_out)
      *status_out = -1;
   if (!project || !project[0] || !from_gen || !from_gen[0] || !to_gen || !to_gen[0])
      return NULL;

   char *project_q = kb_client_query_escape(project);
   char *from_q = kb_client_query_escape(from_gen);
   char *to_q = kb_client_query_escape(to_gen);
   if (!project_q || !from_q || !to_q)
   {
      free(project_q);
      free(from_q);
      free(to_q);
      return NULL;
   }
   size_t cap = strlen("/v1/code/graph/diff?project=&from_gen=&to_gen=&force=1") +
                strlen(project_q) + strlen(from_q) + strlen(to_q) + 8;
   char *path = malloc(cap);
   if (!path)
   {
      free(project_q);
      free(from_q);
      free(to_q);
      return NULL;
   }
   snprintf(path, cap, "/v1/code/graph/diff?project=%s&from_gen=%s&to_gen=%s%s", project_q, from_q,
            to_q, force ? "&force=1" : "");
   free(project_q);
   free(from_q);
   free(to_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_CODE_GRAPH_READ_TIMEOUT_MS, status_out);
   free(path);
   return json;
}

char *kb_client_code_graph_surprising(const char *project, int max_results, int judge,
                                      int *status_out)
{
   if (status_out)
      *status_out = -1;
   if (!project || !project[0])
      return NULL;

   char *project_q = kb_client_query_escape(project);
   if (!project_q)
      return NULL;
   if (max_results < 1)
      max_results = 1;

   size_t cap = strlen("/v1/code/graph/surprising?project=&max_results=&judge=true") +
                strlen(project_q) + 32;
   char *path = malloc(cap);
   if (!path)
   {
      free(project_q);
      return NULL;
   }
   snprintf(path, cap, "/v1/code/graph/surprising?project=%s&max_results=%d%s", project_q,
            max_results, judge ? "&judge=true" : "");
   free(project_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_CODE_GRAPH_READ_TIMEOUT_MS, status_out);
   free(path);
   return json;
}

char *kb_client_code_graph_node(const char *project, const char *node, int max_results,
                                int *status_out)
{
   if (status_out)
      *status_out = -1;
   if (!project || !project[0] || !node || !node[0])
      return NULL;

   char *project_q = kb_client_query_escape(project);
   char *node_q = kb_client_query_escape(node);
   if (!project_q || !node_q)
   {
      free(project_q);
      free(node_q);
      return NULL;
   }
   if (max_results < 1)
      max_results = 1;

   size_t cap = strlen("/v1/code/graph?project=&node=&max_results=") + strlen(project_q) +
                strlen(node_q) + 32;
   char *path = malloc(cap);
   if (!path)
   {
      free(project_q);
      free(node_q);
      return NULL;
   }
   snprintf(path, cap, "/v1/code/graph?project=%s&node=%s&max_results=%d", project_q, node_q,
            max_results);
   free(project_q);
   free(node_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_CODE_GRAPH_READ_TIMEOUT_MS, status_out);
   free(path);
   return json;
}
