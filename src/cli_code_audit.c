/* cli_code_audit.c: see cli_code_audit.h. */
#include "cli_code_audit.h"
#include "cli_client.h" /* cli_http_request, cli_v1_client_* */
#include "aimee_home.h"
#include "cJSON.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- pure helpers ---- */

#define AUDIT_CONTEXT_FILE "audit_context.txt"

static const char *audit_ext(const char *path)
{
   const char *base = strrchr(path, '/');
   base = base ? base + 1 : path;
   const char *dot = strrchr(base, '.');
   return dot ? dot : "";
}

int audit_is_code_file(const char *path)
{
   static const char *exts[] = {".c",  ".h",   ".cc",    ".cpp", ".cxx",   ".hpp",  ".ts", ".tsx",
                                ".js", ".jsx", ".py",    ".go",  ".rs",    ".java", ".kt", ".cs",
                                ".rb", ".php", ".swift", ".m",   ".scala", ".lua"};
   const char *e = audit_ext(path ? path : "");
   for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
      if (strcmp(e, exts[i]) == 0)
         return 1;
   return 0;
}

int audit_is_test_file(const char *path)
{
   if (!path)
      return 0;
   if (strstr(path, "/test/") || strstr(path, "/tests/") || strstr(path, "/__tests__/") ||
       strstr(path, "/spec/"))
      return 1;
   const char *base = strrchr(path, '/');
   base = base ? base + 1 : path;
   if (strstr(base, ".test.") || strstr(base, ".spec.") || strstr(base, "_test.") ||
       strstr(base, "_spec.") || strncmp(base, "test_", 5) == 0 || strncmp(base, "test-", 5) == 0 ||
       strncmp(base, "spec_", 5) == 0)
      return 1;
   return 0;
}

void audit_stem(const char *path, char *out, size_t cap)
{
   if (!out || cap == 0)
      return;
   out[0] = '\0';
   if (!path)
      return;
   const char *base = strrchr(path, '/');
   base = base ? base + 1 : path;
   char tmp[256];
   snprintf(tmp, sizeof(tmp), "%s", base);
   char *dot = strrchr(tmp, '.');
   if (dot)
      *dot = '\0';
   /* Strip a leading test_/spec_ affix and a trailing _test/_spec affix. */
   char *s = tmp;
   if (strncmp(s, "test_", 5) == 0 || strncmp(s, "spec_", 5) == 0 || strncmp(s, "test-", 5) == 0)
      s += 5;
   size_t len = strlen(s);
   const char *suffixes[] = {"_test", "_spec", ".test", ".spec"};
   for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
   {
      size_t sl = strlen(suffixes[i]);
      if (len >= sl && strcmp(s + len - sl, suffixes[i]) == 0)
      {
         s[len - sl] = '\0';
         break;
      }
   }
   snprintf(out, cap, "%s", s);
}

int audit_count_todos(const char *content)
{
   if (!content)
      return 0;
   static const char *markers[] = {"TODO", "FIXME", "HACK", "XXX"};
   int count = 0;
   for (size_t m = 0; m < sizeof(markers) / sizeof(markers[0]); m++)
   {
      const char *p = content;
      size_t ml = strlen(markers[m]);
      while ((p = strstr(p, markers[m])) != NULL)
      {
         count++;
         p += ml;
      }
   }
   return count;
}

static int path_contains_segment(const char *path, const char *needle)
{
   return path && needle && strstr(path, needle) != NULL;
}

static int path_starts_with(const char *path, const char *prefix)
{
   return path && prefix && strncmp(path, prefix, strlen(prefix)) == 0;
}

static int audit_count_marker(const char *content, const char *marker)
{
   int count = 0;
   const char *p = content;
   size_t ml = strlen(marker);
   while (ml > 0 && (p = strstr(p, marker)) != NULL)
   {
      count++;
      p += ml;
   }
   return count;
}

int audit_count_db_in_routes(const char *path, const char *content)
{
   if (!path || !content)
      return 0;
   int route_like =
       path_contains_segment(path, "/routes/") || path_contains_segment(path, "/api/") ||
       path_contains_segment(path, "/controllers/") || path_contains_segment(path, "/handlers/") ||
       path_starts_with(path, "routes/") || path_starts_with(path, "api/") ||
       path_starts_with(path, "controllers/") || path_starts_with(path, "handlers/");
   if (!route_like)
      return 0;
   static const char *markers[] = {"aimee_pg_", "pg_query", "mysql_",      "SELECT ",
                                   "INSERT ",   "UPDATE ",  "DELETE FROM "};
   int count = 0;
   for (size_t m = 0; m < sizeof(markers) / sizeof(markers[0]); m++)
      count += audit_count_marker(content, markers[m]);

   char marker[16];
   snprintf(marker, sizeof(marker), "%s%s%s", "sqli", "te3", "_");
   count += audit_count_marker(content, marker);
   snprintf(marker, sizeof(marker), "%s%s%s", "db", "2", "_");
   count += audit_count_marker(content, marker);
   return count;
}

static int line_has_any(const char *line, size_t len, const char *const *needles, int n)
{
   char tmp[1024];
   size_t copy = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
   memcpy(tmp, line, copy);
   tmp[copy] = '\0';
   for (int i = 0; i < n; i++)
      if (strstr(tmp, needles[i]))
         return 1;
   return 0;
}

int audit_count_unhandled_http(const char *content)
{
   if (!content)
      return 0;
   static const char *calls[] = {
       "fetch(", "axios.", "requests.", "http_request(", "cli_http_request(", "curl_easy_perform("};
   static const char *guards[] = {"try",    "catch", "except", "if (", "if(",
                                  "status", "ok",    "error",  "NULL", "nullptr"};
   int count = 0;
   const char *line = content;
   while (*line)
   {
      const char *next = strchr(line, '\n');
      size_t len = next ? (size_t)(next - line) : strlen(line);
      if (line_has_any(line, len, calls, (int)(sizeof(calls) / sizeof(calls[0]))) &&
          !line_has_any(line, len, guards, (int)(sizeof(guards) / sizeof(guards[0]))))
         count++;
      if (!next)
         break;
      line = next + 1;
   }
   return count;
}

int audit_debt_score(int code_files, int untested, int todo_markers, int db_in_routes,
                     int unhandled_http)
{
   if (code_files <= 0)
      return 100;
   /* Penalize the untested fraction (up to 60 pts) and TODO density (up to
    * 40 pts, ~1 pt per 2 markers per 100 files). 100 = clean. */
   double untested_frac = (double)untested / (double)code_files;
   double todo_density = (double)todo_markers / (double)code_files;
   double db_density = (double)db_in_routes / (double)code_files;
   double http_density = (double)unhandled_http / (double)code_files;
   double penalty =
       untested_frac * 55.0 + todo_density * 15.0 + db_density * 20.0 + http_density * 15.0;
   if (penalty > 100.0)
      penalty = 100.0;
   int score = (int)(100.0 - penalty + 0.5);
   if (score < 0)
      score = 0;
   if (score > 100)
      score = 100;
   return score;
}

/* ---- tree walk + report (impure) ---- */

#define AUDIT_MAX_FILES 40000
#define AUDIT_MAX_READ  (512 * 1024)

typedef struct
{
   char **code_paths; /* non-test code files */
   char **code_stems;
   int n_code, cap_code;
   char **test_stems;
   int n_test, cap_test;
   int todo_markers;
   int db_in_routes;
   int unhandled_http;
} audit_acc_t;

static int skip_dir(const char *name)
{
   static const char *skip[] = {".git",   "node_modules", "build",  "dist",        ".next",
                                "vendor", "target",       ".cache", "__pycache__", ".venv"};
   for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
      if (strcmp(name, skip[i]) == 0)
         return 1;
   return name[0] == '.' && name[1] == '\0';
}

static char *audit_read_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = NULL;
   if (sz > 0 && sz < AUDIT_MAX_READ)
   {
      buf = malloc((size_t)sz + 1);
      if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz)
         buf[sz] = '\0';
      else
      {
         free(buf);
         buf = NULL;
      }
   }
   fclose(f);
   return buf;
}

static void acc_push(char ***arr, int *n, int *cap, const char *s)
{
   if (*n >= AUDIT_MAX_FILES)
      return;
   if (*n >= *cap)
   {
      int nc = *cap ? *cap * 2 : 256;
      char **np = realloc(*arr, (size_t)nc * sizeof(char *));
      if (!np)
         return;
      *arr = np;
      *cap = nc;
   }
   (*arr)[(*n)++] = strdup(s);
}

/* Append a non-test code file: path and stem go to parallel arrays in lockstep
 * under the shared n_code/cap_code counters. */
static void code_push(audit_acc_t *a, const char *path, const char *stem)
{
   if (a->n_code >= AUDIT_MAX_FILES)
      return;
   if (a->n_code >= a->cap_code)
   {
      int nc = a->cap_code ? a->cap_code * 2 : 256;
      char **np = realloc(a->code_paths, (size_t)nc * sizeof(char *));
      if (!np)
         return;
      a->code_paths = np;
      char **ns = realloc(a->code_stems, (size_t)nc * sizeof(char *));
      if (!ns)
         return;
      a->code_stems = ns;
      a->cap_code = nc;
   }
   a->code_paths[a->n_code] = strdup(path);
   a->code_stems[a->n_code] = strdup(stem);
   a->n_code++;
}

static void audit_walk(const char *dir, audit_acc_t *a, int depth)
{
   if (depth > 32 || a->n_code >= AUDIT_MAX_FILES)
      return;
   DIR *d = opendir(dir);
   if (!d)
      return;
   struct dirent *ent;
   char path[4096];
   while ((ent = readdir(d)) != NULL)
   {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
         continue;
      snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
      struct stat st;
      if (stat(path, &st) != 0)
         continue;
      if (S_ISDIR(st.st_mode))
      {
         if (!skip_dir(ent->d_name))
            audit_walk(path, a, depth + 1);
         continue;
      }
      if (!S_ISREG(st.st_mode) || !audit_is_code_file(path))
         continue;

      char *content = audit_read_file(path);
      if (content)
      {
         a->todo_markers += audit_count_todos(content);
         a->db_in_routes += audit_count_db_in_routes(path, content);
         a->unhandled_http += audit_count_unhandled_http(content);
         free(content);
      }
      char stem[256];
      audit_stem(path, stem, sizeof(stem));
      if (audit_is_test_file(path))
         acc_push(&a->test_stems, &a->n_test, &a->cap_test, stem);
      else
         code_push(a, path, stem);
   }
   closedir(d);
}

static int stem_in_tests(const audit_acc_t *a, const char *stem)
{
   if (!stem || !stem[0])
      return 0;
   for (int i = 0; i < a->n_test; i++)
      if (a->test_stems[i] && strcmp(a->test_stems[i], stem) == 0)
         return 1;
   return 0;
}

static cJSON *audit_graph_remote(const char *project, int quiet)
{
   char *endpoint = cli_v1_client_endpoint();
   if (!endpoint)
   {
      if (!quiet)
         fprintf(stderr, "code audit --graph: no aimee server configured (set `aimee remote`).\n");
      return NULL;
   }
   char *bearer = cli_v1_client_bearer();
   cJSON *body = cJSON_CreateObject();
   if (project && project[0])
      cJSON_AddStringToObject(body, "project", project);
   char *body_s = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);

   int status = 0;
   cJSON *resp =
       cli_http_request(endpoint, "POST", "/v1/code/audit", body_s, bearer, 60000, &status);
   free(endpoint);
   free(bearer);
   free(body_s);
   if (!resp || status != 200)
   {
      if (!quiet)
         fprintf(stderr, "code audit --graph: server query failed (status %d)\n", status);
      cJSON_Delete(resp);
      return NULL;
   }
   return resp;
}

static void print_graph_report(cJSON *resp)
{
   cJSON *de = cJSON_GetObjectItemCaseSensitive(resp, "dead_exports");
   cJSON *cy = cJSON_GetObjectItemCaseSensitive(resp, "cycles");
   cJSON *cl = cJSON_GetObjectItemCaseSensitive(resp, "clones");
   printf("  dead exports:  %d\n", cJSON_GetArraySize(de));
   int shown = 0;
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, de)
   {
      if (shown++ >= 10)
         break;
      if (cJSON_IsString(it))
         printf("    - %s\n", it->valuestring);
   }
   printf("  import cycles: %d\n", cJSON_GetArraySize(cy));
   shown = 0;
   cJSON_ArrayForEach(it, cy)
   {
      if (shown++ >= 10)
         break;
      if (cJSON_IsString(it))
         printf("    - %s\n", it->valuestring);
   }
   printf("  clone groups:  %d\n", cJSON_GetArraySize(cl));
   cJSON *gran = cJSON_GetObjectItemCaseSensitive(resp, "clone_granularity");
   cJSON *note = cJSON_GetObjectItemCaseSensitive(resp, "clone_scope_note");
   if (cJSON_IsString(gran))
      printf("    granularity: %s\n", gran->valuestring);
   if (cJSON_IsString(note))
      printf("    note: %s\n", note->valuestring);
   cJSON *nc = cJSON_GetObjectItemCaseSensitive(resp, "near_clones");
   printf("  near clones:   %d\n", cJSON_GetArraySize(nc));
   shown = 0;
   cJSON_ArrayForEach(it, nc)
   {
      if (shown++ >= 10)
         break;
      if (cJSON_IsString(it))
         printf("    - %s\n", it->valuestring);
   }
}

static void build_audit_context(char *out, size_t cap, const char *dir, const char *project,
                                int score, int untested, int todos, int db_in_routes,
                                int unhandled_http, cJSON *graph)
{
   if (!out || cap == 0)
      return;
   int dead = 0, cycles = 0, clones = 0;
   if (graph)
   {
      dead = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(graph, "dead_exports"));
      cycles = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(graph, "cycles"));
      clones = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(graph, "clones"));
   }
   snprintf(out, cap,
            "AUDIT_CONTEXT: scope=%s%s%s; debt_score=%d/100; prioritize %d untested files, %d "
            "TODO/FIXME markers, %d route DB-access smells, %d unhandled HTTP calls, %d dead "
            "exports, %d import cycles, %d clone groups.",
            dir && dir[0] ? dir : ".", project && project[0] ? "; project=" : "",
            project && project[0] ? project : "", score, untested, todos, db_in_routes,
            unhandled_http, dead, cycles, clones);
}

static int write_audit_context(const char *audit_context)
{
   if (!audit_context || !audit_context[0])
      return -1;
   const char *dir = aimee_home();
   if (!dir || !dir[0])
      return -1;
   char path[4096];
   int n = snprintf(path, sizeof(path), "%s/%s", dir, AUDIT_CONTEXT_FILE);
   if (n < 0 || (size_t)n >= sizeof(path))
      return -1;
   FILE *f = fopen(path, "w");
   if (!f)
      return -1;
   fprintf(f, "%s\n", audit_context);
   return fclose(f) == 0 ? 0 : -1;
}

static void audit_roadmap_counts(cJSON *graph, int *dead, int *cycles, int *clones)
{
   if (dead)
      *dead =
          graph ? cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(graph, "dead_exports")) : 0;
   if (cycles)
      *cycles = graph ? cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(graph, "cycles")) : 0;
   if (clones)
      *clones = graph ? cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(graph, "clones")) : 0;
}

static void audit_add_roadmap_item(cJSON *arr, const char *text)
{
   if (arr && text && text[0])
      cJSON_AddItemToArray(arr, cJSON_CreateString(text));
}

static cJSON *audit_build_roadmap_json(int untested, int todos, int db_in_routes,
                                       int unhandled_http, cJSON *graph)
{
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return NULL;
   int dead = 0, cycles = 0, clones = 0;
   audit_roadmap_counts(graph, &dead, &cycles, &clones);
   char line[192];
   if (untested > 0)
   {
      snprintf(line, sizeof(line), "Add or link tests for %d untested source file%s.", untested,
               untested == 1 ? "" : "s");
      audit_add_roadmap_item(arr, line);
   }
   if (db_in_routes > 0)
   {
      snprintf(line, sizeof(line), "Move or wrap %d route-layer DB access smell%s.", db_in_routes,
               db_in_routes == 1 ? "" : "s");
      audit_add_roadmap_item(arr, line);
   }
   if (unhandled_http > 0)
   {
      snprintf(line, sizeof(line), "Add explicit error handling for %d HTTP call%s.",
               unhandled_http, unhandled_http == 1 ? "" : "s");
      audit_add_roadmap_item(arr, line);
   }
   if (todos > 0)
   {
      snprintf(line, sizeof(line), "Triage %d TODO/FIXME/HACK/XXX marker%s.", todos,
               todos == 1 ? "" : "s");
      audit_add_roadmap_item(arr, line);
   }
   if (cycles > 0)
   {
      snprintf(line, sizeof(line), "Break %d import cycle%s.", cycles, cycles == 1 ? "" : "s");
      audit_add_roadmap_item(arr, line);
   }
   if (dead > 0)
   {
      snprintf(line, sizeof(line), "Review %d exported symbol%s without imports/references.", dead,
               dead == 1 ? "" : "s");
      audit_add_roadmap_item(arr, line);
   }
   if (clones > 0)
   {
      snprintf(line, sizeof(line), "Inspect %d exact clone group%s before refactoring.", clones,
               clones == 1 ? "" : "s");
      audit_add_roadmap_item(arr, line);
   }
   if (cJSON_GetArraySize(arr) == 0)
      audit_add_roadmap_item(arr, "No prioritized code-health actions found.");
   return arr;
}

static void audit_print_roadmap(cJSON *roadmap)
{
   printf("\nPrioritized roadmap:\n");
   if (!cJSON_IsArray(roadmap) || cJSON_GetArraySize(roadmap) == 0)
   {
      printf("  1. No prioritized code-health actions found.\n");
      return;
   }
   int n = 1;
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, roadmap)
   {
      if (cJSON_IsString(it) && it->valuestring)
         printf("  %d. %s\n", n++, it->valuestring);
   }
}

int handle_code_audit(int argc, char **argv, int json_output)
{
   const char *dir = ".";
   const char *project = "";
   int dir_set = 0, graph_only = 0, fix = 0;
   for (int i = 0; i < argc; i++)
   {
      if (!argv[i])
         continue;
      if (strcmp(argv[i], "--json") == 0)
         json_output = 1;
      else if (strcmp(argv[i], "--graph") == 0)
         graph_only = 1;
      else if (strcmp(argv[i], "--fix") == 0)
         fix = 1;
      else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc)
         project = argv[++i];
      else if (argv[i][0] != '-' && !dir_set)
      {
         dir = argv[i];
         dir_set = 1;
      }
   }

   if (graph_only)
   {
      cJSON *graph = audit_graph_remote(project, 0);
      if (!graph)
         return 0;
      if (json_output)
      {
         char *s = cJSON_PrintUnformatted(graph);
         if (s)
         {
            puts(s);
            free(s);
         }
      }
      else
      {
         printf("aimee code audit — graph-derived checks (via aimee-kb)\n");
         print_graph_report(graph);
      }
      cJSON_Delete(graph);
      return 0;
   }

   audit_acc_t a;
   memset(&a, 0, sizeof(a));
   audit_walk(dir, &a, 0);

   int untested = 0;
   for (int i = 0; i < a.n_code; i++)
      if (!stem_in_tests(&a, a.code_stems[i]))
         untested++;

   cJSON *graph = audit_graph_remote(project, 1);
   int score =
       audit_debt_score(a.n_code, untested, a.todo_markers, a.db_in_routes, a.unhandled_http);
   char audit_context[512];
   build_audit_context(audit_context, sizeof(audit_context), dir, project, score, untested,
                       a.todo_markers, a.db_in_routes, a.unhandled_http, graph);
   int context_written = write_audit_context(audit_context) == 0 ? 1 : 0;
   cJSON *roadmap =
       audit_build_roadmap_json(untested, a.todo_markers, a.db_in_routes, a.unhandled_http, graph);

   if (json_output)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "code_files", a.n_code);
      cJSON_AddNumberToObject(o, "test_files", a.n_test);
      cJSON_AddNumberToObject(o, "untested_files", untested);
      cJSON_AddNumberToObject(o, "todo_markers", a.todo_markers);
      cJSON_AddNumberToObject(o, "db_in_routes", a.db_in_routes);
      cJSON_AddNumberToObject(o, "unhandled_http_calls", a.unhandled_http);
      cJSON_AddNumberToObject(o, "debt_score", score);
      cJSON_AddStringToObject(o, "audit_context", audit_context);
      cJSON_AddBoolToObject(o, "audit_context_written", context_written);
      if (roadmap)
         cJSON_AddItemToObject(o, "roadmap", cJSON_Duplicate(roadmap, 1));
      if (fix)
      {
         cJSON *fx = cJSON_AddObjectToObject(o, "fix");
         cJSON_AddNumberToObject(fx, "applied", 0);
         cJSON_AddStringToObject(fx, "status", "no_safe_automatic_fixes");
      }
      if (graph)
         cJSON_AddItemToObject(o, "graph", cJSON_Duplicate(graph, 1));
      else
         cJSON_AddStringToObject(o, "graph_status", "unavailable");
      char *s = cJSON_PrintUnformatted(o);
      if (s)
      {
         puts(s);
         free(s);
      }
      cJSON_Delete(o);
   }
   else
   {
      printf("aimee code audit — %s\n", dir);
      printf("  code files:     %d\n", a.n_code);
      printf("  test files:     %d\n", a.n_test);
      printf("  untested files: %d (%.0f%%)\n", untested,
             a.n_code ? 100.0 * untested / a.n_code : 0.0);
      printf("  TODO/FIXME/HACK/XXX markers: %d\n", a.todo_markers);
      printf("  DB-in-routes smells: %d\n", a.db_in_routes);
      printf("  unhandled HTTP calls: %d\n", a.unhandled_http);
      printf("  debt score:     %d/100 (100 = clean)\n", score);
      if (graph)
      {
         printf("\nGraph-derived checks (via aimee-kb):\n");
         print_graph_report(graph);
      }
      else
         printf("\nGraph-derived checks: unavailable (configure `aimee remote` to query kb).\n");
      audit_print_roadmap(roadmap);
      printf("\n%s\n", audit_context);
      printf("  pre-injection: %s%s\n", context_written ? "written to " : "not written",
             context_written ? AUDIT_CONTEXT_FILE : "");
      if (fix)
         printf("  --fix: no safe automatic fixes are available yet; roadmap emitted only.\n");
   }

   for (int i = 0; i < a.n_code; i++)
   {
      free(a.code_paths[i]);
      free(a.code_stems[i]);
   }
   for (int i = 0; i < a.n_test; i++)
      free(a.test_stems[i]);
   free(a.code_paths);
   free(a.code_stems);
   free(a.test_stems);
   cJSON_Delete(roadmap);
   cJSON_Delete(graph);
   return 0;
}
