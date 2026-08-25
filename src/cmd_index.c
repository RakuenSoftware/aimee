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
         char stable_project[512], stable_workspace[512];
         if (workspace_repo_index_keys(pos_argv[i + 1], NULL, stable_project,
                                       sizeof(stable_project), stable_workspace,
                                       sizeof(stable_workspace)) != 0)
            fatal("cannot read or persist a stable project identity for %s", pos_argv[i + 1]);
         const char *project = stable_project;
         kb_client_index_scan_result_t one;
         memset(&one, 0, sizeof(one));
         if (kb_client_index_scan(project, pos_argv[i + 1], force, &one) != 0)
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

static void idx_verify(app_ctx_t *ctx, int argc, char **argv)
{
   const char *project = NULL;
   const char *root = NULL;
   int deep = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--deep") == 0)
         deep = 1;
      else if (!project)
         project = argv[i];
      else if (!root)
         root = argv[i];
   }
   if (!project || !root)
      fatal("index verify requires <project> <root> [--deep]");
   int http_status = 0;
   char *json = kb_client_index_verify_json(project, root, deep, &http_status);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   if (!resp)
   {
      free(json);
      fatal("index verify: knowledge service unavailable");
   }
   cJSON *workspace = cJSON_GetObjectItemCaseSensitive(resp, "workspace_state");
   const char *state = cJSON_IsString(workspace) ? workspace->valuestring : "unavailable";
   int matched = http_status >= 200 && http_status < 300 && strcmp(state, "matched") == 0;
   if (ctx->json_output)
      emit_json_ctx(resp, ctx->json_fields, ctx->response_profile);
   else
   {
      cJSON *revision = cJSON_GetObjectItemCaseSensitive(resp, "index_revision");
      cJSON *verification = cJSON_GetObjectItemCaseSensitive(resp, "verification");
      cJSON *modified = cJSON_GetObjectItemCaseSensitive(resp, "modified_files");
      cJSON *missing = cJSON_GetObjectItemCaseSensitive(resp, "missing_files");
      cJSON *unindexed = cJSON_GetObjectItemCaseSensitive(resp, "unindexed_files");
      printf("index_state=current revision=%lld workspace_state=%s verification=%s\n",
             cJSON_IsNumber(revision) ? (long long)revision->valuedouble : 0, state,
             cJSON_IsString(verification) ? verification->valuestring : "none");
      printf("modified=%d missing=%d unindexed=%d\n",
             cJSON_IsNumber(modified) ? modified->valueint : 0,
             cJSON_IsNumber(missing) ? missing->valueint : 0,
             cJSON_IsNumber(unindexed) ? unindexed->valueint : 0);
      cJSON *examples = cJSON_GetObjectItemCaseSensitive(resp, "examples");
      cJSON *example;
      cJSON_ArrayForEach(example, examples)
         if (cJSON_IsString(example))
            printf("  %s\n", example->valuestring);
   }
   cJSON_Delete(resp);
   free(json);
   if (!matched)
      exit(1);
}

static int idx_lifecycle_emit(app_ctx_t *ctx, const char *operation, const char *project,
                              const char *json, int http_status)
{
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   if (!resp)
   {
      if (ctx->json_output)
      {
         resp = cJSON_CreateObject();
         cJSON_AddStringToObject(resp, "status", "unavailable");
         cJSON_AddNumberToObject(resp, "http_status", http_status);
         cJSON_AddStringToObject(resp, "operation", operation);
         cJSON_AddStringToObject(resp, "project", project);
         emit_json_ctx(resp, ctx->json_fields, ctx->response_profile);
      }
      else
         fprintf(stderr, "aimee index %s: knowledge service returned HTTP %d\n", operation,
                 http_status);
      return 0;
   }
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int ok = http_status >= 200 && http_status < 300 && cJSON_IsString(status) &&
            strcmp(status->valuestring, "ok") == 0;
   if (ctx->json_output)
   {
      emit_json_ctx(resp, ctx->json_fields, ctx->response_profile);
      return ok;
   }
   if (!ok)
   {
      fprintf(stderr, "aimee index %s: %s\n", operation, json ? json : "invalid response");
      cJSON_Delete(resp);
      return 0;
   }
   cJSON *mode = cJSON_GetObjectItemCaseSensitive(resp, "mode");
   cJSON *hash = cJSON_GetObjectItemCaseSensitive(resp, "manifest_hash");
   cJSON *generation = cJSON_GetObjectItemCaseSensitive(resp, "generation");
   cJSON *total = cJSON_GetObjectItemCaseSensitive(resp, "total_rows");
   cJSON *state = cJSON_GetObjectItemCaseSensitive(resp, "state");
   if (cJSON_IsString(state))
      printf("%s generation %lld is %s\n", project,
             cJSON_IsNumber(generation) ? (long long)generation->valuedouble : 0,
             state->valuestring);
   else
   {
      printf("%s %s: %lld row(s), generation %lld, manifest %s\n", operation,
             cJSON_IsString(mode) ? mode->valuestring : "result",
             cJSON_IsNumber(total) ? (long long)total->valuedouble : 0,
             cJSON_IsNumber(generation) ? (long long)generation->valuedouble : 0,
             cJSON_IsString(hash) ? hash->valuestring : "");
      if (cJSON_IsString(mode) && strcmp(mode->valuestring, "dry_run") == 0 && cJSON_IsString(hash))
         printf("  confirm: aimee index %s %s --confirm %s --reason '<reason>'\n", operation,
                project, hash->valuestring);
   }
   cJSON_Delete(resp);
   return 1;
}

static void idx_detach(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("index detach requires a stable project id");
   int status = 0;
   char *json = kb_client_index_project_lifecycle_json("detach", argv[0], NULL, NULL, 30, &status);
   int ok = idx_lifecycle_emit(ctx, "detach", argv[0], json, status);
   free(json);
   if (!ok)
      fatal("index detach failed");
}

static void idx_lifecycle_mutation(app_ctx_t *ctx, const char *operation, int argc, char **argv)
{
   const char *project = NULL;
   const char *confirm = NULL;
   const char *reason = NULL;
   int retention_days = 30;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--confirm") == 0 && i + 1 < argc)
         confirm = argv[++i];
      else if (strcmp(argv[i], "--reason") == 0 && i + 1 < argc)
         reason = argv[++i];
      else if (strcmp(argv[i], "--retention-days") == 0 && i + 1 < argc)
         retention_days = atoi(argv[++i]);
      else if (argv[i][0] != '-' && !project)
         project = argv[i];
   }
   char active[256] = "";
   if (!project && strcmp(operation, "gc") == 0)
   {
      char cwd[MAX_PATH_LEN];
      if (getcwd(cwd, sizeof(cwd)) &&
          workspace_repo_identity(cwd, active, sizeof(active), NULL, 0) == 0)
         project = active;
   }
   if (!project)
      fatal(strcmp(operation, "purge") == 0 ? "index purge requires a stable project id"
                                            : "index gc requires an active or explicit project");
   if (confirm && (!reason || !reason[0]))
      fatal("confirmed index lifecycle mutations require --reason");
   int status = 0;
   char *json = kb_client_index_project_lifecycle_json(operation, project, confirm, reason,
                                                       retention_days, &status);
   int ok = idx_lifecycle_emit(ctx, operation, project, json, status);
   free(json);
   if (!ok)
      fatal("index %s failed", operation);
}

static void idx_purge(app_ctx_t *ctx, int argc, char **argv)
{
   idx_lifecycle_mutation(ctx, "purge", argc, argv);
}

static void idx_gc(app_ctx_t *ctx, int argc, char **argv)
{
   idx_lifecycle_mutation(ctx, "gc", argc, argv);
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

static int idx_active_project(char *out, size_t cap)
{
   char cwd[MAX_PATH_LEN];
   if (!out || cap == 0 || !getcwd(cwd, sizeof(cwd)))
      return -1;
   out[0] = '\0';
   return workspace_repo_identity(cwd, out, cap, NULL, 0);
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
   int all = argc >= 3 && strcmp(argv[1], "--scope") == 0 && strcmp(argv[2], "all") == 0;
   char project[256] = "";
   (void)idx_active_project(project, sizeof(project));
   term_hit_t hits[128];
   int count = kb_client_index_find_scoped(project, all, argv[0], hits, 128);
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
   if (argc < 1)
      fatal("index blast-radius requires a file");
   char active[256] = "";
   const char *project = NULL;
   const char *file_path = NULL;
   if (argc >= 2)
   {
      project = argv[0];
      file_path = argv[1];
   }
   else if (idx_active_project(active, sizeof(active)) == 0)
   {
      project = active;
      file_path = argv[0];
   }
   if (!project)
      fatal("index blast-radius requires an active or explicit project");
   blast_radius_t br;
   if (kb_client_index_blast_radius(project, file_path, &br) != 0)
      fatal("knowledge service could not resolve the current-generation blast radius");
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "file", br.file);
      cJSON_AddStringToObject(j, "project", br.project);
      cJSON_AddNumberToObject(j, "generation", (double)br.generation);
      cJSON_AddStringToObject(j, "freshness", br.freshness);
      cJSON_AddBoolToObject(j, "resolved", br.resolved);
      cJSON *deps = cJSON_AddArrayToObject(j, "dependents");
      cJSON *dependent_edges = cJSON_AddArrayToObject(j, "dependent_edges");
      for (int i = 0; i < br.dependent_count; i++)
      {
         cJSON_AddItemToArray(deps, cJSON_CreateString(br.dependents[i]));
         cJSON *edge = cJSON_CreateObject();
         cJSON_AddStringToObject(edge, "path", br.dependents[i]);
         cJSON_AddStringToObject(edge, "provenance", br.dependent_meta[i].provenance);
         cJSON_AddStringToObject(edge, "confidence", br.dependent_meta[i].confidence);
         cJSON_AddStringToObject(edge, "project", br.dependent_meta[i].project);
         cJSON_AddNumberToObject(edge, "generation", (double)br.dependent_meta[i].generation);
         cJSON_AddStringToObject(edge, "freshness", br.dependent_meta[i].freshness);
         cJSON_AddItemToArray(dependent_edges, edge);
      }
      cJSON *ddeps = cJSON_AddArrayToObject(j, "dependencies");
      cJSON *dependency_edges = cJSON_AddArrayToObject(j, "dependency_edges");
      for (int i = 0; i < br.dependency_count; i++)
      {
         cJSON_AddItemToArray(ddeps, cJSON_CreateString(br.dependencies[i]));
         cJSON *edge = cJSON_CreateObject();
         cJSON_AddStringToObject(edge, "identity", br.dependencies[i]);
         cJSON_AddStringToObject(edge, "provenance", br.dependency_meta[i].provenance);
         cJSON_AddStringToObject(edge, "confidence", br.dependency_meta[i].confidence);
         cJSON_AddStringToObject(edge, "project", br.dependency_meta[i].project);
         cJSON_AddNumberToObject(edge, "generation", (double)br.dependency_meta[i].generation);
         cJSON_AddStringToObject(edge, "freshness", br.dependency_meta[i].freshness);
         cJSON_AddItemToArray(dependency_edges, edge);
      }
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("Blast radius for %s:\n", br.file);
   if (br.dependent_count > 0)
   {
      printf("  Dependents (%d):\n", br.dependent_count);
      for (int i = 0; i < br.dependent_count; i++)
         printf("    %s [%s, %s, %s@%lld, %s]\n", br.dependents[i], br.dependent_meta[i].provenance,
                br.dependent_meta[i].confidence, br.dependent_meta[i].project,
                br.dependent_meta[i].generation, br.dependent_meta[i].freshness);
   }
   if (br.dependency_count > 0)
   {
      printf("  Dependencies (%d):\n", br.dependency_count);
      for (int i = 0; i < br.dependency_count; i++)
         printf("    %s [%s, %s, %s@%lld, %s]\n", br.dependencies[i],
                br.dependency_meta[i].provenance, br.dependency_meta[i].confidence,
                br.dependency_meta[i].project, br.dependency_meta[i].generation,
                br.dependency_meta[i].freshness);
   }
   if (br.dependent_count == 0 && br.dependency_count == 0)
      printf("  No dependents or dependencies found.\n");
}

static void idx_structure(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("index structure requires a file");
   char active[256] = "";
   const char *project = NULL;
   const char *file_path = NULL;
   if (argc >= 2)
   {
      project = argv[0];
      file_path = argv[1];
   }
   else if (idx_active_project(active, sizeof(active)) == 0)
   {
      project = active;
      file_path = argv[0];
   }
   if (!project)
      fatal("index structure requires an active or explicit project");
   definition_t defs[256];
   int count = kb_client_index_structure(project, file_path, defs, 256);
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
         printf("No definitions found in %s/%s\n", project, file_path);
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
   char active[256] = "";
   int all = 0;
   const char *project = NULL;
   if (argc >= 2 && strcmp(argv[1], "--scope") == 0)
   {
      if (argc < 3 || (strcmp(argv[2], "all") != 0 && strcmp(argv[2], "current") != 0))
         fatal("index callers --scope must be current or all");
      all = strcmp(argv[2], "all") == 0;
   }
   else if (argc >= 2)
      project = argv[1];
   if (!project && idx_active_project(active, sizeof(active)) == 0)
      project = active;
   caller_hit_t hits[128];
   int count = kb_client_index_find_callers_scoped(project, all, argv[0], hits, 128);
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
   if (config_workspace_count() > 0)
   {
      snprintf(buf, buf_size, "%s", config_workspaces(0));
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
    {"verify", "Read-only manifest or content-hash drift check", idx_verify},
    {"detach", "Hide a project generation without deleting it", idx_detach},
    {"purge", "Dry-run or confirm an audited project purge", idx_purge},
    {"gc", "Retire stale checkout aliases and detached generations", idx_gc},
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
