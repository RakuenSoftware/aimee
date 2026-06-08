/* cli_code_audit.c: see cli_code_audit.h. */
#include "cli_code_audit.h"
#include "cli_client.h" /* cli_http_request, cli_rpc_client_* */
#include "cJSON.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- pure helpers ---- */

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

int audit_debt_score(int code_files, int untested, int todo_markers)
{
   if (code_files <= 0)
      return 100;
   /* Penalize the untested fraction (up to 60 pts) and TODO density (up to
    * 40 pts, ~1 pt per 2 markers per 100 files). 100 = clean. */
   double untested_frac = (double)untested / (double)code_files;
   double todo_density = (double)todo_markers / (double)code_files;
   double penalty = untested_frac * 60.0 + todo_density * 20.0;
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

/* Graph-derived checks: query the server's /v1/code/audit (dead exports, import
 * cycles, clones — computed kb-side over entity_edges + code_embeddings) and
 * print them. Advisory; returns 0. */
static int audit_graph_remote(const char *project, int json_output)
{
   char *endpoint = cli_rpc_client_endpoint();
   if (!endpoint)
   {
      fprintf(stderr, "code audit --graph: no aimee server configured (set `aimee remote`).\n");
      return 0;
   }
   char *bearer = cli_rpc_client_bearer();
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
      fprintf(stderr, "code audit --graph: server query failed (status %d)\n", status);
      cJSON_Delete(resp);
      return 0;
   }

   if (json_output)
   {
      char *s = cJSON_PrintUnformatted(resp);
      if (s)
      {
         puts(s);
         free(s);
      }
   }
   else
   {
      cJSON *de = cJSON_GetObjectItemCaseSensitive(resp, "dead_exports");
      cJSON *cy = cJSON_GetObjectItemCaseSensitive(resp, "cycles");
      cJSON *cl = cJSON_GetObjectItemCaseSensitive(resp, "clones");
      printf("aimee code audit — graph-derived checks (via aimee-kb)\n");
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
   }
   cJSON_Delete(resp);
   return 0;
}

int handle_code_audit(int argc, char **argv, int json_output)
{
   const char *dir = ".";
   const char *project = "";
   int dir_set = 0, graph = 0;
   for (int i = 0; i < argc; i++)
   {
      if (!argv[i])
         continue;
      if (strcmp(argv[i], "--json") == 0)
         json_output = 1;
      else if (strcmp(argv[i], "--graph") == 0)
         graph = 1;
      else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc)
         project = argv[++i];
      else if (argv[i][0] != '-' && !dir_set)
      {
         dir = argv[i];
         dir_set = 1;
      }
   }

   /* --graph switches to the kb-side graph-derived checks (a different surface
    * from the local file scan). */
   if (graph)
      return audit_graph_remote(project, json_output);

   audit_acc_t a;
   memset(&a, 0, sizeof(a));
   audit_walk(dir, &a, 0);

   int untested = 0;
   for (int i = 0; i < a.n_code; i++)
      if (!stem_in_tests(&a, a.code_stems[i]))
         untested++;

   int score = audit_debt_score(a.n_code, untested, a.todo_markers);

   if (json_output)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "code_files", a.n_code);
      cJSON_AddNumberToObject(o, "test_files", a.n_test);
      cJSON_AddNumberToObject(o, "untested_files", untested);
      cJSON_AddNumberToObject(o, "todo_markers", a.todo_markers);
      cJSON_AddNumberToObject(o, "debt_score", score);
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
      printf("  debt score:     %d/100 (100 = clean)\n", score);
      printf("\nNote: graph-derived checks (dead exports, import cycles, clones) are the kb "
             "follow-on in docs/proposals/pending/code-health-audit.md.\n");
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
   return 0;
}
