/* kb_client_index.c: thin client wrappers for the aimee-kb /v1 code-index API */
#include "kb_client.h"
#include "kb_client_internal.h"
#include "code_collect.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scan timeout is generous because canonical scans of large monorepos can take
 * tens of seconds. The shared v1 helpers choose remote HTTP when configured and
 * otherwise tunnel the same /v1 route over the local UDS transport. */
#define KB_CLIENT_INDEX_SCAN_TIMEOUT_MS (5 * 60 * 1000)
#define KB_CLIENT_INDEX_READ_TIMEOUT_MS (5 * 1000)

static int kb_index_find_parse(cJSON *resp, term_hit_t *out, int max)
{
   if (!resp)
      return 0;
   int count = 0;
   cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
   if (cJSON_IsArray(hits))
   {
      cJSON *h;
      cJSON_ArrayForEach(h, hits)
      {
         if (count >= max)
            break;
         cJSON *p = cJSON_GetObjectItemCaseSensitive(h, "project");
         cJSON *f = cJSON_GetObjectItemCaseSensitive(h, "file_path");
         cJSON *l = cJSON_GetObjectItemCaseSensitive(h, "line");
         cJSON *k = cJSON_GetObjectItemCaseSensitive(h, "kind");
         if (cJSON_IsString(p))
            snprintf(out[count].project, sizeof(out[count].project), "%s", p->valuestring);
         if (cJSON_IsString(f))
            snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", f->valuestring);
         out[count].line = cJSON_IsNumber(l) ? (int)l->valuedouble : 0;
         cJSON *le = cJSON_GetObjectItemCaseSensitive(h, "line_end");
         out[count].line_end = cJSON_IsNumber(le) ? (int)le->valuedouble : 0;
         if (cJSON_IsString(k))
            snprintf(out[count].kind, sizeof(out[count].kind), "%s", k->valuestring);
         count++;
      }
   }
   return count;
}

static int kb_index_project_list_parse(cJSON *resp, project_info_t *out, int max)
{
   if (!resp)
      return 0;
   int count = 0;
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(resp, "projects");
   if (cJSON_IsArray(projects))
   {
      cJSON *p;
      cJSON_ArrayForEach(p, projects)
      {
         if (count >= max)
            break;
         cJSON *n = cJSON_GetObjectItemCaseSensitive(p, "name");
         cJSON *r = cJSON_GetObjectItemCaseSensitive(p, "root");
         cJSON *s = cJSON_GetObjectItemCaseSensitive(p, "scanned_at");
         if (cJSON_IsString(n))
            snprintf(out[count].name, sizeof(out[count].name), "%s", n->valuestring);
         if (cJSON_IsString(r))
            snprintf(out[count].root, sizeof(out[count].root), "%s", r->valuestring);
         if (cJSON_IsString(s))
            snprintf(out[count].scanned_at, sizeof(out[count].scanned_at), "%s", s->valuestring);
         count++;
      }
   }
   return count;
}

static int kb_index_code_search_parse(cJSON *resp, code_search_hit_t *out, int max)
{
   if (!resp)
      return 0;
   int count = 0;
   cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
   if (cJSON_IsArray(hits))
   {
      cJSON *h;
      cJSON_ArrayForEach(h, hits)
      {
         if (count >= max)
            break;
         cJSON *p = cJSON_GetObjectItemCaseSensitive(h, "project");
         cJSON *f = cJSON_GetObjectItemCaseSensitive(h, "file_path");
         cJSON *s = cJSON_GetObjectItemCaseSensitive(h, "snippet");
         cJSON *r = cJSON_GetObjectItemCaseSensitive(h, "rank");
         cJSON *ln = cJSON_GetObjectItemCaseSensitive(h, "line");
         if (cJSON_IsString(p))
            snprintf(out[count].project, sizeof(out[count].project), "%s", p->valuestring);
         if (cJSON_IsString(f))
            snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", f->valuestring);
         if (cJSON_IsString(s))
            snprintf(out[count].snippet, sizeof(out[count].snippet), "%s", s->valuestring);
         out[count].rank = cJSON_IsNumber(r) ? r->valuedouble : 0.0;
         /* P1b span enrichment: matched line, present only when the search
          * enriched (absent -> 0). */
         out[count].line = (cJSON_IsNumber(ln) && ln->valueint > 0) ? ln->valueint : 0;
         count++;
      }
   }
   return count;
}

static int kb_index_find_callers_parse(cJSON *resp, caller_hit_t *out, int max)
{
   if (!resp)
      return 0;
   int count = 0;
   cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
   if (cJSON_IsArray(hits))
   {
      cJSON *h;
      cJSON_ArrayForEach(h, hits)
      {
         if (count >= max)
            break;
         cJSON *p = cJSON_GetObjectItemCaseSensitive(h, "project");
         cJSON *f = cJSON_GetObjectItemCaseSensitive(h, "file_path");
         cJSON *cf = cJSON_GetObjectItemCaseSensitive(h, "caller");
         cJSON *l = cJSON_GetObjectItemCaseSensitive(h, "line");
         if (cJSON_IsString(p))
            snprintf(out[count].project, sizeof(out[count].project), "%s", p->valuestring);
         if (cJSON_IsString(f))
            snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", f->valuestring);
         if (cJSON_IsString(cf))
            snprintf(out[count].caller, sizeof(out[count].caller), "%s", cf->valuestring);
         out[count].line = cJSON_IsNumber(l) ? (int)l->valuedouble : 0;
         count++;
      }
   }
   return count;
}

int kb_client_code_scan_push(const char *name, const char *root, int force, void *files_arr_v,
                             kb_client_index_scan_result_t *out)
{
   cJSON *files_arr = (cJSON *)files_arr_v;

   if (!name || !name[0] || !root || !root[0])
   {
      if (files_arr)
         cJSON_Delete(files_arr);
      if (out)
      {
         out->skipped = 1;
         snprintf(out->reason, sizeof(out->reason), "missing_project_root");
         snprintf(out->message, sizeof(out->message),
                  "code index scan requires explicit project and root");
      }
      return -1;
   }

   cJSON *req = cJSON_CreateObject();
   if (!req)
   {
      if (files_arr)
         cJSON_Delete(files_arr);
      return kb_client_index_scan_apply_response(NULL, out);
   }
   cJSON_AddStringToObject(req, "project", name);
   cJSON_AddStringToObject(req, "root_path", root);
   if (force)
      cJSON_AddBoolToObject(req, "force", 1);
   /* Caller-supplied {"rel_path","content"} entries (adopted) let the kb index
    * without filesystem access — used both for a remote kb and for files the
    * thin client pushes for a workspace the server cannot see. */
   if (files_arr)
      cJSON_AddItemToObject(req, "files", files_arr);

   char *json = kb_client_v1_post_json("/v1/code/scan", req, KB_CLIENT_INDEX_SCAN_TIMEOUT_MS, NULL);
   cJSON_Delete(req);
   if (!json)
      return kb_client_index_scan_apply_response(NULL, out);

   cJSON *resp = cJSON_Parse(json);
   free(json);
   int rc = kb_client_index_scan_apply_response(resp, out);
   if (rc == 0 && out && out->projects == 0)
      out->projects = 1;
   if (resp)
      cJSON_Delete(resp);
   return rc;
}

static int kb_client_index_scan_v1(const char *name, const char *root, int force,
                                   kb_client_index_scan_result_t *out)
{
   cJSON *files_arr = NULL;
#ifdef AIMEE_POSIX
   /* When aimee-kb is remote (AIMEE_KB_API_URL set), the container cannot reach
    * the host filesystem via root_path.  Enumerate and push file contents so
    * the handler can index without filesystem access. */
   if (name && name[0] && root && root[0] && kb_client_v1_base_url())
   {
      files_arr = cJSON_CreateArray();
      if (files_arr)
         code_collect_files(root, files_arr);
   }
#endif
   return kb_client_code_scan_push(name, root, force, files_arr, out);
}

int kb_client_index_scan(const char *name, const char *root, int force,
                         kb_client_index_scan_result_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));

   return kb_client_index_scan_v1(name, root, force, out);
}

int kb_client_index_find(const char *identifier, term_hit_t *out, int max)
{
   if (!identifier || !identifier[0] || !out || max <= 0)
      return 0;
   memset(out, 0, sizeof(*out) * (size_t)max);

   char *encoded = kb_client_query_escape(identifier);
   if (!encoded)
      return 0;
   char path[512];
   snprintf(path, sizeof(path), "/v1/code/find?identifier=%s&max_results=%d", encoded, max);
   free(encoded);
   char *json = kb_client_v1_get_json(path, KB_CLIENT_INDEX_READ_TIMEOUT_MS, NULL);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   int count = kb_index_find_parse(resp, out, max);
   cJSON_Delete(resp);
   return count;
}

int kb_client_index_list(project_info_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   memset(out, 0, sizeof(*out) * (size_t)max);

   char path[64];
   snprintf(path, sizeof(path), "/v1/code/projects?max_results=%d", max);
   char *json = kb_client_v1_get_json(path, KB_CLIENT_INDEX_READ_TIMEOUT_MS, NULL);
   if (!json)
      return -1; /* service unavailable — caller can distinguish from empty (0) */
   cJSON *resp = cJSON_Parse(json);
   free(json);
   int count = kb_index_project_list_parse(resp, out, max);
   cJSON_Delete(resp);
   return count;
}

int kb_client_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out)
{
   if (!project || !file_path || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   snprintf(out->file, sizeof(out->file), "%s", file_path);

   char *project_q = kb_client_query_escape(project);
   char *file_q = kb_client_query_escape(file_path);
   if (!project_q || !file_q)
   {
      free(project_q);
      free(file_q);
      return -1;
   }
   char path[MAX_PATH_LEN + 512];
   snprintf(path, sizeof(path), "/v1/code/blast-radius?project=%s&file_path=%s", project_q, file_q);
   free(project_q);
   free(file_q);
   char *json = kb_client_v1_get_json(path, KB_CLIENT_INDEX_READ_TIMEOUT_MS, NULL);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;

   cJSON *file = cJSON_GetObjectItemCaseSensitive(resp, "file");
   if (cJSON_IsString(file))
      snprintf(out->file, sizeof(out->file), "%s", file->valuestring);

   cJSON *deps = cJSON_GetObjectItemCaseSensitive(resp, "dependencies");
   if (cJSON_IsArray(deps))
   {
      cJSON *d;
      cJSON_ArrayForEach(d, deps)
      {
         if (out->dependency_count >= 64)
            break;
         if (cJSON_IsString(d))
            snprintf(out->dependencies[out->dependency_count++], MAX_PATH_LEN, "%s",
                     d->valuestring);
      }
   }
   cJSON *depts = cJSON_GetObjectItemCaseSensitive(resp, "dependents");
   if (cJSON_IsArray(depts))
   {
      cJSON *d;
      cJSON_ArrayForEach(d, depts)
      {
         if (out->dependent_count >= 64)
            break;
         if (cJSON_IsString(d))
            snprintf(out->dependents[out->dependent_count++], MAX_PATH_LEN, "%s", d->valuestring);
      }
   }
   cJSON_Delete(resp);
   return 0;
}

static const char *kb_index_preview_severity(int dependent_count)
{
   if (dependent_count >= 10)
      return "red";
   if (dependent_count >= 3)
      return "yellow";
   return "green";
}

static int kb_index_preview_severity_rank(const char *severity)
{
   if (severity && strcmp(severity, "red") == 0)
      return 2;
   if (severity && strcmp(severity, "yellow") == 0)
      return 1;
   return 0;
}

static char *kb_client_index_blast_radius_preview_v1(const char *project, char **paths,
                                                     int path_count)
{
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;
   cJSON *files = cJSON_AddArrayToObject(root, "files");
   cJSON *warnings = cJSON_CreateArray();
   if (!files || !warnings)
   {
      cJSON_Delete(root);
      cJSON_Delete(warnings);
      return NULL;
   }

   int total_dependents = 0;
   int max_severity = 0;
   int red_count = 0;
   char dirs[20][64];
   int dir_count = 0;

   for (int i = 0; i < path_count; i++)
   {
      const char *path = paths[i] ? paths[i] : "";
      blast_radius_t br;
      int rc = kb_client_index_blast_radius(project, path, &br);

      cJSON *file = cJSON_CreateObject();
      cJSON_AddStringToObject(file, "path", path);
      cJSON *deps = cJSON_AddArrayToObject(file, "dependents");
      int dependent_count = 0;
      if (rc == 0)
      {
         dependent_count = br.dependent_count;
         for (int j = 0; j < br.dependent_count; j++)
            cJSON_AddItemToArray(deps, cJSON_CreateString(br.dependents[j]));
      }
      cJSON_AddNumberToObject(file, "dependent_count", dependent_count);
      const char *severity = kb_index_preview_severity(dependent_count);
      cJSON_AddStringToObject(file, "severity", severity);
      cJSON_AddItemToArray(files, file);

      total_dependents += dependent_count;
      int rank = kb_index_preview_severity_rank(severity);
      if (rank > max_severity)
         max_severity = rank;
      if (rank == 2)
         red_count++;
      if (dependent_count > 10)
      {
         char warn[256];
         snprintf(warn, sizeof(warn), "%s has %d dependents, consider splitting the change", path,
                  dependent_count);
         cJSON_AddItemToArray(warnings, cJSON_CreateString(warn));
      }

      const char *slash = strrchr(path, '/');
      if (slash && dir_count < 20)
      {
         char prefix[64];
         int len = (int)(slash - path);
         if (len > 63)
            len = 63;
         snprintf(prefix, sizeof(prefix), "%.*s", len, path);
         int found = 0;
         for (int d = 0; d < dir_count; d++)
            if (strcmp(dirs[d], prefix) == 0)
               found = 1;
         if (!found)
            snprintf(dirs[dir_count++], sizeof(dirs[0]), "%s", prefix);
      }
   }

   if (red_count > 1)
      cJSON_AddItemToArray(warnings,
                           cJSON_CreateString("Multiple high-impact files in this change set"));
   if (dir_count > 2)
      cJSON_AddItemToArray(
          warnings, cJSON_CreateString("Changes span multiple subsystems, consider separate PRs"));
   cJSON_AddNumberToObject(root, "total_dependents", total_dependents);
   cJSON_AddStringToObject(root, "severity",
                           max_severity == 2   ? "red"
                           : max_severity == 1 ? "yellow"
                                               : "green");
   cJSON_AddItemToObject(root, "warnings", warnings);
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return json ? json : strdup("{}");
}

char *kb_client_index_blast_radius_preview_json(const char *project, char **paths, int path_count)
{
   if (!project || !paths || path_count < 1)
      return NULL;

   return kb_client_index_blast_radius_preview_v1(project, paths, path_count);
}

int kb_client_index_structure(const char *project, const char *file_path, definition_t *out,
                              int max)
{
   if (!project || !file_path || !out || max <= 0)
      return 0;
   memset(out, 0, sizeof(*out) * (size_t)max);

   char *project_q = kb_client_query_escape(project);
   char *file_q = kb_client_query_escape(file_path);
   if (!project_q || !file_q)
   {
      free(project_q);
      free(file_q);
      return 0;
   }
   char path[MAX_PATH_LEN + 512];
   snprintf(path, sizeof(path), "/v1/code/structure?project=%s&file_path=%s&max_results=%d",
            project_q, file_q, max);
   free(project_q);
   free(file_q);
   char *json = kb_client_v1_get_json(path, KB_CLIENT_INDEX_READ_TIMEOUT_MS, NULL);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return 0;

   int count = 0;
   cJSON *defs = cJSON_GetObjectItemCaseSensitive(resp, "definitions");
   if (cJSON_IsArray(defs))
   {
      cJSON *d;
      cJSON_ArrayForEach(d, defs)
      {
         if (count >= max)
            break;
         cJSON *name = cJSON_GetObjectItemCaseSensitive(d, "name");
         cJSON *kind = cJSON_GetObjectItemCaseSensitive(d, "kind");
         cJSON *line = cJSON_GetObjectItemCaseSensitive(d, "line");
         if (cJSON_IsString(name))
            snprintf(out[count].name, sizeof(out[count].name), "%s", name->valuestring);
         if (cJSON_IsString(kind))
            snprintf(out[count].kind, sizeof(out[count].kind), "%s", kind->valuestring);
         out[count].line = cJSON_IsNumber(line) ? (int)line->valuedouble : 0;
         cJSON *line_end = cJSON_GetObjectItemCaseSensitive(d, "line_end");
         out[count].line_end = cJSON_IsNumber(line_end) ? (int)line_end->valuedouble : 0;
         count++;
      }
   }
   cJSON_Delete(resp);
   return count;
}

int kb_client_index_project_stats(const char *project, int *files_out, int *defs_out)
{
   if (files_out)
      *files_out = 0;
   if (defs_out)
      *defs_out = 0;
   if (!project || !project[0])
      return -1;

   {
      char *project_q = kb_client_query_escape(project);
      if (!project_q)
         return -1;
      char path[512];
      snprintf(path, sizeof(path), "/v1/code/project-stats?project=%s", project_q);
      free(project_q);
      char *json = kb_client_v1_get_json(path, KB_CLIENT_INDEX_READ_TIMEOUT_MS, NULL);
      if (!json)
         return -1;
      cJSON *resp = cJSON_Parse(json);
      free(json);
      if (!resp)
         return -1;

      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      int rc = -1;
      if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0)
      {
         cJSON *f = cJSON_GetObjectItemCaseSensitive(resp, "files");
         cJSON *d = cJSON_GetObjectItemCaseSensitive(resp, "definitions");
         if (files_out && cJSON_IsNumber(f))
            *files_out = (int)f->valuedouble;
         if (defs_out && cJSON_IsNumber(d))
            *defs_out = (int)d->valuedouble;
         rc = 0;
      }
      cJSON_Delete(resp);
      return rc;
   }
}

int kb_client_index_project_lang(const char *project, char *buf, size_t bufsz)
{
   if (!buf || bufsz < 3)
      return -1;
   buf[0] = '[';
   buf[1] = ']';
   buf[2] = '\0';
   if (!project || !project[0])
      return -1;

   {
      char *project_q = kb_client_query_escape(project);
      if (!project_q)
         return -1;
      char path[512];
      snprintf(path, sizeof(path), "/v1/code/project-stats?project=%s", project_q);
      free(project_q);
      char *json = kb_client_v1_get_json(path, KB_CLIENT_INDEX_READ_TIMEOUT_MS, NULL);
      if (!json)
         return -1;
      cJSON *resp = cJSON_Parse(json);
      free(json);
      if (!resp)
         return -1;

      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      int rc = -1;
      if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0)
      {
         cJSON *langs = cJSON_GetObjectItemCaseSensitive(resp, "langs");
         if (cJSON_IsArray(langs))
         {
            char *printed = cJSON_PrintUnformatted(langs);
            if (printed)
            {
               snprintf(buf, bufsz, "%s", printed);
               free(printed);
            }
         }
         rc = 0;
      }
      cJSON_Delete(resp);
      return rc;
   }
}

int kb_client_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return 0;
   memset(out, 0, sizeof(*out) * (size_t)max);

   char *query_q = kb_client_query_escape(query);
   char *project_q = (project && project[0]) ? kb_client_query_escape(project) : NULL;
   if (!query_q || ((project && project[0]) && !project_q))
   {
      free(query_q);
      free(project_q);
      return 0;
   }
   size_t path_len = strlen("/v1/code/search?query=&max_results=&project=") + strlen(query_q) +
                     (project_q ? strlen(project_q) : 0) + 32;
   char *path = malloc(path_len);
   if (!path)
   {
      free(query_q);
      free(project_q);
      return 0;
   }
   snprintf(path, path_len, "/v1/code/search?query=%s&max_results=%d%s%s", query_q, max,
            project_q ? "&project=" : "", project_q ? project_q : "");
   free(query_q);
   free(project_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_INDEX_READ_TIMEOUT_MS, NULL);
   free(path);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   int count = kb_index_code_search_parse(resp, out, max);
   cJSON_Delete(resp);
   return count;
}

int kb_client_index_find_callers(const char *project, const char *symbol, caller_hit_t *out,
                                 int max)
{
   if (!symbol || !symbol[0] || !out || max <= 0)
      return 0;
   memset(out, 0, sizeof(*out) * (size_t)max);

   char *symbol_q = kb_client_query_escape(symbol);
   char *project_q = (project && project[0]) ? kb_client_query_escape(project) : NULL;
   if (!symbol_q || ((project && project[0]) && !project_q))
   {
      free(symbol_q);
      free(project_q);
      return 0;
   }
   size_t path_len = strlen("/v1/code/callers?symbol=&max_results=&project=") + strlen(symbol_q) +
                     (project_q ? strlen(project_q) : 0) + 32;
   char *path = malloc(path_len);
   if (!path)
   {
      free(symbol_q);
      free(project_q);
      return 0;
   }
   snprintf(path, path_len, "/v1/code/callers?symbol=%s&max_results=%d%s%s", symbol_q, max,
            project_q ? "&project=" : "", project_q ? project_q : "");
   free(symbol_q);
   free(project_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_INDEX_READ_TIMEOUT_MS, NULL);
   free(path);
   if (!json)
      return 0;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   int count = kb_index_find_callers_parse(resp, out, max);
   cJSON_Delete(resp);
   return count;
}

/* S6: cross-repo dependency proxy. The kb response is rich (per-edge evidence +
 * version stamp, or a separate AMBIGUOUS review-queue shape when status=ambiguous)
 * so we forward the raw kb body verbatim rather than flatten it into a struct —
 * same passthrough idiom as kb_client_index_blast_radius_preview_json. Caller frees
 * the returned string; NULL means the kb was unreachable or returned no body. */
char *kb_client_index_cross_repo_deps_json(const char *project, const char *direction,
                                           const char *min_tier, int status_ambiguous, int dry_run)
{
   if (!project || !project[0])
      return NULL;

   char *project_q = kb_client_query_escape(project);
   char *direction_q = (direction && direction[0]) ? kb_client_query_escape(direction) : NULL;
   char *min_tier_q = (min_tier && min_tier[0]) ? kb_client_query_escape(min_tier) : NULL;
   if (!project_q || ((direction && direction[0]) && !direction_q) ||
       ((min_tier && min_tier[0]) && !min_tier_q))
   {
      free(project_q);
      free(direction_q);
      free(min_tier_q);
      return NULL;
   }

   size_t path_len =
       strlen("/v1/code/cross-repo-deps?project=&direction=&min_tier=&status=ambiguous&dry_run=1") +
       strlen(project_q) + (direction_q ? strlen(direction_q) : 0) +
       (min_tier_q ? strlen(min_tier_q) : 0) + 8;
   char *path = malloc(path_len);
   if (!path)
   {
      free(project_q);
      free(direction_q);
      free(min_tier_q);
      return NULL;
   }
   snprintf(path, path_len, "/v1/code/cross-repo-deps?project=%s%s%s%s%s%s%s", project_q,
            direction_q ? "&direction=" : "", direction_q ? direction_q : "",
            min_tier_q ? "&min_tier=" : "", min_tier_q ? min_tier_q : "",
            status_ambiguous ? "&status=ambiguous" : "", dry_run ? "&dry_run=1" : "");
   free(project_q);
   free(direction_q);
   free(min_tier_q);

   char *json = kb_client_v1_get_json(path, KB_CLIENT_INDEX_READ_TIMEOUT_MS, NULL);
   free(path);
   return json;
}

/* S7: POST the cross-repo trust write to the kb. Returns the raw kb JSON body on
 * 2xx (caller frees); NULL on any non-2xx or transport failure with *http_status
 * (optional) set to the kb HTTP status (so the proxy can map 404/403/400 to a
 * precise client error instead of a blanket 502). */
char *kb_client_repo_trust_json(const char *project, const char *trust, const char *actor,
                                const char *request_id, int *http_status)
{
   if (http_status)
      *http_status = -1;
   if (!project || !project[0] || !trust || !trust[0])
      return NULL;
   cJSON *body = cJSON_CreateObject();
   if (!body)
      return NULL;
   cJSON_AddStringToObject(body, "project", project);
   cJSON_AddStringToObject(body, "trust", trust);
   if (actor && actor[0])
      cJSON_AddStringToObject(body, "actor", actor);
   if (request_id && request_id[0])
      cJSON_AddStringToObject(body, "request_id", request_id);
   char *json = kb_client_v1_post_json("/v1/code/repo-trust", body, KB_CLIENT_INDEX_READ_TIMEOUT_MS,
                                       http_status);
   cJSON_Delete(body);
   return json;
}
