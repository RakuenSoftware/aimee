/* cmd_index.c: code indexing CLI (scan, overview, find, blast-radius, structure)
 *
 * Reads and writes are routed through the knowledge service via
 * kb_client_index_* RPCs. If the service is unavailable, subcommands report
 * that instead of falling back to local session state. */
#include "aimee.h"
#include "commands.h"
#include "workspace.h"
#include "lsp.h"
#include "kb_client.h"
#include "util.h"
#include "cJSON.h"
#include "json_fluent.h" /* jo_ok */
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* Project discovery and the previous fork-based parallel scan have
 * moved into the knowledge service (which holds the single-slot scan coordinator).
 * cmd_infra.c performs its own workspace dedup against aimee.yaml when
 * the user runs `aimee workspace add`, so cmd_index.c no longer needs
 * a register_workspace_path helper. */

/* --- index subcommand handlers --- */

static void idx_scan(app_ctx_t *ctx, int argc, char **argv)
{

   int force = 0;
   int pos_argc = 0;
   char *pos_argv[64];
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--force") == 0)
         force = 1;
      else if (pos_argc < 64)
         pos_argv[pos_argc++] = argv[i];
   }

   int verbose = !ctx->json_output;
   kb_client_index_scan_result_t res;

   memset(&res, 0, sizeof(res));
   if (pos_argc == 0)
   {
      if (kb_client_index_scan(NULL, NULL, force, &res) != 0)
      {
         if (verbose)
         {
            if (strcmp(res.reason, "error") == 0 && res.message[0])
               fprintf(stderr, "aimee: %s\n", res.message);
            else
               fprintf(stderr,
                       "aimee: knowledge service unavailable — cannot scan canonical index\n");
         }
         if (ctx->json_output)
            emit_ok_ctx(ctx->json_fields, ctx->response_profile);
         return;
      }
   }
   else
   {
      for (int i = 0; i < pos_argc; i += 2)
      {
         if (i + 1 >= pos_argc)
            break;
         kb_client_index_scan_result_t one;
         memset(&one, 0, sizeof(one));
         if (kb_client_index_scan(pos_argv[i], pos_argv[i + 1], force, &one) != 0)
         {
            if (verbose)
            {
               if (strcmp(one.reason, "error") == 0 && one.message[0])
                  fprintf(stderr, "aimee: %s\n", one.message);
               else
                  fprintf(stderr,
                          "aimee: knowledge service unavailable — cannot scan canonical index\n");
            }
            if (ctx->json_output)
               emit_ok_ctx(ctx->json_fields, ctx->response_profile);
            return;
         }
         res.projects += one.projects;
         res.files += one.files;
         res.inspected += one.inspected;
         if (one.skipped)
         {
            res.skipped = 1;
            snprintf(res.reason, sizeof(res.reason), "%s", one.reason);
            res.retry_after = one.retry_after;
         }
      }
   }

   if (verbose)
   {
      if (res.skipped)
      {
         if (strcmp(res.reason, "cooldown") == 0)
            printf("==> Scan skipped: cooldown active (%lds remaining)\n", res.retry_after);
         else if (strcmp(res.reason, "busy") == 0)
            printf("==> Scan skipped: another scan is already running\n");
         else
            printf("==> Scan skipped: %s\n", res.reason[0] ? res.reason : "no reason given");
      }
      else
      {
         int unchanged = res.inspected - res.files;
         if (res.inspected > 0 && unchanged > 0)
            printf("==> Scan complete: %d project(s), %d file(s) re-indexed (%d unchanged)\n",
                   res.projects, res.files, unchanged);
         else
            printf("==> Scan complete: %d project(s), %d file(s) re-indexed\n", res.projects,
                   res.files);
      }
      fflush(stdout);
   }

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

static void idx_overview(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   project_info_t projects[64];
   int count = kb_client_index_list(projects, 64);

   if (count < 0)
   {
      /* Negative return means the knowledge service is unreachable. */
      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "status", "unavailable");
         cJSON_AddStringToObject(obj, "error", "knowledge service unavailable");
         cJSON_AddStringToObject(obj, "repair", "aimee-kb --start or run: ./update.sh");
         emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         fprintf(stderr, "aimee index: knowledge service unavailable\n");
         fprintf(stderr, "  repair: start the knowledge service with 'aimee-kb --start'\n");
         fprintf(stderr, "          or reinstall with: ./update.sh\n");
      }
      return;
   }

   if (count == 0)
   {
      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "status", "empty");
         cJSON_AddStringToObject(obj, "error", "no indexed projects");
         cJSON_AddStringToObject(obj, "repair", "aimee index scan <name> <root>");
         emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("No indexed projects.\n");
         printf("  repair: aimee index scan <project-name> <project-root>\n");
      }
      return;
   }

   char langbuf[1024];
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         int files = 0, defs = 0;
         kb_client_index_project_stats(projects[i].name, &files, &defs);
         langbuf[0] = '\0';
         kb_client_index_project_lang(projects[i].name, langbuf, sizeof(langbuf));
         cJSON *p = cJSON_CreateObject();
         cJSON_AddStringToObject(p, "id", projects[i].name);
         cJSON_AddStringToObject(p, "root", projects[i].root);
         cJSON_AddNumberToObject(p, "file_count", files);
         cJSON_AddNumberToObject(p, "symbol_count", defs);
         cJSON_AddStringToObject(p, "last_scan", projects[i].scanned_at);
         cJSON_AddStringToObject(p, "freshness",
                                 projects[i].scanned_at[0] ? projects[i].scanned_at : "unknown");
         if (langbuf[0])
         {
            cJSON *langs = cJSON_Parse(langbuf);
            if (langs)
               cJSON_AddItemToObject(p, "languages", langs);
         }
         cJSON_AddItemToArray(arr, p);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("%-24s %8s %8s  %-20s  %s\n", "PROJECT", "FILES", "SYMBOLS", "LAST SCAN", "ROOT");
   printf("%-24s %8s %8s  %-20s  %s\n", "-------", "-----", "-------", "---------", "----");
   for (int i = 0; i < count; i++)
   {
      int files = 0, defs = 0;
      kb_client_index_project_stats(projects[i].name, &files, &defs);
      const char *scan = projects[i].scanned_at[0] ? projects[i].scanned_at : "(never)";
      printf("%-24s %8d %8d  %-20s  %s\n", projects[i].name, files, defs, scan, projects[i].root);
      langbuf[0] = '\0';
      if (kb_client_index_project_lang(projects[i].name, langbuf, sizeof(langbuf)) == 0 &&
          langbuf[0] && strcmp(langbuf, "[]") != 0)
      {
         /* Print compact lang summary: c(42), h(21), ... */
         cJSON *langs = cJSON_Parse(langbuf);
         if (langs && cJSON_IsArray(langs))
         {
            printf("  %-22s languages: ", "");
            int first = 1;
            cJSON *entry;
            cJSON_ArrayForEach(entry, langs)
            {
               cJSON *lang = cJSON_GetObjectItemCaseSensitive(entry, "lang");
               cJSON *cnt = cJSON_GetObjectItemCaseSensitive(entry, "count");
               if (cJSON_IsString(lang) && cJSON_IsNumber(cnt))
               {
                  printf("%s%s(%d)", first ? "" : ", ", lang->valuestring, cnt->valueint);
                  first = 0;
               }
            }
            printf("\n");
            cJSON_Delete(langs);
         }
      }
   }
}

static int idx_source_authority_enabled(void)
{
   const char *enabled = getenv("AIMEE_DELEGATE_SOURCE_AUTHORITY");
   return enabled && enabled[0] && strcmp(enabled, "0") != 0;
}

static int idx_source_path_matches(const char *source_path, const char *hit_path)
{
   if (!source_path || !source_path[0] || !hit_path || !hit_path[0])
      return 0;
   if (strcmp(source_path, hit_path) == 0)
      return 1;
   const char *source_base = strrchr(source_path, '/');
   source_base = source_base ? source_base + 1 : source_path;
   const char *hit_base = strrchr(hit_path, '/');
   hit_base = hit_base ? hit_base + 1 : hit_path;
   return source_base[0] && strcmp(source_base, hit_base) == 0;
}

static int idx_source_paths_contains(const char *hit_path)
{
   const char *paths = getenv("AIMEE_DELEGATE_SOURCE_PATHS");
   if (!paths || !paths[0] || !hit_path || !hit_path[0])
      return 0;
   const char *p = paths;
   while (*p)
   {
      const char *nl = strchr(p, '\n');
      size_t len = nl ? (size_t)(nl - p) : strlen(p);
      if (len > 0)
      {
         char one[MAX_PATH_LEN];
         size_t copy = len < sizeof(one) - 1 ? len : sizeof(one) - 1;
         memcpy(one, p, copy);
         one[copy] = '\0';
         if (idx_source_path_matches(one, hit_path))
            return 1;
      }
      if (!nl)
         break;
      p = nl + 1;
   }
   return 0;
}

static const char *idx_source_authority_root(void)
{
   const char *root = getenv("AIMEE_DELEGATE_WORKTREE_ROOT");
   return root && root[0] ? root : NULL;
}

static int idx_git_path_differs_from_main(const char *file_path)
{
   const char *root = idx_source_authority_root();
   if (!root || !file_path || !file_path[0] || file_path[0] == '/')
      return -1;
   const char *argv[] = {"git", "-C", root, "diff", "--quiet", "main", "--", file_path, NULL};
   char *out = NULL;
   int rc = safe_exec_capture(argv, &out, 4096);
   free(out);
   if (rc == 0)
      return 0;
   if (rc == 1)
      return 1;
   return -1;
}

static const char *idx_find_freshness(const char *file_path, const char **authority_out)
{
   if (authority_out)
      *authority_out = "discovery_only";
   if (idx_source_authority_enabled() && idx_source_paths_contains(file_path))
   {
      if (authority_out)
         *authority_out = "current_source";
      return "source_packet_current";
   }
   if (idx_git_path_differs_from_main(file_path) > 0)
      return "worktree_differs_from_main";
   return "canonical_index_unverified";
}

static void idx_find(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("index find requires an identifier");
   term_hit_t hits[128];
   int count = kb_client_index_find(argv[0], hits, 128);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *h = cJSON_CreateObject();
         cJSON_AddStringToObject(h, "project", hits[i].project);
         cJSON_AddStringToObject(h, "file_path", hits[i].file_path);
         cJSON_AddNumberToObject(h, "line", hits[i].line);
         cJSON_AddNumberToObject(h, "line_end", hits[i].line_end);
         cJSON_AddStringToObject(h, "kind", hits[i].kind);
         if (idx_source_authority_enabled() || idx_source_authority_root())
         {
            const char *authority = "discovery_only";
            const char *freshness = idx_find_freshness(hits[i].file_path, &authority);
            cJSON_AddStringToObject(h, "authority", authority);
            cJSON_AddStringToObject(h, "freshness", freshness);
            if (strcmp(freshness, "worktree_differs_from_main") == 0)
               cJSON_AddBoolToObject(h, "stale_index_risk", 1);
         }
         cJSON_AddItemToArray(arr, h);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      return;
   }

   if (count == 0)
   {
      printf("No matches for '%s' (knowledge service may be unavailable).\n", argv[0]);
      return;
   }
   for (int i = 0; i < count; i++)
   {
      if (idx_source_authority_enabled() || idx_source_authority_root())
      {
         const char *authority = "discovery_only";
         const char *freshness = idx_find_freshness(hits[i].file_path, &authority);
         printf("  %s:%d  %-12s [%s] freshness=%s authority=%s\n", hits[i].file_path, hits[i].line,
                hits[i].kind, hits[i].project, freshness, authority);
      }
      else
         printf("  %s:%d  %-12s [%s]\n", hits[i].file_path, hits[i].line, hits[i].kind,
                hits[i].project);
   }
}

static void idx_blast_radius(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 2)
      fatal("index blast-radius requires project and file");
   blast_radius_t br;
   if (kb_client_index_blast_radius(argv[0], argv[1], &br) != 0)
   {
      if (!ctx->json_output)
         fprintf(stderr,
                 "aimee: knowledge service unavailable — cannot compute canonical blast radius\n");
      memset(&br, 0, sizeof(br));
      snprintf(br.file, sizeof(br.file), "%s", argv[1]);
   }
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "file", br.file);
      cJSON *deps = cJSON_AddArrayToObject(j, "dependents");
      for (int i = 0; i < br.dependent_count; i++)
         cJSON_AddItemToArray(deps, cJSON_CreateString(br.dependents[i]));
      cJSON *ddeps = cJSON_AddArrayToObject(j, "dependencies");
      for (int i = 0; i < br.dependency_count; i++)
         cJSON_AddItemToArray(ddeps, cJSON_CreateString(br.dependencies[i]));
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("Blast radius for %s:\n", br.file);
   if (br.dependent_count > 0)
   {
      printf("  Dependents (%d):\n", br.dependent_count);
      for (int i = 0; i < br.dependent_count; i++)
         printf("    %s\n", br.dependents[i]);
   }
   if (br.dependency_count > 0)
   {
      printf("  Dependencies (%d):\n", br.dependency_count);
      for (int i = 0; i < br.dependency_count; i++)
         printf("    %s\n", br.dependencies[i]);
   }
   if (br.dependent_count == 0 && br.dependency_count == 0)
      printf("  No dependents or dependencies found.\n");
}

static void idx_structure(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 2)
      fatal("index structure requires project and file");
   definition_t defs[256];
   int count = kb_client_index_structure(argv[0], argv[1], defs, 256);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *d = cJSON_CreateObject();
         cJSON_AddStringToObject(d, "name", defs[i].name);
         cJSON_AddStringToObject(d, "kind", defs[i].kind);
         cJSON_AddNumberToObject(d, "line", defs[i].line);
         cJSON_AddNumberToObject(d, "line_end", defs[i].line_end);
         cJSON_AddItemToArray(arr, d);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (count == 0)
      {
         printf("No definitions found in %s/%s\n", argv[0], argv[1]);
         return;
      }
      for (int i = 0; i < count; i++)
      {
         printf("  %4d  %-12s %s\n", defs[i].line, defs[i].kind, defs[i].name);
      }
   }
}

static void idx_callers(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("index callers requires a symbol name");
   const char *project = argc >= 2 ? argv[1] : NULL;
   caller_hit_t hits[128];
   int count = kb_client_index_find_callers(project, argv[0], hits, 128);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *h = cJSON_CreateObject();
         cJSON_AddStringToObject(h, "project", hits[i].project);
         cJSON_AddStringToObject(h, "file_path", hits[i].file_path);
         cJSON_AddStringToObject(h, "caller", hits[i].caller);
         cJSON_AddNumberToObject(h, "line", hits[i].line);
         cJSON_AddItemToArray(arr, h);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (count == 0)
      {
         printf("No callers found for '%s'\n", argv[0]);
         return;
      }
      printf("Callers of '%s':\n", argv[0]);
      for (int i = 0; i < count; i++)
      {
         if (hits[i].caller[0])
            printf("  %s:%d in %s() [%s]\n", hits[i].file_path, hits[i].line, hits[i].caller,
                   hits[i].project);
         else
            printf("  %s:%d (file scope) [%s]\n", hits[i].file_path, hits[i].line, hits[i].project);
      }
   }
}

/* --- LSP subcommand helpers --- */

/* Resolve the workspace root: use --workspace flag or first configured workspace */
static const char *resolve_workspace(int argc, char **argv, char *buf, size_t buf_size)
{
   for (int i = 0; i + 1 < argc; i++)
   {
      if (strcmp(argv[i], "--workspace") == 0)
         return argv[i + 1];
   }
   config_t cfg;
   if (config_load(&cfg) == 0 && cfg.workspace_count > 0)
   {
      snprintf(buf, buf_size, "%s", cfg.workspaces[0]);
      return buf;
   }
   /* Fall back to cwd */
   if (getcwd(buf, buf_size))
      return buf;
   return ".";
}

static void idx_lsp_diag(app_ctx_t *ctx, int argc, char **argv)
{
   char ws_buf[MAX_PATH_LEN];
   const char *workspace = resolve_workspace(argc, argv, ws_buf, sizeof(ws_buf));

   /* Optional positional: file path */
   const char *file = NULL;
   for (int i = 0; i < argc; i++)
   {
      if (argv[i][0] != '-')
      {
         file = argv[i];
         break;
      }
   }

   lsp_manager_init();
   lsp_diag_t diags[64];
   int n = lsp_manager_diagnostics(workspace, file, diags, 64);

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < n; i++)
      {
         cJSON *d = cJSON_CreateObject();
         cJSON_AddStringToObject(d, "file", diags[i].file);
         cJSON_AddNumberToObject(d, "line", diags[i].line + 1);
         cJSON_AddNumberToObject(d, "col", diags[i].col + 1);
         cJSON_AddStringToObject(d, "severity", lsp_severity_label(diags[i].severity));
         cJSON_AddStringToObject(d, "message", diags[i].message);
         cJSON_AddItemToArray(arr, d);
      }
      cJSON *resp = jo_ok();
      cJSON_AddItemToObject(resp, "diagnostics", arr);
      char *out = cJSON_PrintUnformatted(resp);
      if (out)
      {
         printf("%s\n", out);
         free(out);
      }
      cJSON_Delete(resp);
   }
   else
   {
      if (n == 0)
      {
         printf("No diagnostics for %s\n", file ? file : workspace);
      }
      else
      {
         printf("%d diagnostic%s:\n", n, n == 1 ? "" : "s");
         for (int i = 0; i < n; i++)
         {
            printf("  %s:%d:%d [%s] %s\n", diags[i].file[0] ? diags[i].file : "(unknown)",
                   diags[i].line + 1, diags[i].col + 1, lsp_severity_label(diags[i].severity),
                   diags[i].message);
         }
      }
   }
}

static void idx_lsp_def(app_ctx_t *ctx, int argc, char **argv)
{

   if (argc < 3)
      fatal("usage: index lsp-def <file> <line> <col>");

   const char *file = argv[0];
   int line = atoi(argv[1]) - 1; /* convert from 1-based display to 0-based LSP */
   int col = atoi(argv[2]) - 1;

   char ws_buf[MAX_PATH_LEN];
   const char *workspace = resolve_workspace(argc, argv, ws_buf, sizeof(ws_buf));

   lsp_manager_init();
   lsp_location_t locs[32];
   char errbuf[256] = "";
   int n = lsp_manager_definition(workspace, file, line, col, locs, 32, errbuf, sizeof(errbuf));

   if (n < 0)
   {
      fprintf(stderr, "lsp-def error: %s\n", errbuf[0] ? errbuf : "request failed");
      return;
   }

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < n; i++)
      {
         cJSON *l = cJSON_CreateObject();
         cJSON_AddStringToObject(l, "file", locs[i].file);
         cJSON_AddNumberToObject(l, "line", locs[i].line + 1);
         cJSON_AddNumberToObject(l, "col", locs[i].col + 1);
         cJSON_AddItemToArray(arr, l);
      }
      cJSON *resp = jo_ok();
      cJSON_AddItemToObject(resp, "locations", arr);
      char *out = cJSON_PrintUnformatted(resp);
      if (out)
      {
         printf("%s\n", out);
         free(out);
      }
      cJSON_Delete(resp);
   }
   else
   {
      if (n == 0)
         printf("No definition found\n");
      else
         for (int i = 0; i < n; i++)
            printf("  %s:%d:%d\n", locs[i].file[0] ? locs[i].file : "(unknown)", locs[i].line + 1,
                   locs[i].col + 1);
   }
}

static void idx_lsp_refs(app_ctx_t *ctx, int argc, char **argv)
{

   if (argc < 3)
      fatal("usage: index lsp-refs <file> <line> <col>");

   const char *file = argv[0];
   int line = atoi(argv[1]) - 1;
   int col = atoi(argv[2]) - 1;

   char ws_buf[MAX_PATH_LEN];
   const char *workspace = resolve_workspace(argc, argv, ws_buf, sizeof(ws_buf));

   lsp_manager_init();
   lsp_location_t locs[64];
   char errbuf[256] = "";
   int n = lsp_manager_references(workspace, file, line, col, locs, 64, errbuf, sizeof(errbuf));

   if (n < 0)
   {
      fprintf(stderr, "lsp-refs error: %s\n", errbuf[0] ? errbuf : "request failed");
      return;
   }

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < n; i++)
      {
         cJSON *l = cJSON_CreateObject();
         cJSON_AddStringToObject(l, "file", locs[i].file);
         cJSON_AddNumberToObject(l, "line", locs[i].line + 1);
         cJSON_AddNumberToObject(l, "col", locs[i].col + 1);
         cJSON_AddItemToArray(arr, l);
      }
      cJSON *resp = jo_ok();
      cJSON_AddItemToObject(resp, "locations", arr);
      char *out = cJSON_PrintUnformatted(resp);
      if (out)
      {
         printf("%s\n", out);
         free(out);
      }
      cJSON_Delete(resp);
   }
   else
   {
      if (n == 0)
         printf("No references found\n");
      else
         for (int i = 0; i < n; i++)
            printf("  %s:%d:%d\n", locs[i].file[0] ? locs[i].file : "(unknown)", locs[i].line + 1,
                   locs[i].col + 1);
   }
}

static void idx_lsp_rename(app_ctx_t *ctx, int argc, char **argv)
{

   if (argc < 4)
      fatal("usage: index lsp-rename <file> <line> <col> <new-name>");

   const char *file = argv[0];
   int line = atoi(argv[1]) - 1; /* convert from 1-based display to 0-based LSP */
   int col = atoi(argv[2]) - 1;
   const char *new_name = argv[3];

   char ws_buf[MAX_PATH_LEN];
   const char *workspace = resolve_workspace(argc, argv, ws_buf, sizeof(ws_buf));

   lsp_manager_init();
   char out[4096] = "";
   char errbuf[256] = "";
   int n = lsp_manager_rename(workspace, file, line, col, new_name, out, sizeof(out), errbuf,
                              sizeof(errbuf));

   if (n < 0)
   {
      fprintf(stderr, "lsp-rename error: %s\n", errbuf[0] ? errbuf : "request failed");
      return;
   }

   if (ctx->json_output)
   {
      cJSON *resp = jo_ok();
      cJSON_AddNumberToObject(resp, "files_changed", n);
      cJSON_AddStringToObject(resp, "changed", out);
      char *s = cJSON_PrintUnformatted(resp);
      if (s)
      {
         printf("%s\n", s);
         free(s);
      }
      cJSON_Delete(resp);
   }
   else
   {
      if (n == 0)
         printf("No changes applied (symbol not found or no rename needed)\n");
      else
         printf("Renamed in %d file%s: %s\n", n, n == 1 ? "" : "s", out);
   }
}

/* --- subcommand table --- */

static const subcmd_t index_subcmds[] = {
    {"scan", "Scan a project directory for indexing", idx_scan},
    {"overview", "List all indexed projects", idx_overview},
    {"find", "Find definitions/references by identifier", idx_find},
    {"blast-radius", "Show files affected by changes to a file", idx_blast_radius},
    {"structure", "Show file structure (functions, classes)", idx_structure},
    {"callers", "Find all callers of a symbol", idx_callers},
    {"lsp-diag", "Show LSP diagnostics for a file or workspace", idx_lsp_diag},
    {"lsp-def", "Go to definition via LSP (file line col)", idx_lsp_def},
    {"lsp-refs", "Find all references via LSP (file line col)", idx_lsp_refs},
    {"lsp-rename", "Rename a symbol workspace-wide via LSP (file line col new-name)",
     idx_lsp_rename},
    {NULL, NULL, NULL},
};

const subcmd_t *get_index_subcmds(void)
{
   return index_subcmds;
}

/* --- cmd_index --- */

void cmd_index(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("index", index_subcmds);
      exit(1);
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   if (subcmd_dispatch(index_subcmds, sub, ctx, argc, argv) != 0)
      fatal("unknown index subcommand: %s", sub);
}
