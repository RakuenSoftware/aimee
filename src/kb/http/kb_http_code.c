#include "kb_http_code.h"
#include "aimee.h"
#include "config.h"
#include "kb_curator_queue.h"
#include "cJSON.h"
#include "db2/canonical_index.h"
#include "db2/cross_repo_classify.h" /* xrepo_tier_name */
#include "db2/cross_repo_deps.h"     /* canonical_index_cross_repo_deps */
#include "db2/cross_repo_review.h"   /* db2_cross_repo_review_list */
#include "db2/cross_repo_stats.h"    /* db2_cross_repo_set_trust, recompute_blocked_symbols */
#include "db2/lifecycle.h"
#include "db2/memory_query.h"
#include "db2/code_projection.h"
#include "db2/pgvec_transport.h"
#include "db2/entity_edges.h"     /* §6 memory-fusion leg: knowledge-graph edges */
#include "db2/entity_nodes.h"     /* db2_entity_node_get -> file_path */
#include "code_collect.h"         /* §6 live: git_resolve_default_sha + change gate */
#include "db2/kb_runtime_state.h" /* stored last-indexed default-branch SHA */
#include "memory.h"
#include "kb/kb_rrf.h"
#include "kb/kb_graph_analytics.h"
#include "kb/kb_service_graph.h"
#include "kb/kb_surprising_judge.h"
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

/* GET /v1/code/cross-repo-deps?project=X&direction=out|in|both&min_tier=high|medium|tentative
 *                              [&status=ambiguous]
 * Cross-repo dependency edges for a project (§3.7/§3.9), or the AMBIGUOUS review
 * queue when status=ambiguous (§3.8). */
static xrepo_tier_t crd_parse_min_tier(const char *s)
{
   if (s && strcmp(s, "high") == 0)
      return XREPO_TIER_HIGH;
   if (s && (strcmp(s, "tentative") == 0 || strcmp(s, "low") == 0))
      return XREPO_TIER_LOW;
   return XREPO_TIER_MEDIUM;
}

/* Serialize `resp` into the fixed out_buf, returning 413 (not a silently
 * truncated 200) if it would not fit. Always consumes (frees) `resp`. */
static int crd_emit(cJSON *resp, char *out_buf, int out_cap)
{
   if (!resp)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   char *json = cJSON_PrintUnformatted(resp);
   cJSON_Delete(resp);
   if (!json)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   int rc = 200;
   if (strlen(json) >= (size_t)out_cap)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"response too large; narrow the query\",\"code\":413}");
      rc = 413;
   }
   else
      snprintf(out_buf, (size_t)out_cap, "%s", json);
   free(json);
   return rc;
}

static int handle_get_code_cross_repo_review(const char *project, char *out_buf, int out_cap)
{
   enum
   {
      MAXR = 200
   };
   xrepo_review_row_t *rows = calloc((size_t)MAXR, sizeof(*rows));
   if (!rows)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   int64_t dropped = 0;
   int n = db2_cross_repo_review_list(project, "open", rows, MAXR, &dropped);
   if (n < 0)
   {
      free(rows);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"canonical index unavailable\"}");
      return 503;
   }
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(rows);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "project", project);
   cJSON *arr = cJSON_AddArrayToObject(resp, "ambiguous");
   for (int i = 0; arr && i < n; i++)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "id", (double)rows[i].id);
      cJSON_AddStringToObject(o, "symbol", rows[i].symbol);
      cJSON_AddStringToObject(o, "caller_repo", rows[i].caller_repo);
      cJSON_AddStringToObject(o, "candidate_definer", rows[i].candidate_definer);
      cJSON_AddStringToObject(o, "review_class", rows[i].review_class);
      cJSON_AddNumberToObject(o, "evidence_score", rows[i].evidence_score);
      cJSON *ev = cJSON_Parse(rows[i].evidence); /* embed as object, else raw string */
      if (ev)
         cJSON_AddItemToObject(o, "evidence", ev);
      else
         cJSON_AddStringToObject(o, "evidence", rows[i].evidence);
      cJSON_AddItemToArray(arr, o);
   }
   cJSON *ov = cJSON_AddObjectToObject(resp, "overflow");
   if (ov)
      cJSON_AddNumberToObject(ov, "dropped", (double)dropped);
   free(rows); /* cJSON has copied every field */
   return crd_emit(resp, out_buf, out_cap);
}

int handle_get_code_cross_repo_deps(const char *query_string, char *out_buf, int out_cap)
{
   char project[256] = "", dir_s[16] = "", tier_s[16] = "", status_s[16] = "", dry_s[8] = "";
   if (!code_qparam(query_string, "project", project, sizeof(project)) || !project[0])
      return code_scan_write_error(out_buf, out_cap, "missing project");
   code_qparam(query_string, "direction", dir_s, sizeof(dir_s));
   code_qparam(query_string, "min_tier", tier_s, sizeof(tier_s));
   code_qparam(query_string, "status", status_s, sizeof(status_s));
   code_qparam(query_string, "dry_run", dry_s, sizeof(dry_s));
   int dry_run = (strcmp(dry_s, "1") == 0 || strcmp(dry_s, "true") == 0);

   if (strcmp(status_s, "ambiguous") == 0)
      return handle_get_code_cross_repo_review(project, out_buf, out_cap);

   /* direction: out (deps OF project, default), in (dependents, §B --reverse), or
    * both. An unrecognized non-empty value is a client error, not a silent OUT. */
   xrepo_direction_t dir = XREPO_DIR_OUT;
   if (dir_s[0])
   {
      if (strcmp(dir_s, "out") == 0)
         dir = XREPO_DIR_OUT;
      else if (strcmp(dir_s, "in") == 0)
         dir = XREPO_DIR_IN;
      else if (strcmp(dir_s, "both") == 0)
         dir = XREPO_DIR_BOTH;
      else
      {
         snprintf(out_buf, (size_t)out_cap,
                  "{\"error\":\"direction must be out, in, or both\",\"code\":400}");
         return 400;
      }
   }
   xrepo_deps_opts_t opts = {.min_tier = crd_parse_min_tier(tier_s),
                             .direction = dir,
                             .include_review = 0,
                             .dry_run = dry_run};

   xrepo_dep_edge_t *edges = NULL;
   size_t n = 0;
   int trunc = 0;
   xrepo_amb_cand_t *amb = NULL;
   size_t amb_n = 0;
   if (canonical_index_cross_repo_deps_ex(project, &opts, &edges, &n, &trunc, dry_run ? &amb : NULL,
                                          dry_run ? &amb_n : NULL) != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"canonical index unavailable\"}");
      return 503;
   }
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(amb);
      free(edges);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "project", project);
   cJSON_AddBoolToObject(resp, "truncated", trunc ? 1 : 0);
   if (dry_run)
      cJSON_AddBoolToObject(resp, "dry_run", 1);
   cJSON *arr = cJSON_AddArrayToObject(resp, "deps");
   for (size_t i = 0; arr && i < n; i++)
   {
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "caller_repo", edges[i].caller_repo);
      cJSON_AddStringToObject(e, "definer_repo", edges[i].definer_repo);
      cJSON_AddStringToObject(e, "tier", xrepo_tier_name(edges[i].tier));
      cJSON_AddNumberToObject(e, "symbol_count", edges[i].symbol_count);
      cJSON_AddNumberToObject(e, "call_site_count", edges[i].call_site_count);
      cJSON_AddBoolToObject(e, "import_corroborated", edges[i].import_corroborated ? 1 : 0);
      cJSON_AddBoolToObject(e, "export_corroborated", edges[i].export_corroborated ? 1 : 0);
      /* recall R2c: evidence class (symbol_resolved|build_declared|both) + build_kind */
      cJSON_AddStringToObject(e, "evidence_type",
                              edges[i].evidence_type[0] ? edges[i].evidence_type
                                                        : "symbol_resolved");
      if (edges[i].build_kind[0])
         cJSON_AddStringToObject(e, "build_kind", edges[i].build_kind);
      cJSON *ex = cJSON_AddObjectToObject(e, "example");
      if (ex)
      {
         cJSON_AddStringToObject(ex, "symbol", edges[i].example_symbol);
         cJSON_AddStringToObject(ex, "file", edges[i].example_file);
         cJSON_AddNumberToObject(ex, "line", edges[i].example_line);
      }
      cJSON *v = cJSON_AddObjectToObject(e, "version");
      if (v)
      {
         cJSON_AddStringToObject(v, "repo_set_hash", edges[i].repo_set_hash);
         cJSON_AddNumberToObject(v, "distinctiveness_v", edges[i].distinctiveness_v);
         cJSON_AddNumberToObject(v, "blocked_symbols_version",
                                 (double)edges[i].blocked_symbols_version);
         cJSON_AddNumberToObject(v, "resolver_version", edges[i].resolver_version);
      }
      cJSON_AddItemToArray(arr, e);
   }
   free(edges); /* cJSON has copied every field */

   /* --dry-run: surface the AMBIGUOUS candidates the pipeline held back (would
    * normally go to the review queue) so offline inspection sees every band. */
   if (dry_run)
   {
      cJSON *aarr = cJSON_AddArrayToObject(resp, "ambiguous");
      for (size_t i = 0; aarr && i < amb_n; i++)
      {
         cJSON *a = cJSON_CreateObject();
         cJSON_AddStringToObject(a, "symbol", amb[i].symbol);
         cJSON_AddStringToObject(a, "caller_repo", amb[i].caller_repo);
         cJSON_AddStringToObject(a, "candidate_definer", amb[i].candidate_definer);
         cJSON_AddStringToObject(a, "review_class", "ambiguous");
         cJSON_AddNumberToObject(a, "evidence_score", amb[i].evidence_score);
         cJSON *ev = cJSON_Parse(amb[i].evidence); /* embed as object, else raw string */
         if (ev)
            cJSON_AddItemToObject(a, "evidence", ev);
         else
            cJSON_AddStringToObject(a, "evidence", amb[i].evidence);
         cJSON_AddItemToArray(aarr, a);
      }
   }
   free(amb); /* cJSON has copied every field */
   return crd_emit(resp, out_buf, out_cap);
}

int handle_get_code_cross_repo_deps_route(const char *method, const char *query_string,
                                          char *out_buf, int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_cross_repo_deps(query_string, out_buf, out_cap);
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
   kb_rrf_item_t *vector_items = calloc(HYBRID_PER_SIGNAL, sizeof(*vector_items));
   kb_rrf_item_t *memory_items = calloc(HYBRID_PER_SIGNAL, sizeof(*memory_items));
   char(*vpaths)[256] = calloc(HYBRID_PER_SIGNAL, sizeof(*vpaths));
   double *vscores = calloc(HYBRID_PER_SIGNAL, sizeof(*vscores));
   kb_rrf_result_t *fused = calloc(HYBRID_PER_SIGNAL * 4, sizeof(*fused));
   if (!chits || !ghits || !mems || !code_items || !graph_items || !vector_items || !memory_items ||
       !vpaths || !vscores || !fused)
   {
      free(chits);
      free(ghits);
      free(mems);
      free(code_items);
      free(graph_items);
      free(vector_items);
      free(memory_items);
      free(vpaths);
      free(vscores);
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
   double w_code = 1.0, w_graph = 1.0, w_vector = 1.0, w_memory = 1.0, rrf_k = KB_RRF_DEFAULT_K;
   if (config_load(&hcfg) == 0)
   {
      w_code = hcfg.code_hybrid_weight_code;
      w_graph = hcfg.code_hybrid_weight_graph;
      w_vector = hcfg.code_hybrid_weight_vector;
      w_memory = hcfg.code_hybrid_weight_memory;
      if (hcfg.code_hybrid_rrf_k > 0)
         rrf_k = hcfg.code_hybrid_rrf_k;
   }

   /* Signal C — vector similarity (key = file_path; §5). Embed the query and
    * pgvec-search code_embeddings. Gated on a real embedder whose output dim
    * matches the corpus (db2_embedding_dim): on a dim mismatch (e.g. the 384-dim
    * builtin vs a 2560-dim corpus) or a down/unconfigured embedder the leg is
    * simply empty and the route degrades to code+graph. w_vector<=0 disables it. */
   int nv = 0;
   if (w_vector > 0.0)
   {
      const char *embed_cmd = config_embedding_command(&hcfg, NULL);
      float qvec[EMBED_MAX_DIM];
      int qdim = memory_embed_text(query, embed_cmd, qvec, EMBED_MAX_DIM);
      if (qdim > 0 && qdim == db2_embedding_dim())
      {
         nv = pgvec_code_search_paths(proj, qvec, qdim, HYBRID_PER_SIGNAL, (char *)vpaths,
                                      (int)sizeof(vpaths[0]), vscores, HYBRID_PER_SIGNAL);
         if (nv < 0)
            nv = 0;
         for (int i = 0; i < nv; i++)
         {
            snprintf(vector_items[i].id, sizeof(vector_items[i].id), "%s", vpaths[i]);
            vector_items[i].structural_weight = 0;
         }
      }
   }

   /* Signal D — cross-session memory / knowledge graph (§6 fusion). Symbol-anchored
    * like the graph leg: seed the symbol's entity node and walk its incident
    * knowledge-graph edges (built by the curator across sessions), resolving each
    * neighbor entity to a file_path. Surfaces files the recorded reasoning associates
    * with the symbol — a signal a regenerated code-only snapshot can never hold.
    * w_memory<=0 disables it; absent a symbol or an entity graph it is simply empty. */
   int nmem = 0;
   if (w_memory > 0.0 && symbol[0])
   {
      char skey[GRAPH_ENDPOINT_MAX];
      db2_entity_edge_explain_t *eedges = calloc(HYBRID_PER_SIGNAL, sizeof(*eedges));
      if (eedges && db2_entity_node_key_symbol(proj, symbol, skey, sizeof(skey)) == 0)
      {
         int ne2 = db2_entity_edge_explain_by_entity(skey, eedges, HYBRID_PER_SIGNAL);
         for (int i = 0; i < ne2 && nmem < HYBRID_PER_SIGNAL; i++)
         {
            /* the neighbor is whichever endpoint isn't the seed symbol */
            const char *neighbor =
                strcmp(eedges[i].source, skey) == 0 ? eedges[i].target : eedges[i].source;
            if (strcmp(neighbor, skey) == 0)
               continue; /* self-edge: the symbol isn't its own memory neighbor */
            db2_entity_node_t node;
            if (db2_entity_node_get(neighbor, &node) != 0 || !node.file_path[0])
               continue;
            int dup = 0; /* keep the best-ranked row per file (edges arrive weight-desc) */
            for (int j = 0; j < nmem; j++)
               if (strcmp(memory_items[j].id, node.file_path) == 0)
               {
                  dup = 1;
                  break;
               }
            if (dup)
               continue;
            snprintf(memory_items[nmem].id, sizeof(memory_items[nmem].id), "%s", node.file_path);
            memory_items[nmem].structural_weight = eedges[i].structural_weight;
            nmem++;
         }
      }
      free(eedges);
   }

   kb_rrf_signal_t sigs[4] = {
       {code_items, nc, w_code, "code"},
       {graph_items, ng, w_graph, "graph"},
       {vector_items, nv, w_vector, "vector"},
       {memory_items, nmem, w_memory, "memory"},
   };
   int nf = kb_rrf_fuse(sigs, 4, rrf_k, fused, HYBRID_PER_SIGNAL * 4);
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
      free(vector_items);
      free(memory_items);
      free(vpaths);
      free(vscores);
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
      for (int j = 0; j < nv; j++)
         if (strcmp(vpaths[j], fp) == 0)
         {
            if (which)
               cJSON_AddItemToArray(which, cJSON_CreateString("vector"));
            cJSON_AddNumberToObject(row, "vector_score", vscores[j]);
            break;
         }
      for (int j = 0; j < nmem; j++)
         if (strcmp(memory_items[j].id, fp) == 0)
         {
            if (which)
               cJSON_AddItemToArray(which, cJSON_CreateString("memory"));
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
   free(vector_items);
   free(memory_items);
   free(vpaths);
   free(vscores);
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

/* GET /v1/code/graph?project=<proj>&node=<node>&max_results=N
 * Read-only node-neighborhood projection (proposal §8): the incident projection
 * edges of `node` — its callers/callees/containers/etc — each with the relation,
 * direction (out = node→neighbor, in = neighbor→node, self = recursive edge),
 * structural-trust weight, and the §3 provenance tag. Backs the webchat graph
 * view; not on the agent hot path. Reuses db2_code_projection_list_edges (the
 * published generation's edges, capped at HUBS_MAX_EDGES) and
 * kb_graph_edge_provenance. Emits `neighbor_count` (rows returned), `match_count`
 * (total incident, pre-cap), and `truncated` (page cap hit OR scan window full). */
int handle_get_code_graph(const char *query_string, char *out_buf, int out_cap)
{
   char project[256] = "";
   char node[512] = "";
   if (!code_qparam(query_string, "project", project, sizeof(project)) || !project[0])
      return code_scan_write_error(out_buf, out_cap, "missing project");
   if (!code_qparam(query_string, "node", node, sizeof(node)) || !node[0])
      return code_scan_write_error(out_buf, out_cap, "missing node");

   int max_r = 50;
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
   if (!edges)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }

   int ne = db2_code_projection_list_edges(project, edges, HUBS_MAX_EDGES);
   if (ne < 0)
   {
      free(edges);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"projection graph unavailable (knowledge service not initialized)\"}");
      return 503;
   }

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(edges);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "project", project);
   cJSON_AddStringToObject(resp, "node", node);
   cJSON *arr = cJSON_AddArrayToObject(resp, "neighbors");
   int emitted = 0; /* edges actually written to the array (after the max_r cap) */
   int matched = 0; /* edges incident to `node`, counted before the cap */
   /* Scan the whole edge window so `matched` reflects the node's true incident
    * count even once `emitted` has hit max_r; that lets `truncated` distinguish a
    * page cap from a complete neighborhood. */
   for (int i = 0; i < ne; i++)
   {
      const char *dir = NULL, *neighbor = NULL;
      int is_source = edges[i].source[0] && strcmp(edges[i].source, node) == 0;
      int is_target = edges[i].target[0] && strcmp(edges[i].target, node) == 0;
      if (is_source && is_target)
      {
         dir = "self"; /* recursive / self-referential edge (e.g. a self-call) */
         neighbor = node;
      }
      else if (is_source)
      {
         dir = "out"; /* node --relation--> neighbor */
         neighbor = edges[i].target;
      }
      else if (is_target)
      {
         dir = "in"; /* neighbor --relation--> node (e.g. callers) */
         neighbor = edges[i].source;
      }
      if (!dir || !neighbor || !neighbor[0])
         continue;
      matched++;
      if (!arr || emitted >= max_r)
         continue; /* counted toward `matched`/truncation, but past the page cap */
      cJSON *n = cJSON_CreateObject();
      if (!n)
         continue;
      cJSON_AddStringToObject(n, "neighbor", neighbor);
      cJSON_AddStringToObject(n, "relation", edges[i].relation);
      cJSON_AddStringToObject(n, "direction", dir);
      cJSON_AddNumberToObject(n, "structural_weight", edges[i].structural_weight);
      cJSON_AddStringToObject(
          n, "provenance", kb_graph_edge_provenance("code_projection", edges[i].structural_weight));
      cJSON_AddItemToArray(arr, n);
      emitted++;
   }
   cJSON_AddNumberToObject(resp, "neighbor_count", emitted);
   cJSON_AddNumberToObject(resp, "match_count", matched);
   /* Two independent truncation sources: the per-request page cap (matched > emitted)
    * and the projection scan window — a full edge buffer (ne >= HUBS_MAX_EDGES) means
    * the scan covered only a deterministic prefix, so even `matched` may undercount. */
   cJSON_AddBoolToObject(resp, "truncated", matched > emitted || ne >= HUBS_MAX_EDGES);
   free(edges);

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
   return status;
}

int handle_get_code_graph_route(const char *method, const char *query_string, char *out_buf,
                                int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_graph(query_string, out_buf, out_cap);
}

/* GET /v1/code/graph/surprising?project&max_results&k&d_min&percentile&min_cosine
 * §4 surprising links: file-node pairs that are semantically CLOSE (high embedding
 * cosine) yet structurally FAR (graph hop-distance >= d_min, or disconnected) — the
 * "this module reinvented that one" signal. Gathers candidate high-similarity pairs
 * from code_embeddings (pgvec self-kNN; a row's node_key === the projection file-node
 * key, so pairs join the graph directly), then filters them against the projection
 * graph with the pure `kb_graph_surprising` (data-driven cosine percentile + hop
 * distance). Read-only analytics, off the agent hot path. With `judge=true` the top
 * candidates additionally go through the §4 relevance gate — a shared-symbol
 * cross-check plus ONE batched Tier-B LLM-judge call (kb_surprising_judge) that
 * confirms genuine parallel/duplicated logic vs coincidental similarity; without a
 * configured LLM that degrades to unconfirmed structural candidates. */
#define SURPRISING_MAX_PAIRS 1000
/* Bound on anchor rows the self-kNN probes, so cost is O(anchors*k) HNSW probes
 * regardless of project size; a huge project is sampled (and flagged truncated). */
#define SURPRISING_MAX_ANCHORS 5000
/* Cap on links sent to the LLM judge in one batch (bounds the prompt + latency). */
#define SURPRISING_JUDGE_MAX 12
/* §4 precision self-suppress: don't act on the judge-sampled precision until this many
 * candidates have been judged; halve the rolling (judged,confirmed) counters once they
 * exceed the cap so the metric tracks RECENT precision, not all-time. */
#define SURPRISING_PRECISION_MIN_SAMPLES 20
#define SURPRISING_PRECISION_DECAY_CAP   500

/* Read the rolling per-project judge stats (count judged + count confirmed) used to
 * sample the structural generator's precision. Absent keys -> 0. */
static void surprising_stats_get(const char *project, int *judged, int *confirmed)
{
   char key[320], val[64];
   *judged = 0;
   *confirmed = 0;
   snprintf(key, sizeof(key), "surprising_judged:%s", project);
   if (db2_kb_runtime_state_get(key, val, sizeof(val)) == 0)
      *judged = atoi(val);
   snprintf(key, sizeof(key), "surprising_confirmed:%s", project);
   if (db2_kb_runtime_state_get(key, val, sizeof(val)) == 0)
      *confirmed = atoi(val);
}

/* Accumulate this request's (judged, confirmed) into the rolling stats, decaying when
 * the sample grows past the cap so older precision fades. Best-effort. */
static void surprising_stats_add(const char *project, int dj, int dc)
{
   if (dj <= 0)
      return;
   int judged = 0, confirmed = 0;
   surprising_stats_get(project, &judged, &confirmed);
   judged += dj;
   confirmed += dc;
   if (judged > SURPRISING_PRECISION_DECAY_CAP)
   {
      judged /= 2;
      confirmed /= 2;
   }
   char key[320], val[32];
   snprintf(key, sizeof(key), "surprising_judged:%s", project);
   snprintf(val, sizeof(val), "%d", judged);
   db2_kb_runtime_state_set(key, val);
   snprintf(key, sizeof(key), "surprising_confirmed:%s", project);
   snprintf(val, sizeof(val), "%d", confirmed);
   db2_kb_runtime_state_set(key, val);
}

int handle_get_code_graph_surprising(const char *query_string, char *out_buf, int out_cap)
{
   char project[256] = "";
   if (!code_qparam(query_string, "project", project, sizeof(project)) || !project[0])
      return code_scan_write_error(out_buf, out_cap, "missing project");

   char tmp[32] = "";
   int max_r = 20;
   if (code_qparam(query_string, "max_results", tmp, sizeof(tmp)))
      max_r = atoi(tmp);
   if (max_r < 1)
      max_r = 1;
   if (max_r > 200)
      max_r = 200;

   int k = 5; /* nearest neighbors gathered per file node */
   if (code_qparam(query_string, "k", tmp, sizeof(tmp)))
      k = atoi(tmp);
   if (k < 1)
      k = 1;
   if (k > 50)
      k = 50;

   int d_min = 4; /* min structural hop distance to count as "far" */
   if (code_qparam(query_string, "d_min", tmp, sizeof(tmp)))
      d_min = atoi(tmp);
   if (d_min < 1)
      d_min = 1;

   double percentile = 0.9; /* data-driven cosine floor: top decile of candidates */
   if (code_qparam(query_string, "percentile", tmp, sizeof(tmp)))
      percentile = atof(tmp);
   if (percentile < 0.0)
      percentile = 0.0;
   if (percentile > 1.0)
      percentile = 1.0;

   double min_cosine = 0.5; /* prefilter floor on the SQL gather (relevance gate) */
   if (code_qparam(query_string, "min_cosine", tmp, sizeof(tmp)))
      min_cosine = atof(tmp);
   if (min_cosine < 0.0)
      min_cosine = 0.0;
   if (min_cosine > 1.0)
      min_cosine = 1.0;

   int judge = 0; /* opt-in LLM confirmation of the top candidates */
   if (code_qparam(query_string, "judge", tmp, sizeof(tmp)))
      judge = (strcmp(tmp, "true") == 0 || strcmp(tmp, "1") == 0);

   if (!db2_is_initialized())
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"knowledge service not initialized\"}");
      return 503;
   }

   code_projection_edge_t *edges = calloc(HUBS_MAX_EDGES, sizeof(*edges));
   kb_graph_edge_t *gedges = calloc(HUBS_MAX_EDGES, sizeof(*gedges));
   char *a_keys = calloc(SURPRISING_MAX_PAIRS, KB_GRAPH_NODE_MAX);
   char *b_keys = calloc(SURPRISING_MAX_PAIRS, KB_GRAPH_NODE_MAX);
   double *cosines = calloc(SURPRISING_MAX_PAIRS, sizeof(*cosines));
   kb_graph_pair_t *pairs = calloc(SURPRISING_MAX_PAIRS, sizeof(*pairs));
   /* One extra slot: ask kb_graph_surprising for max_r+1 so a return of >max_r
    * tells us the result was page-capped (we still emit only max_r) — precise
    * truncation, vs. over-reporting whenever exactly max_r qualify. */
   kb_graph_surprising_t *out = calloc((size_t)max_r + 1, sizeof(*out));
   if (!edges || !gedges || !a_keys || !b_keys || !cosines || !pairs || !out)
   {
      free(edges);
      free(gedges);
      free(a_keys);
      free(b_keys);
      free(cosines);
      free(pairs);
      free(out);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }

   int ne = db2_code_projection_list_edges(project, edges, HUBS_MAX_EDGES);
   if (ne < 0)
   {
      free(edges);
      free(gedges);
      free(a_keys);
      free(b_keys);
      free(cosines);
      free(pairs);
      free(out);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"projection graph unavailable (knowledge service not initialized)\"}");
      return 503;
   }
   /* Build the distance graph WITHOUT the project containment super-hub. The
    * projection has a `project --contains--> <every file>` edge, so over the full
    * graph any two same-project files are exactly 2 hops apart (file ← project →
    * file) — the hop-distance signal would be meaningless. Dropping project-incident
    * edges makes distance reflect real code coupling (file → symbol → … → file), so
    * a semantically-close pair that is structurally far/disconnected actually shows. */
   int ng = 0;
   for (int i = 0; i < ne; i++)
   {
      if (strncmp(edges[i].source, "project:", 8) == 0 ||
          strncmp(edges[i].target, "project:", 8) == 0)
         continue;
      snprintf(gedges[ng].source, sizeof(gedges[ng].source), "%s", edges[i].source);
      snprintf(gedges[ng].target, sizeof(gedges[ng].target), "%s", edges[i].target);
      gedges[ng].weight = edges[i].structural_weight;
      ng++;
   }
   free(edges);
   edges = NULL;

   /* np == 0 is a legitimate empty (project not embedded yet, or no candidate
    * pairs); np < 0 is a genuine vector-store failure (no connection / query error)
    * and must NOT masquerade as "no surprising links" — surface it as a 503 so
    * callers/alerting can tell an outage from an empty result. */
   int np = pgvec_code_similar_pairs(project, k, min_cosine, SURPRISING_MAX_ANCHORS, a_keys, b_keys,
                                     KB_GRAPH_NODE_MAX, cosines, SURPRISING_MAX_PAIRS);
   if (np < 0)
   {
      free(gedges);
      free(a_keys);
      free(b_keys);
      free(cosines);
      free(pairs);
      free(out);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"vector store unavailable\"}");
      return 503;
   }
   for (int i = 0; i < np; i++)
   {
      snprintf(pairs[i].a, sizeof(pairs[i].a), "%s", a_keys + (size_t)i * KB_GRAPH_NODE_MAX);
      snprintf(pairs[i].b, sizeof(pairs[i].b), "%s", b_keys + (size_t)i * KB_GRAPH_NODE_MAX);
      pairs[i].cosine = cosines[i];
   }
   free(a_keys);
   free(b_keys);
   free(cosines);
   a_keys = b_keys = NULL;
   cosines = NULL;

   int ns = kb_graph_surprising(gedges, ng, pairs, np, percentile, d_min, out, max_r + 1);
   if (ns < 0)
      ns = 0;
   int links_truncated = ns > max_r; /* the +1 probe overflowed -> more qualify */
   if (ns > max_r)
      ns = max_r;
   free(gedges);
   free(pairs);
   gedges = NULL;
   pairs = NULL;

   /* §4 precision self-suppress + rolling judge stats. The LLM judge samples the
    * structural generator's precision (confirmed/judged); when NOT judging, suppress
    * the unjudged candidates if that sampled precision has fallen below the configured
    * floor — they'd be mostly false positives. Floor <= 0 (default) disables it. */
   config_t scfg;
   int have_cfg = (config_load(&scfg) == 0);
   double precision_floor = have_cfg ? scfg.code_surprising_precision_floor : 0.0;
   int stat_judged = 0, stat_confirmed = 0;
   surprising_stats_get(project, &stat_judged, &stat_confirmed);
   int suppressed = 0;
   if (!judge &&
       kb_surprising_precision_suppress(stat_judged, stat_confirmed,
                                        SURPRISING_PRECISION_MIN_SAMPLES, precision_floor))
   {
      suppressed = 1;
      ns = 0; /* historically low-precision structural candidates -> show none */
      links_truncated = 0;
   }

   /* §4 relevance gate (opt-in): confirm the top links with one batched LLM judge.
    * Bounded to SURPRISING_JUDGE_MAX (prompt/latency). Degrades to unconfirmed if no
    * Tier-B LLM is configured (judged stays 0). The verdicts also feed the precision
    * sample above for future requests. */
   kb_surprising_verdict_t *verdicts = NULL;
   int jn = 0, judged_n = 0, confirmed_n = 0;
   if (judge && ns > 0 && have_cfg)
   {
      jn = ns < SURPRISING_JUDGE_MAX ? ns : SURPRISING_JUDGE_MAX;
      verdicts = calloc((size_t)jn, sizeof(*verdicts));
      if (verdicts)
      {
         char jerr[256] = "";
         int r = kb_surprising_judge(&scfg, scfg.kb_curator_judge_command, project, out, jn,
                                     verdicts, jerr, sizeof(jerr));
         if (r > 0)
         {
            judged_n = r;
            for (int i = 0; i < jn; i++)
               if (verdicts[i].judged && verdicts[i].confirmed)
                  confirmed_n++;
            surprising_stats_add(project, judged_n, confirmed_n);
         }
      }
   }

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(verdicts);
      free(out);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "project", project);
   cJSON_AddNumberToObject(resp, "candidate_count", np);
   /* edge_count is the COUPLING-graph size (project-hub edges excluded), i.e. the
    * graph hop distance was actually computed over. */
   cJSON_AddNumberToObject(resp, "edge_count", ng);
   cJSON_AddNumberToObject(resp, "d_min", d_min);
   cJSON_AddNumberToObject(resp, "percentile", percentile);
   cJSON *arr = cJSON_AddArrayToObject(resp, "links");
   for (int i = 0; arr && i < ns; i++)
   {
      cJSON *l = cJSON_CreateObject();
      if (!l)
         continue;
      cJSON_AddStringToObject(l, "a", out[i].a);
      cJSON_AddStringToObject(l, "b", out[i].b);
      cJSON_AddNumberToObject(l, "cosine", out[i].cosine);
      /* Stable schema: every link carries BOTH fields. `hops` is the undirected
       * shortest-path distance (-1 = unreachable within the BFS bound), and
       * `disconnected` is the convenience boolean for that case — structurally
       * unrelated yet semantically close is the strongest surprising signal. */
      cJSON_AddNumberToObject(l, "hops", out[i].hops);
      cJSON_AddBoolToObject(l, "disconnected", out[i].hops < 0);
      /* §4 confirmation: shared_symbols only when the pair was actually sent (else 0
       * would conflate "computed none" with "not computed"); confirmed + reason only
       * when the LLM returned a verdict. */
      if (verdicts && i < jn && verdicts[i].sent)
      {
         cJSON_AddNumberToObject(l, "shared_symbols", verdicts[i].shared_symbols);
         if (verdicts[i].judged)
         {
            cJSON_AddBoolToObject(l, "confirmed", verdicts[i].confirmed);
            if (verdicts[i].reason[0])
               cJSON_AddStringToObject(l, "reason", verdicts[i].reason);
         }
      }
      cJSON_AddItemToArray(arr, l);
   }
   free(verdicts);
   cJSON_AddNumberToObject(resp, "link_count", ns);
   /* How many links the LLM judge actually confirmed/rejected (0 = judging off or no
    * Tier-B LLM configured -> links are unconfirmed structural candidates). */
   if (judge)
      cJSON_AddNumberToObject(resp, "judged", judged_n);
   /* §4 self-suppress observability: the judge-sampled structural precision and its
    * sample size; `suppressed` true when an unjudged request returned nothing because
    * that precision is below the floor. */
   if (stat_judged > 0)
      cJSON_AddNumberToObject(resp, "sampled_precision",
                              (double)stat_confirmed / (double)stat_judged);
   cJSON_AddNumberToObject(resp, "judged_samples", stat_judged);
   if (suppressed)
      cJSON_AddBoolToObject(resp, "suppressed", 1);
   /* Truncated if the page cap bound the links (precise: the +1 probe overflowed),
    * or either input scan filled its window (edges or candidate pairs) so the
    * candidate set was itself a prefix. */
   cJSON_AddBoolToObject(resp, "truncated",
                         links_truncated || ne >= HUBS_MAX_EDGES || np >= SURPRISING_MAX_PAIRS);
   free(out);

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
   return status;
}

int handle_get_code_graph_surprising_route(const char *method, const char *query_string,
                                           char *out_buf, int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_graph_surprising(query_string, out_buf, out_cap);
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
   char sha_now[128] = ""; /* §6: default-branch SHA, tracked across the local-scan path */
   char sha_key[320] = "";

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
      /* §6 live idempotency: resolve the default-branch SHA (cheap) so a !force scan
       * can skip the expensive git re-walk when the branch hasn't moved since the last
       * index, and so any successful scan (force or not) refreshes the stored SHA. A
       * future post-merge/fetch hook reuses exactly this gate. Disabled under the
       * worktree opt-in, where the index tracks WIP the branch SHA can't see. */
      if (!code_index_source_is_worktree() &&
          git_resolve_default_sha(root_path, sha_now, sizeof(sha_now)) == 0 && !force)
      {
         char stored[128] = "";
         snprintf(sha_key, sizeof(sha_key), "code_scan_sha:%s", project);
         db2_kb_runtime_state_get(sha_key, stored, sizeof(stored));
         if (!code_default_branch_changed(stored, sha_now))
         {
            snprintf(out_buf, (size_t)out_cap,
                     "{\"status\":\"ok\",\"skipped\":true,\"reason\":\"default branch unchanged\","
                     "\"project\":\"%s\",\"files\":0,\"inspected\":0}",
                     project);
            cJSON_Delete(root);
            return 200;
         }
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

   /* Record the scanned default-branch SHA so the next !force scan can no-op when the
    * branch hasn't moved (sha_now is set only on the local-scan path that resolved it). */
   if (sha_now[0])
   {
      snprintf(sha_key, sizeof(sha_key), "code_scan_sha:%s", project);
      db2_kb_runtime_state_set(sha_key, sha_now);
   }

   /* §6 live (opt-in): install the post-merge reindex hook so future pulls keep the
    * graph fresh. Only for a resolved git repo; best-effort — a foreign hook (-2) or
    * any failure just omits the field, never failing the scan. */
   int hook_installed = 0;
   if (sha_now[0] && code_scan_bool(root, "install_hook", 0))
      hook_installed = (code_index_install_branch_hook(root_path, project) == 0);

   snprintf(out_buf, (size_t)out_cap,
            "{\"status\":\"ok\",\"skipped\":false,\"project\":\"%s\",\"files\":%d,"
            "\"inspected\":%d,\"hook_installed\":%s}",
            project, files, inspected, hook_installed ? "true" : "false");
   cJSON_Delete(root);
   return 200;
}

int handle_post_code_scan_route(const char *method, const char *body, char *out_buf, int out_cap)
{
   if (strcmp(method, "POST") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_post_code_scan(body, out_buf, out_cap);
}

/* S7: POST /v1/code/repo-trust {project, trust:"trusted"|"untrusted", actor?,
 * request_id?}. `owner` (caller holds the unscoped owner credential) is required:
 * a scoped token must not be able to flip trust. Applies the audited db2 trust
 * write and, on a real transition, recomputes the blocked_symbols frequency model
 * (best-effort — the trust write has already committed and bumped trust_epoch,
 * which invalidates cached results either way). */
int handle_post_code_repo_trust(const char *body, char *out_buf, int out_cap, int owner)
{
   if (!owner)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"forbidden: repo trust requires the owner credential\"}");
      return 403;
   }
   cJSON *root = cJSON_Parse(body ? body : "{}");
   if (!root)
      return code_scan_write_error(out_buf, out_cap, "invalid json");

   char project[256] = "", trust[16] = "", actor[256] = "", request_id[128] = "";
   cJSON *j;
   if ((j = cJSON_GetObjectItemCaseSensitive(root, "project")) && cJSON_IsString(j))
      snprintf(project, sizeof(project), "%s", j->valuestring);
   if ((j = cJSON_GetObjectItemCaseSensitive(root, "trust")) && cJSON_IsString(j))
      snprintf(trust, sizeof(trust), "%s", j->valuestring);
   if ((j = cJSON_GetObjectItemCaseSensitive(root, "actor")) && cJSON_IsString(j))
      snprintf(actor, sizeof(actor), "%s", j->valuestring);
   if ((j = cJSON_GetObjectItemCaseSensitive(root, "request_id")) && cJSON_IsString(j))
      snprintf(request_id, sizeof(request_id), "%s", j->valuestring);
   cJSON_Delete(root);

   if (!project[0])
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"missing project\"}");
      return 400;
   }
   if (strcmp(trust, "trusted") != 0 && strcmp(trust, "untrusted") != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"trust must be 'trusted' or 'untrusted'\"}");
      return 400;
   }
   if (!db2_is_initialized())
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"failed to open knowledge service store\"}");
      return 503;
   }

   char prior[16] = "";
   int changed = 0;
   int rc =
       db2_cross_repo_set_trust(project, trust, actor, request_id, prior, sizeof(prior), &changed);
   if (rc == 1)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"no such project\"}");
      return 404;
   }
   if (rc != 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"trust write failed\"}");
      return 503;
   }

   int recomputed = -1; /* -1 = not attempted (no change); >=0 rows; -1 on a recompute error too */
   if (changed)
   {
      config_t cfg;
      if (config_load(&cfg) == 0)
         recomputed = db2_cross_repo_recompute_blocked_symbols(cfg.kb_curator_cross_repo_k,
                                                               cfg.kb_curator_cross_repo_m,
                                                               cfg.kb_curator_cross_repo_len_min);
   }

   /* Build via cJSON so the (owner-supplied) project name is JSON-escaped rather
    * than interpolated raw into a template. */
   cJSON *out = cJSON_CreateObject();
   if (!out)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"out of memory\"}");
      return 500;
   }
   cJSON_AddStringToObject(out, "status", "ok");
   cJSON_AddStringToObject(out, "project", project);
   cJSON_AddStringToObject(out, "prior_trust", prior);
   cJSON_AddStringToObject(out, "new_trust", trust);
   cJSON_AddBoolToObject(out, "changed", changed ? 1 : 0);
   cJSON_AddNumberToObject(out, "blocked_symbols_recomputed", recomputed);
   char *s = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   if (!s)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"out of memory\"}");
      return 500;
   }
   int over = ((int)strlen(s) >= out_cap);
   if (!over)
      snprintf(out_buf, (size_t)out_cap, "%s", s);
   free(s);
   if (over)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"response too large\"}");
      return 500;
   }
   return 200;
}

int handle_post_code_repo_trust_route(const char *method, const char *body, char *out_buf,
                                      int out_cap, int owner)
{
   if (strcmp(method, "POST") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_post_code_repo_trust(body, out_buf, out_cap, owner);
}
