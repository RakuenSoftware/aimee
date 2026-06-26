#include "kb_http_code.h"
#include "aimee.h"
#include "config.h"
#include "kb_curator_queue.h"
#include "cJSON.h"
#include "db2/canonical_index.h"
#include "db2/lifecycle.h"
#include "db2/memory_query.h"
#include "db2/code_projection.h"
#include "memory.h"
#include "kb/kb_rrf.h"
#include "kb/kb_graph_analytics.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int code_scan_bool(cJSON *root, const char *key, int default_val)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
   if (!item)
      return default_val;
   return cJSON_IsTrue(item) ? 1 : cJSON_IsFalse(item) ? 0 : default_val;
}

static int code_scan_write_error(char *out_buf, int out_cap, const char *message)
{
   snprintf(out_buf, (size_t)out_cap, "{\"error\":\"%s\"}", message ? message : "error");
   return 400;
}

static int code_method_not_allowed(char *out_buf, int out_cap)
{
   snprintf(out_buf, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
   return 405;
}

static int code_qparam(const char *qs, const char *key, char *out, int outsz)
{
   if (!qs || !key || !out || outsz <= 0)
      return 0;
   int klen = (int)strlen(key);
   const char *p = qs;
   while (*p)
   {
      if ((p == qs || p[-1] == '&') && strncmp(p, key, (size_t)klen) == 0 && p[klen] == '=')
      {
         p += klen + 1;
         int i = 0;
         while (*p && *p != '&' && i < outsz - 1)
         {
            if (*p == '%' && p[1] && p[2])
            {
               char hex[3] = {p[1], p[2], 0};
               out[i++] = (char)strtol(hex, NULL, 16);
               p += 3;
            }
            else if (*p == '+')
            {
               out[i++] = ' ';
               p++;
            }
            else
            {
               out[i++] = *p++;
            }
         }
         out[i] = '\0';
         return 1;
      }
      p = strchr(p, '&');
      if (!p)
         break;
      p++;
   }
   return 0;
}

int handle_get_code_projects(const char *query_string, char *out_buf, int out_cap)
{
   int max_r = 100;
   char max_r_s[16] = "";
   if (code_qparam(query_string, "max_results", max_r_s, sizeof(max_r_s)))
      max_r = atoi(max_r_s);
   if (max_r < 1)
      max_r = 1;
   if (max_r > 100)
      max_r = 100;

   project_info_t *projects = calloc((size_t)max_r, sizeof(project_info_t));
   if (!projects)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   int n = canonical_index_list_projects(projects, max_r);
   if (n < 0)
   {
      free(projects);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"canonical index unavailable (knowledge service not initialized)\"}");
      return 503;
   }

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(projects);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *arr = cJSON_AddArrayToObject(resp, "projects");
   for (int i = 0; arr && i < n; i++)
   {
      cJSON *project = cJSON_CreateObject();
      cJSON_AddStringToObject(project, "name", projects[i].name);
      cJSON_AddStringToObject(project, "root", projects[i].root);
      cJSON_AddStringToObject(project, "scanned_at", projects[i].scanned_at);
      cJSON_AddItemToArray(arr, project);
   }
   cJSON_AddNullToObject(resp, "next_cursor");
   char *json = cJSON_PrintUnformatted(resp);
   snprintf(out_buf, (size_t)out_cap, "%s", json ? json : "{\"status\":\"ok\",\"projects\":[]}");
   free(json);
   cJSON_Delete(resp);
   free(projects);
   return 200;
}

int handle_get_code_projects_route(const char *method, const char *query_string, char *out_buf,
                                   int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_projects(query_string, out_buf, out_cap);
}

int handle_get_code_find(const char *query_string, char *out_buf, int out_cap)
{
   char identifier[256] = "";
   char project_filter[256] = "";
   if (!code_qparam(query_string, "identifier", identifier, sizeof(identifier)) || !identifier[0])
      return code_scan_write_error(out_buf, out_cap, "missing identifier");
   code_qparam(query_string, "project", project_filter, sizeof(project_filter));

   int max_r = 20;
   char max_r_s[16] = "";
   if (code_qparam(query_string, "max_results", max_r_s, sizeof(max_r_s)))
      max_r = atoi(max_r_s);
   if (max_r < 1)
      max_r = 1;
   if (max_r > 100)
      max_r = 100;

   term_hit_t *hits = calloc((size_t)max_r, sizeof(term_hit_t));
   if (!hits)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   int n = canonical_index_find(identifier, hits, max_r);
   if (n < 0)
   {
      free(hits);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"canonical index unavailable (knowledge service not initialized)\"}");
      return 503;
   }

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(hits);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON *arr = cJSON_AddArrayToObject(resp, "hits");
   for (int i = 0; arr && i < n; i++)
   {
      if (project_filter[0] && strcmp(hits[i].project, project_filter) != 0)
         continue;
      cJSON *hit = cJSON_CreateObject();
      cJSON_AddStringToObject(hit, "project", hits[i].project);
      cJSON_AddStringToObject(hit, "file_path", hits[i].file_path);
      cJSON_AddNumberToObject(hit, "line", hits[i].line);
      cJSON_AddNumberToObject(hit, "line_end", hits[i].line_end);
      cJSON_AddStringToObject(hit, "kind", hits[i].kind);
      cJSON_AddItemToArray(arr, hit);
   }
   cJSON_AddNullToObject(resp, "next_cursor");
   char *json = cJSON_PrintUnformatted(resp);
   snprintf(out_buf, (size_t)out_cap, "%s", json ? json : "{\"hits\":[]}");
   free(json);
   cJSON_Delete(resp);
   free(hits);
   return 200;
}

int handle_get_code_find_route(const char *method, const char *query_string, char *out_buf,
                               int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_find(query_string, out_buf, out_cap);
}

int handle_get_code_blast_radius(const char *query_string, char *out_buf, int out_cap)
{
   char project[256] = "";
   char file_path[4096] = "";
   if (!code_qparam(query_string, "project", project, sizeof(project)) || !project[0])
      return code_scan_write_error(out_buf, out_cap, "missing project");
   if (!code_qparam(query_string, "file_path", file_path, sizeof(file_path)) || !file_path[0])
      return code_scan_write_error(out_buf, out_cap, "missing file_path");

   blast_radius_t *br = calloc(1, sizeof(blast_radius_t));
   if (!br)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   if (canonical_index_blast_radius(project, file_path, br) != 0)
   {
      free(br);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
      return 404;
   }

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(br);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "file", br->file);
   cJSON *dependents = cJSON_AddArrayToObject(resp, "dependents");
   for (int i = 0; dependents && i < br->dependent_count; i++)
      cJSON_AddItemToArray(dependents, cJSON_CreateString(br->dependents[i]));
   cJSON_AddNumberToObject(resp, "dependent_count", br->dependent_count);
   cJSON *dependencies = cJSON_AddArrayToObject(resp, "dependencies");
   for (int i = 0; dependencies && i < br->dependency_count; i++)
      cJSON_AddItemToArray(dependencies, cJSON_CreateString(br->dependencies[i]));
   cJSON_AddNumberToObject(resp, "dependency_count", br->dependency_count);
   char *json = cJSON_PrintUnformatted(resp);
   snprintf(out_buf, (size_t)out_cap, "%s", json ? json : "{\"file\":\"\"}");
   free(json);
   cJSON_Delete(resp);
   free(br);
   return 200;
}

int handle_get_code_blast_radius_route(const char *method, const char *query_string, char *out_buf,
                                       int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_blast_radius(query_string, out_buf, out_cap);
}

int handle_get_code_structure(const char *query_string, char *out_buf, int out_cap)
{
   char project[256] = "";
   char file_path[4096] = "";
   if (!code_qparam(query_string, "project", project, sizeof(project)) || !project[0])
      return code_scan_write_error(out_buf, out_cap, "missing project");
   if (!code_qparam(query_string, "file_path", file_path, sizeof(file_path)) || !file_path[0])
      return code_scan_write_error(out_buf, out_cap, "missing file_path");

   int max_r = 256;
   char max_r_s[16] = "";
   if (code_qparam(query_string, "max_results", max_r_s, sizeof(max_r_s)))
      max_r = atoi(max_r_s);
   if (max_r < 1)
      max_r = 1;
   if (max_r > 256)
      max_r = 256;

   definition_t *defs = calloc((size_t)max_r, sizeof(definition_t));
   if (!defs)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   int n = canonical_index_structure(project, file_path, defs, max_r);
   if (n < 0)
   {
      free(defs);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"canonical index unavailable (knowledge service not initialized)\"}");
      return 503;
   }

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(defs);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *arr = cJSON_AddArrayToObject(resp, "definitions");
   for (int i = 0; arr && i < n; i++)
   {
      cJSON *d = cJSON_CreateObject();
      cJSON_AddStringToObject(d, "name", defs[i].name);
      cJSON_AddStringToObject(d, "kind", defs[i].kind);
      cJSON_AddNumberToObject(d, "line", defs[i].line);
      cJSON_AddNumberToObject(d, "line_end", defs[i].line_end);
      cJSON_AddItemToArray(arr, d);
   }
   char *json = cJSON_PrintUnformatted(resp);
   snprintf(out_buf, (size_t)out_cap, "%s", json ? json : "{\"status\":\"ok\",\"definitions\":[]}");
   free(json);
   cJSON_Delete(resp);
   free(defs);
   return 200;
}

int handle_get_code_structure_route(const char *method, const char *query_string, char *out_buf,
                                    int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_structure(query_string, out_buf, out_cap);
}

int handle_get_code_search(const char *query_string, char *out_buf, int out_cap)
{
   char query[512] = "";
   char project[256] = "";
   if (!code_qparam(query_string, "query", query, sizeof(query)) || !query[0])
      return code_scan_write_error(out_buf, out_cap, "missing query");
   code_qparam(query_string, "project", project, sizeof(project));

   int max_r = 20;
   char max_r_s[16] = "";
   if (code_qparam(query_string, "max_results", max_r_s, sizeof(max_r_s)))
      max_r = atoi(max_r_s);
   if (max_r < 1)
      max_r = 1;
   if (max_r > 100)
      max_r = 100;

   code_search_hit_t *hits = calloc((size_t)max_r, sizeof(code_search_hit_t));
   if (!hits)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   /* Enrich matched-line spans only when ingress compression is enabled (the
    * lossy-fold consumer). Default-off keeps the query and JSON identical. */
   config_t scfg;
   int enrich = (config_load(&scfg) == 0 && scfg.ingress_compress_enabled) ? 1 : 0;
   int n = canonical_index_code_search(query, project[0] ? project : NULL, hits, max_r, enrich);
   if (n < 0)
   {
      free(hits);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"canonical index unavailable (knowledge service not initialized)\"}");
      return 503;
   }

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(hits);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *arr = cJSON_AddArrayToObject(resp, "hits");
   for (int i = 0; arr && i < n; i++)
   {
      cJSON *hit = cJSON_CreateObject();
      cJSON_AddStringToObject(hit, "project", hits[i].project);
      cJSON_AddStringToObject(hit, "file_path", hits[i].file_path);
      cJSON_AddStringToObject(hit, "snippet", hits[i].snippet);
      cJSON_AddNumberToObject(hit, "rank", hits[i].rank);
      /* P2 Layer-1: file content hash for citation + drift detection. */
      cJSON_AddStringToObject(hit, "content_hash", hits[i].content_hash);
      /* P1b span enrichment: 1-based matched line, only when computed (>0). */
      if (hits[i].line > 0)
         cJSON_AddNumberToObject(hit, "line", hits[i].line);
      cJSON_AddItemToArray(arr, hit);
   }
   cJSON_AddNullToObject(resp, "next_cursor");
   char *json = cJSON_PrintUnformatted(resp);
   snprintf(out_buf, (size_t)out_cap, "%s", json ? json : "{\"status\":\"ok\",\"hits\":[]}");
   free(json);
   cJSON_Delete(resp);
   free(hits);
   return 200;
}

int handle_get_code_search_route(const char *method, const char *query_string, char *out_buf,
                                 int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_search(query_string, out_buf, out_cap);
}

int handle_get_code_callers(const char *query_string, char *out_buf, int out_cap)
{
   char symbol[256] = "";
   char project[256] = "";
   if (!code_qparam(query_string, "symbol", symbol, sizeof(symbol)) || !symbol[0])
      return code_scan_write_error(out_buf, out_cap, "missing symbol");
   code_qparam(query_string, "project", project, sizeof(project));

   int max_r = 20;
   char max_r_s[16] = "";
   if (code_qparam(query_string, "max_results", max_r_s, sizeof(max_r_s)))
      max_r = atoi(max_r_s);
   if (max_r < 1)
      max_r = 1;
   if (max_r > 100)
      max_r = 100;

   caller_hit_t *hits = calloc((size_t)max_r, sizeof(caller_hit_t));
   if (!hits)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   int n = canonical_index_find_callers(project[0] ? project : NULL, symbol, hits, max_r);
   if (n < 0)
   {
      free(hits);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"canonical index unavailable (knowledge service not initialized)\"}");
      return 503;
   }

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(hits);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON *arr = cJSON_AddArrayToObject(resp, "hits");
   for (int i = 0; arr && i < n; i++)
   {
      cJSON *hit = cJSON_CreateObject();
      cJSON_AddStringToObject(hit, "project", hits[i].project);
      cJSON_AddStringToObject(hit, "file_path", hits[i].file_path);
      cJSON_AddStringToObject(hit, "caller", hits[i].caller);
      cJSON_AddNumberToObject(hit, "line", hits[i].line);
      cJSON_AddItemToArray(arr, hit);
   }
   cJSON_AddNullToObject(resp, "next_cursor");
   char *json = cJSON_PrintUnformatted(resp);
   snprintf(out_buf, (size_t)out_cap, "%s", json ? json : "{\"status\":\"ok\",\"hits\":[]}");
   free(json);
   cJSON_Delete(resp);
   free(hits);
   return 200;
}

int handle_get_code_callers_route(const char *method, const char *query_string, char *out_buf,
                                  int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_callers(query_string, out_buf, out_cap);
}

int handle_get_code_project_stats(const char *query_string, char *out_buf, int out_cap)
{
   char project[256] = "";
   if (!code_qparam(query_string, "project", project, sizeof(project)) || !project[0])
      return code_scan_write_error(out_buf, out_cap, "missing project");

   int files = 0;
   int defs = 0;
   if (canonical_index_project_stats(project, &files, &defs) != 0)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"canonical index unavailable (knowledge service not initialized)\"}");
      return 503;
   }

   char langs_json[1024];
   if (canonical_index_project_lang_breakdown(project, langs_json, sizeof(langs_json)) != 0)
      snprintf(langs_json, sizeof(langs_json), "[]");

   cJSON *resp = cJSON_CreateObject();
   cJSON *langs = cJSON_Parse(langs_json);
   if (!resp)
   {
      cJSON_Delete(langs);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "project", project);
   cJSON_AddNumberToObject(resp, "files", files);
   cJSON_AddNumberToObject(resp, "definitions", defs);
   cJSON *langs_out = cJSON_IsArray(langs) ? langs : cJSON_CreateArray();
   cJSON_AddItemToObject(resp, "langs", langs_out);
   if (langs && langs != langs_out)
      cJSON_Delete(langs);

   char *json = cJSON_PrintUnformatted(resp);
   snprintf(out_buf, (size_t)out_cap, "%s", json ? json : "{\"status\":\"ok\",\"langs\":[]}");
   free(json);
   cJSON_Delete(resp);
   return 200;
}

int handle_get_code_project_stats_route(const char *method, const char *query_string, char *out_buf,
                                        int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_project_stats(query_string, out_buf, out_cap);
}

/* GET /v1/code/hybrid?query=<text>&symbol=<sym>&project=<proj>&max_results=N
 *
 * Hybrid code retrieval (proposal §5): fuse independently-ranked signals through
 * Reciprocal Rank Fusion (kb_rrf_fuse) into one ranking, plus a memory "why"
 * context. Two signals are fused in FILE-PATH space so consensus is meaningful —
 * a file that is BOTH textually relevant to `query` AND structurally connected to
 * `symbol` (calls it) rises to the top:
 *   - "code"  : lexical search over file contents (canonical_index_code_search);
 *   - "graph" : callers of `symbol` from the structural call graph
 *               (canonical_index_find_callers), marked structural (tie-break).
 * Memory recall (db2_memory_find_facts_like) is returned as a separate `why`
 * array — the recorded reasoning behind the code, not a file, so it is context
 * rather than a fused row. The vector signal (pgvec_code_search) slots in as a
 * third fused leg once the query embedder is wired (integration-tier). */
#define HYBRID_PER_SIGNAL 25
#define HYBRID_WHY_MAX    5

int handle_get_code_hybrid(const char *query_string, char *out_buf, int out_cap)
{
   char query[512] = "";
   char symbol[256] = "";
   char project[256] = "";
   if (!code_qparam(query_string, "query", query, sizeof(query)) || !query[0])
      return code_scan_write_error(out_buf, out_cap, "missing query");
   code_qparam(query_string, "symbol", symbol, sizeof(symbol));
   code_qparam(query_string, "project", project, sizeof(project));
   const char *proj = project[0] ? project : NULL;

   int max_r = 20;
   char mr[16] = "";
   if (code_qparam(query_string, "max_results", mr, sizeof(mr)))
      max_r = atoi(mr);
   if (max_r < 1)
      max_r = 1;
   if (max_r > 100)
      max_r = 100;

   if (!db2_is_initialized())
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"knowledge service not initialized\"}");
      return 503;
   }

   code_search_hit_t *chits = calloc(HYBRID_PER_SIGNAL, sizeof(*chits));
   caller_hit_t *ghits = calloc(HYBRID_PER_SIGNAL, sizeof(*ghits));
   memory_t *mems = calloc(HYBRID_PER_SIGNAL, sizeof(*mems));
   kb_rrf_item_t *code_items = calloc(HYBRID_PER_SIGNAL, sizeof(*code_items));
   kb_rrf_item_t *graph_items = calloc(HYBRID_PER_SIGNAL, sizeof(*graph_items));
   kb_rrf_result_t *fused = calloc(HYBRID_PER_SIGNAL * 2, sizeof(*fused));
   if (!chits || !ghits || !mems || !code_items || !graph_items || !fused)
   {
      free(chits);
      free(ghits);
      free(mems);
      free(code_items);
      free(graph_items);
      free(fused);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }

   /* Signal A — lexical code (key = file_path). No span enrichment here — hybrid
    * ranking does not surface matched-line spans. */
   int nc = canonical_index_code_search(query, proj, chits, HYBRID_PER_SIGNAL, 0);
   if (nc < 0)
      nc = 0;
   for (int i = 0; i < nc; i++)
   {
      snprintf(code_items[i].id, sizeof(code_items[i].id), "%s", chits[i].file_path);
      code_items[i].structural_weight = 0;
   }

   /* Signal B — graph callers of `symbol` (key = file_path; structural edge).
    * Pass `proj` (NULL when absent) to match Signal A and the existing
    * /v1/code/callers route — canonical_index_find_callers takes its all-projects
    * SQL path on NULL, so both legs scope identically instead of one searching all
    * projects (NULL) while the other got "" (which is not the all-projects sentinel). */
   int ng = 0;
   if (symbol[0])
   {
      ng = canonical_index_find_callers(proj, symbol, ghits, HYBRID_PER_SIGNAL);
      if (ng < 0)
         ng = 0;
      for (int i = 0; i < ng; i++)
      {
         snprintf(graph_items[i].id, sizeof(graph_items[i].id), "%s", ghits[i].file_path);
         graph_items[i].structural_weight = 1; /* a structural call edge */
      }
   }

   /* Per-signal RRF weights + rank constant are config-tunable (§5). */
   config_t hcfg;
   double w_code = 1.0, w_graph = 1.0, rrf_k = KB_RRF_DEFAULT_K;
   if (config_load(&hcfg) == 0)
   {
      w_code = hcfg.code_hybrid_weight_code;
      w_graph = hcfg.code_hybrid_weight_graph;
      if (hcfg.code_hybrid_rrf_k > 0)
         rrf_k = hcfg.code_hybrid_rrf_k;
   }
   kb_rrf_signal_t sigs[2] = {
       {code_items, nc, w_code, "code"},
       {graph_items, ng, w_graph, "graph"},
   };
   int nf = kb_rrf_fuse(sigs, 2, rrf_k, fused, HYBRID_PER_SIGNAL * 2);
   if (nf < 0)
      nf = 0;
   if (nf > max_r)
      nf = max_r;

   /* Memory "why" context (recorded reasoning, capped). */
   int nm = db2_memory_find_facts_like(query, HYBRID_WHY_MAX, mems, HYBRID_PER_SIGNAL);
   if (nm < 0)
      nm = 0;
   if (nm > HYBRID_WHY_MAX)
      nm = HYBRID_WHY_MAX;

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(chits);
      free(ghits);
      free(mems);
      free(code_items);
      free(graph_items);
      free(fused);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "query", query);
   if (symbol[0])
      cJSON_AddStringToObject(resp, "symbol", symbol);
   if (project[0])
      cJSON_AddStringToObject(resp, "project", project);

   cJSON *results = cJSON_AddArrayToObject(resp, "results");
   for (int i = 0; results && i < nf; i++)
   {
      const char *fp = fused[i].id;
      cJSON *row = cJSON_CreateObject();
      if (!row)
         continue;
      cJSON_AddStringToObject(row, "file_path", fp);
      cJSON_AddNumberToObject(row, "score", fused[i].score);
      cJSON_AddNumberToObject(row, "signal_hits", fused[i].signal_hits);
      cJSON_AddNumberToObject(row, "structural_weight", fused[i].structural_weight);
      cJSON *which = cJSON_AddArrayToObject(row, "signals");
      /* Enrich + label from whichever source(s) carried this file. */
      for (int j = 0; j < nc; j++)
         if (strcmp(chits[j].file_path, fp) == 0)
         {
            if (which)
               cJSON_AddItemToArray(which, cJSON_CreateString("code"));
            cJSON_AddStringToObject(row, "snippet", chits[j].snippet);
            if (chits[j].content_hash[0])
               cJSON_AddStringToObject(row, "content_hash", chits[j].content_hash);
            break;
         }
      for (int j = 0; j < ng; j++)
         if (strcmp(ghits[j].file_path, fp) == 0)
         {
            if (which)
               cJSON_AddItemToArray(which, cJSON_CreateString("graph"));
            cJSON_AddStringToObject(row, "caller", ghits[j].caller);
            cJSON_AddNumberToObject(row, "caller_line", ghits[j].line);
            break;
         }
      cJSON_AddItemToArray(results, row);
   }

   cJSON *why = cJSON_AddArrayToObject(resp, "why");
   for (int i = 0; why && i < nm; i++)
   {
      cJSON *m = cJSON_CreateObject();
      if (!m)
         continue;
      cJSON_AddNumberToObject(m, "id", (double)mems[i].id);
      cJSON_AddStringToObject(m, "kind", mems[i].kind);
      if (mems[i].headline[0])
         cJSON_AddStringToObject(m, "headline", mems[i].headline);
      cJSON_AddStringToObject(m, "content", mems[i].content);
      cJSON_AddItemToArray(why, m);
   }

   char *s = cJSON_PrintUnformatted(resp);
   int status = 200;
   if (!s)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      status = 500;
   }
   else if (strlen(s) >= (size_t)out_cap)
   {
      /* Never return truncated (invalid) JSON: signal the caller to narrow. */
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"result too large; reduce max_results or narrow the "
               "query\",\"code\":\"result_too_large\"}");
      status = 413;
   }
   else
   {
      snprintf(out_buf, (size_t)out_cap, "%s", s);
   }
   free(s);
   cJSON_Delete(resp);
   free(chits);
   free(ghits);
   free(mems);
   free(code_items);
   free(graph_items);
   free(fused);
   return status;
}

int handle_get_code_hybrid_route(const char *method, const char *query_string, char *out_buf,
                                 int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_hybrid(query_string, out_buf, out_cap);
}

/* GET /v1/code/graph/hubs?project=<proj>&max_results=N
 *
 * Graph analytics (proposal §4): rank a project's most-connected symbols by
 * degree centrality over the visible code projection graph — a refactor-risk
 * signal ("editing this touches a lot"). Reads the published generation's edges
 * (db2_code_projection_list_edges), computes hubs with the pure kb_graph_hubs,
 * and returns the top N with in/out/weighted degree. Read-only. */
#define HUBS_MAX_EDGES 10000

int handle_get_code_graph_hubs(const char *query_string, char *out_buf, int out_cap)
{
   char project[256] = "";
   if (!code_qparam(query_string, "project", project, sizeof(project)) || !project[0])
      return code_scan_write_error(out_buf, out_cap, "missing project");

   int max_r = 20;
   char mr[16] = "";
   if (code_qparam(query_string, "max_results", mr, sizeof(mr)))
      max_r = atoi(mr);
   if (max_r < 1)
      max_r = 1;
   if (max_r > 200)
      max_r = 200;

   if (!db2_is_initialized())
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"knowledge service not initialized\"}");
      return 503;
   }

   code_projection_edge_t *edges = calloc(HUBS_MAX_EDGES, sizeof(*edges));
   kb_graph_edge_t *gedges = calloc(HUBS_MAX_EDGES, sizeof(*gedges));
   kb_graph_hub_t *hubs = calloc((size_t)max_r, sizeof(*hubs));
   if (!edges || !gedges || !hubs)
   {
      free(edges);
      free(gedges);
      free(hubs);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }

   int ne = db2_code_projection_list_edges(project, edges, HUBS_MAX_EDGES);
   if (ne < 0)
   {
      free(edges);
      free(gedges);
      free(hubs);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"projection graph unavailable (knowledge service not initialized)\"}");
      return 503;
   }
   for (int i = 0; i < ne; i++)
   {
      snprintf(gedges[i].source, sizeof(gedges[i].source), "%s", edges[i].source);
      snprintf(gedges[i].target, sizeof(gedges[i].target), "%s", edges[i].target);
      gedges[i].weight = edges[i].structural_weight;
   }
   free(edges); /* converted; drop before kb_graph_hubs allocates its accumulator */
   edges = NULL;

   int nh = kb_graph_hubs(gedges, ne, hubs, max_r);
   if (nh < 0)
      nh = 0;

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(gedges);
      free(hubs);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "project", project);
   cJSON_AddNumberToObject(resp, "edge_count", ne);
   /* A full edge buffer means the projection graph was larger than the analytics
    * cap, so the degree counts are over a (deterministic, source/target-ordered)
    * prefix rather than the whole graph — surface that instead of implying totals. */
   cJSON_AddBoolToObject(resp, "truncated", ne >= HUBS_MAX_EDGES);
   cJSON *arr = cJSON_AddArrayToObject(resp, "hubs");
   for (int i = 0; arr && i < nh; i++)
   {
      cJSON *h = cJSON_CreateObject();
      if (!h)
         continue;
      cJSON_AddStringToObject(h, "node", hubs[i].node);
      cJSON_AddNumberToObject(h, "degree", hubs[i].degree);
      cJSON_AddNumberToObject(h, "in_degree", hubs[i].in_degree);
      cJSON_AddNumberToObject(h, "out_degree", hubs[i].out_degree);
      cJSON_AddNumberToObject(h, "weighted_degree", hubs[i].weighted_degree);
      cJSON_AddItemToArray(arr, h);
   }

   char *s = cJSON_PrintUnformatted(resp);
   int status = 200;
   if (!s)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      status = 500;
   }
   else if (strlen(s) >= (size_t)out_cap)
   {
      snprintf(
          out_buf, (size_t)out_cap,
          "{\"error\":\"result too large; reduce max_results\",\"code\":\"result_too_large\"}");
      status = 413;
   }
   else
   {
      snprintf(out_buf, (size_t)out_cap, "%s", s);
   }
   free(s);
   cJSON_Delete(resp);
   free(gedges);
   free(hubs);
   return status;
}

int handle_get_code_graph_hubs_route(const char *method, const char *query_string, char *out_buf,
                                     int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_graph_hubs(query_string, out_buf, out_cap);
}

int handle_post_code_scan(const char *body, char *out_buf, int out_cap)
{
   cJSON *root = cJSON_Parse(body ? body : "{}");
   if (!root)
      return code_scan_write_error(out_buf, out_cap, "invalid json");

   cJSON *project_j = cJSON_GetObjectItemCaseSensitive(root, "project");
   const char *project = cJSON_IsString(project_j) ? project_j->valuestring : "";
   if (!project || !project[0])
   {
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing project\"}");
      return 400;
   }

   cJSON *root_path_j = cJSON_GetObjectItemCaseSensitive(root, "root_path");
   const char *root_path = cJSON_IsString(root_path_j) ? root_path_j->valuestring : "";
   int force = code_scan_bool(root, "force", 0);
   cJSON *files_j = cJSON_GetObjectItemCaseSensitive(root, "files");

   if (!db2_is_initialized())
   {
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"failed to open knowledge service store\"}");
      return 503;
   }

   int files = -1;
   int inspected = 0;
   int pushed_files = cJSON_IsArray(files_j);
   if (pushed_files)
   {
      int n = cJSON_GetArraySize(files_j);
      canonical_index_file_input_t *inputs = calloc((size_t)(n > 0 ? n : 1), sizeof(*inputs));
      if (!inputs)
      {
         cJSON_Delete(root);
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"out of memory\"}");
         return 503;
      }
      for (int i = 0; i < n; i++)
      {
         cJSON *entry = cJSON_GetArrayItem(files_j, i);
         cJSON *rel_path_j = cJSON_GetObjectItemCaseSensitive(entry, "rel_path");
         cJSON *content_j = cJSON_GetObjectItemCaseSensitive(entry, "content");
         if (!cJSON_IsString(rel_path_j) || !rel_path_j->valuestring[0] ||
             !cJSON_IsString(content_j))
         {
            free(inputs);
            cJSON_Delete(root);
            return code_scan_write_error(out_buf, out_cap, "invalid files array");
         }
         inputs[i].rel_path = rel_path_j->valuestring;
         inputs[i].content = content_j->valuestring;
      }
      files = canonical_index_scan_files(project, root_path && root_path[0] ? root_path : "remote",
                                         inputs, n, force, &inspected);
      free(inputs);
   }
   else
   {
      if (!root_path || !root_path[0])
      {
         cJSON_Delete(root);
         snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing root_path\"}");
         return 400;
      }
      files = canonical_index_scan_project(project, root_path, force, &inspected);
   }
   if (files < 0)
   {
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"canonical index scan failed\"}");
      return 503;
   }

   /* Queue code units for the deep curator on BOTH paths — a local scan and a
    * thin-client push (which sends `files`). The queue reads code units from DB2
    * by project name (it ignores root_path, so the server need not see the
    * client's filesystem) and self-gates on kb_curator_extract_code_enabled.
    * Previously this ran only for `!pushed_files`, so workspaces ingested from a
    * thin client were never queued for curation. The 0.6B embed pass is driven
    * separately by the curator drain. */
   kb_curator_queue_code_units_for_project(project, root_path);

   snprintf(out_buf, (size_t)out_cap,
            "{\"status\":\"ok\",\"skipped\":false,\"project\":\"%s\",\"files\":%d,"
            "\"inspected\":%d}",
            project, files, inspected);
   cJSON_Delete(root);
   return 200;
}

int handle_post_code_scan_route(const char *method, const char *body, char *out_buf, int out_cap)
{
   if (strcmp(method, "POST") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_post_code_scan(body, out_buf, out_cap);
}
