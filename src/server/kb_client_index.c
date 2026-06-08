/* kb_client_index.c: thin client wrappers for the aimee-kb /v1 code-index API */
#include "kb_client.h"
#include "kb_client_internal.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef AIMEE_POSIX
#include <dirent.h>
#include <sys/stat.h>
#endif

/* Scan timeout is generous because canonical scans of large monorepos can take
 * tens of seconds. The shared v1 helpers choose remote HTTP when configured and
 * otherwise tunnel the same /v1 route over the local UDS transport. */
#define KB_CLIENT_INDEX_SCAN_TIMEOUT_MS (5 * 60 * 1000)
#define KB_CLIENT_INDEX_READ_TIMEOUT_MS (5 * 1000)

#ifdef AIMEE_POSIX
/* Limits for remote push-based scan: keep request sizes sane. */
#define KB_INDEX_PUSH_MAX_FILES      4096
#define KB_INDEX_PUSH_MAX_FILE_BYTES (256 * 1024)

static int kb_index_ext_ok(const char *name)
{
   static const char *const exts[] = {".c",    ".h",   ".cc",    ".cpp",  ".cxx", ".m",    ".py",
                                      ".go",   ".rs",  ".ts",    ".tsx",  ".js",  ".jsx",  ".md",
                                      ".yaml", ".yml", ".toml",  ".json", ".sh",  ".bash", ".rb",
                                      ".java", ".kt",  ".swift", NULL};
   const char *dot = strrchr(name, '.');
   if (!dot || dot == name)
      return 0;
   for (int i = 0; exts[i]; i++)
      if (strcmp(dot, exts[i]) == 0)
         return 1;
   return 0;
}

static int kb_index_dir_skip(const char *name)
{
   static const char *const skip[] = {"node_modules", "__pycache__", "vendor", "target",
                                      "build",        "dist",        "out",    NULL};
   if (name[0] == '.') /* .git, .aimee, .cache, .svn, hidden dirs */
      return 1;
   for (int i = 0; skip[i]; i++)
      if (strcmp(name, skip[i]) == 0)
         return 1;
   return 0;
}

/* Walk root/rel recursively, appending {"rel_path","content"} entries to
 * files_arr.  Stops at KB_INDEX_PUSH_MAX_FILES.  Returns 0 always; errors on
 * individual files are silently skipped (best-effort collection). */
static void kb_index_collect_files(const char *root, const char *rel, cJSON *files_arr, int *count)
{
   char path[4096];
   if (rel && rel[0])
      snprintf(path, sizeof(path), "%s/%s", root, rel);
   else
      snprintf(path, sizeof(path), "%s", root);

   DIR *dir = opendir(path);
   if (!dir)
      return;

   struct dirent *ent;
   while (*count < KB_INDEX_PUSH_MAX_FILES && (ent = readdir(dir)) != NULL)
   {
      if (ent->d_name[0] == '.' &&
          (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
         continue; /* skip . and .. */

      char full[4096];
      snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

      struct stat st;
      if (stat(full, &st) != 0)
         continue;

      char rel_child[4096];
      if (rel && rel[0])
         snprintf(rel_child, sizeof(rel_child), "%s/%s", rel, ent->d_name);
      else
         snprintf(rel_child, sizeof(rel_child), "%s", ent->d_name);

      if (S_ISDIR(st.st_mode))
      {
         if (!kb_index_dir_skip(ent->d_name))
            kb_index_collect_files(root, rel_child, files_arr, count);
      }
      else if (S_ISREG(st.st_mode))
      {
         if (!kb_index_ext_ok(ent->d_name))
            continue;
         if (st.st_size <= 0 || st.st_size > KB_INDEX_PUSH_MAX_FILE_BYTES)
            continue;

         FILE *fp = fopen(full, "rb");
         if (!fp)
            continue;
         size_t sz = (size_t)st.st_size;
         char *buf = malloc(sz + 1);
         if (!buf)
         {
            fclose(fp);
            continue;
         }
         size_t got = fread(buf, 1, sz, fp);
         fclose(fp);
         if (got != sz)
         {
            free(buf);
            continue;
         }
         buf[sz] = '\0';

         /* Skip binary files by scanning for null bytes. */
         int binary = 0;
         for (size_t i = 0; i < sz && !binary; i++)
            if ((unsigned char)buf[i] == '\0')
               binary = 1;
         if (binary)
         {
            free(buf);
            continue;
         }

         cJSON *entry = cJSON_CreateObject();
         if (entry)
         {
            cJSON_AddStringToObject(entry, "rel_path", rel_child);
            cJSON_AddStringToObject(entry, "content", buf);
            cJSON_AddItemToArray(files_arr, entry);
            (*count)++;
         }
         free(buf);
      }
   }
   closedir(dir);
}
#endif /* AIMEE_POSIX */

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
         if (cJSON_IsString(p))
            snprintf(out[count].project, sizeof(out[count].project), "%s", p->valuestring);
         if (cJSON_IsString(f))
            snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", f->valuestring);
         if (cJSON_IsString(s))
            snprintf(out[count].snippet, sizeof(out[count].snippet), "%s", s->valuestring);
         out[count].rank = cJSON_IsNumber(r) ? r->valuedouble : 0.0;
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

static int kb_client_index_scan_v1(const char *name, const char *root, int force,
                                   kb_client_index_scan_result_t *out)
{
   if (!name || !name[0] || !root || !root[0])
   {
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
      return kb_client_index_scan_apply_response(NULL, out);
   cJSON_AddStringToObject(req, "project", name);
   cJSON_AddStringToObject(req, "root_path", root);
   if (force)
      cJSON_AddBoolToObject(req, "force", 1);

#ifdef AIMEE_POSIX
   /* When aimee-kb is remote (AIMEE_KB_API_URL set), the container cannot
    * reach the host filesystem via root_path.  Enumerate and push file
    * contents so the handler can index without filesystem access, using the
    * {"rel_path","content"} format the /v1/code/scan handler accepts. */
   if (kb_client_v1_base_url())
   {
      cJSON *files_arr = cJSON_AddArrayToObject(req, "files");
      if (files_arr)
      {
         int pushed = 0;
         kb_index_collect_files(root, NULL, files_arr, &pushed);
      }
   }
#endif

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
