#include "kb_http_code.h"
#include "aimee.h"
#include "config.h"
#include "kb_curator_queue.h"
#include "cJSON.h"
#include "db2/canonical_index.h"
#include "db2/lifecycle.h"
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
   int n = canonical_index_code_search(query, project[0] ? project : NULL, hits, max_r);
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
