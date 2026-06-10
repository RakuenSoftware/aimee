/* cmd_core.c: core commands (init, setup, version, status, usage, mode, plan, implement, env,
 * contract, clarify) */
#include "aimee.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "config.h"
#include "db1.h"
#include "delegate_credentials.h"
#include "db2/lifecycle.h"
#include "kb_client.h"
#include "workspace.h"
#include "commands.h"
#include "persona.h"
#include "dashboard.h"
#include "hud.h"
#include "memory.h"
#include "cJSON.h"
#include "aimee_client.h"
#include "headers/mcp_git.h"
#include "headers/git_verify.h"
#include "headers/events.h"
#include "headers/aimee_home.h"
#include "headers/workspace_manifest.h"
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>

/* Bootstrap the DB2 postgres connection during `aimee init`. This is the
 * non-interactive "is the database reachable + healthy" gate documented
 * in docs/proposals/pending/three-db-pin-backends.md. We do not provision
 * a postgres role/database here (those typically need superuser privileges
 * and platform-specific commands); we only run db2_init against the
 * configured db2_url and report a clear remediation if it fails.
 *
 * Returns 0 when DB2 was reachable and pg_trgm is installed, or when
 * db2_url is unset (operator has opted out of DB2 wiring).
 * Returns -1 when DB2 was configured but unreachable / unhealthy. */
static int bootstrap_db2(const config_t *cfg, int json_output)
{
   if (!cfg->db2_url[0])
   {
      if (!json_output)
         fprintf(stderr,
                 "Note: db2_url is not configured. Set it in ~/.config/aimee/aimee.yaml under "
                 "database.db2_url to enable the project/workspace knowledge tier.\n");
      return 0;
   }

   db2_set_embedding_dim(cfg->embedding_dim);
   if (db2_init(cfg->db2_url) == 0)
   {
      int schema_ok = 0;
      int have_pg_trgm = 0;
      if (db2_health_probe(&schema_ok, &have_pg_trgm) == 0 && schema_ok && have_pg_trgm)
      {
         if (!json_output)
            fprintf(stderr, "DB2 reachable: schema applied, pg_trgm installed.\n");
         db2_shutdown();
         return 0;
      }
      db2_shutdown();
      if (!json_output)
         fprintf(stderr, "DB2 reachable but health probe failed (schema=%d trgm=%d).\n", schema_ok,
                 have_pg_trgm);
      return -1;
   }

   /* db2_init prints the pg_trgm-missing remediation directly when that's
    * the cause; otherwise the failure is connect/auth/database-missing.
    * Surface a generic remediation that covers the common cases. */
   if (!json_output)
      fprintf(stderr,
              "DB2 init failed for %s.\n"
              "Common fixes:\n"
              "  - Ensure the postgres server is running and reachable from this host.\n"
              "  - Ensure the role and database in db2_url exist:\n"
              "      CREATE ROLE aimee LOGIN PASSWORD '...';\n"
              "      CREATE DATABASE aimee OWNER aimee;\n"
              "  - Connect as a privileged role and install the trigram extension:\n"
              "      \\c aimee\n"
              "      CREATE EXTENSION pg_trgm;\n"
              "  - Re-run `aimee init` once the above succeeds.\n",
              cfg->db2_url);
   return -1;
}

void cmd_init(app_ctx_t *ctx, int argc, char **argv)
{
   int novel = 0, songwriter = 0;
   for (int i = 1; i < argc; i++)
   {
      if (argv[i] && strcmp(argv[i], "--novel") == 0)
         novel = 1;
      else if (argv[i] && strcmp(argv[i], "--songwriter") == 0)
         songwriter = 1;
   }

   ensure_client_integrations();

   config_t cfg;
   config_load(&cfg);
   config_save(&cfg);

   /* Seed the built-in personas as editable files so users can document,
    * customize, and add their own. Idempotent — never overwrites. */
   {
      char pdir[MAX_PATH_LEN];
      snprintf(pdir, sizeof(pdir), "%s/personas", config_default_dir());
      int np = persona_install_defaults(pdir);
      if (!ctx->json_output && np > 0)
         fprintf(stderr, "Seeded: %d persona(s) in %s\n", np, pdir);
   }

   /* Durable entry point for a creative mode: persist it so every process
    * (sessions, delegates, compute agents) reads the matching persona. */
   if (novel)
      config_persist_mode("novel");
   else if (songwriter)
      config_persist_mode("songwriter");

   if (db1_init(cfg.db1_path) != 0)
      fatal("failed to initialize database");
   db1_shutdown();

   int db2_ok = (bootstrap_db2(&cfg, ctx->json_output) == 0);

   /* Create workspace-local .mcp.json for MCP-capable clients */
   char cwd[MAX_PATH_LEN];
   if (getcwd(cwd, sizeof(cwd)))
   {
      ensure_mcp_json(cwd);

      /* Generate .aimee-rules if not present: a creative-mode scaffold (story
       * bible / songbook), otherwise the detected-stack engineering doc. */
      if (novel)
      {
         int rules_rc = write_novel_rules_file(cwd);
         if (!ctx->json_output && rules_rc == 0)
            fprintf(stderr, "Generated: .aimee-rules (novel mode story bible)\n");
      }
      else if (songwriter)
      {
         int rules_rc = write_songbook_rules_file(cwd);
         if (!ctx->json_output && rules_rc == 0)
            fprintf(stderr, "Generated: .aimee-rules (songwriter mode songbook)\n");
      }
      else
      {
         stack_info_t stacks[MAX_DETECTED_STACKS];
         int stack_count = detect_project_stacks(cwd, stacks, MAX_DETECTED_STACKS);
         int rules_rc = write_aimee_rules_file(cwd, stacks, stack_count);
         if (!ctx->json_output && rules_rc == 0)
            fprintf(stderr, "Generated: .aimee-rules (%d stack(s) detected)\n", stack_count);
      }

      /* Install git pre-push hook when enforce: true so terminal-level
       * `git push` is gated the same way MCP-tool pushes are. */
      verify_config_t vcfg;
      if (verify_load_config(cwd, &vcfg) == 0 && vcfg.enforce)
      {
         int hook_rc = verify_install_git_hook(cwd);
         if (!ctx->json_output)
         {
            if (hook_rc == 0)
               fprintf(stderr, "Installed: .git/hooks/pre-push (verify enforce gate)\n");
            else if (hook_rc == -2)
               fprintf(stderr, "Note: existing pre-push hook not replaced — add aimee verify check "
                               "manually\n");
         }
      }
   }

   if (ctx->json_output)
   {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddStringToObject(root, "db1_path", cfg.db1_path);
      cJSON_AddBoolToObject(root, "db2_ready", db2_ok);
      cJSON_AddStringToObject(root, "db2_url", cfg.db2_url);
      emit_json_ctx(root, ctx->json_fields, ctx->response_profile);
      cJSON_Delete(root);
   }
   else
   {
      fprintf(stderr, "Initialized: %s%s\n", cfg.db1_path,
              db2_ok ? "" : " (DB2 not ready — see above)");
   }
}

/* --- cmd_setup (also aliased as quickstart) --- */

/* Clone one manifest repo into dest, returning 0 on success. */
static int clone_repo(const char *url, const char *dest)
{
   char cmd[MAX_PATH_LEN + 600];
   snprintf(cmd, sizeof(cmd), "git clone -- %s %s", url, dest);
   int exit_code = 0;
   char *output = run_cmd(cmd, &exit_code);
   if (output)
   {
      if (output[0])
         fprintf(stderr, "%s", output);
      free(output);
   }
   return exit_code == 0 ? 0 : -1;
}

/* Register path in cfg, save, and discover+index its projects into db. */
static void setup_register_workspace(config_t *cfg, const char *path, int *total_projects)
{
   /* Duplicate check */
   for (int i = 0; i < cfg->workspace_count; i++)
   {
      if (strcmp(cfg->workspaces[i], path) == 0)
         goto discover; /* already registered, still index */
   }
   if (cfg->workspace_count < 64)
   {
      snprintf(cfg->workspaces[cfg->workspace_count++], MAX_PATH_LEN, "%s", path);
      config_save(cfg);
   }

discover:;
   char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
   int count =
       workspace_discover_projects(path, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
   if (count < 0)
   {
      fprintf(stderr, "setup: cannot scan workspace %s\n", path);
      return;
   }

   fprintf(stderr, "==> Workspace %s: %d project(s)\n", path, count);

   for (int p = 0; p < count; p++)
   {
      const char *name = strrchr(projects[p], '/');
      name = name ? name + 1 : projects[p];
      kb_client_index_scan_result_t res;
      (void)kb_client_index_scan(name, projects[p], 0, &res);
      ensure_mcp_json(projects[p]);
      ensure_codex_project_trusted(aimee_home(), projects[p]);
      (*total_projects)++;
   }
   ensure_mcp_json(path);
   ensure_codex_project_trusted(aimee_home(), path);
}

void cmd_setup(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   ensure_client_integrations();

   /* 1. Initialize database and config if missing — db1_init creates
    * the file and applies the DB1 schema if the connection is fresh. */
   config_t cfg;
   config_load(&cfg);
   config_save(&cfg);
   if (db1_init(cfg.db1_path) == 0)
      db1_shutdown();

   /* 2. Check for aimee.workspace.yaml in CWD */
   char cwd[MAX_PATH_LEN];
   int have_cwd = getcwd(cwd, sizeof(cwd)) != NULL;

   workspace_manifest_t manifest;
   int have_manifest = 0;
   if (have_cwd)
   {
      int rc = workspace_manifest_load(cwd, &manifest);
      if (rc == 0)
      {
         have_manifest = 1;
         fprintf(stderr, "==> Loading manifest: %s/%s\n", cwd, WORKSPACE_MANIFEST_FILENAME);
      }
   }

   /* 3. Clone repositories declared in the manifest */
   if (have_manifest && manifest.repo_count > 0)
   {
      fprintf(stderr, "==> Cloning %d repo(s) from manifest\n", manifest.repo_count);
      for (int i = 0; i < manifest.repo_count; i++)
      {
         const manifest_repo_t *r = &manifest.repos[i];
         char dest[MAX_PATH_LEN];
         if (r->path[0])
         {
            /* Relative path is resolved against CWD */
            if (r->path[0] == '/')
               snprintf(dest, sizeof(dest), "%s", r->path);
            else
               snprintf(dest, sizeof(dest), "%s/%s", cwd, r->path);
         }
         else
         {
            /* Derive name from URL */
            const char *slash = strrchr(r->url, '/');
            const char *name_start = slash ? slash + 1 : r->url;
            char name[256];
            snprintf(name, sizeof(name), "%s", name_start);
            size_t nl = strlen(name);
            if (nl > 4 && strcmp(name + nl - 4, ".git") == 0)
               name[nl - 4] = '\0';
            snprintf(dest, sizeof(dest), "%s/%s", cwd, name);
         }

         struct stat st;
         if (stat(dest, &st) == 0)
         {
            fprintf(stderr, "  skip (exists): %s\n", dest);
            continue;
         }

         fprintf(stderr, "  cloning: %s -> %s\n", r->url, dest);
         if (clone_repo(r->url, dest) != 0)
            fprintf(stderr, "  warning: git clone failed for %s\n", r->url);
      }
   }

   /* 4. Surface credential requirements */
   if (have_manifest && manifest.secret_count > 0)
   {
      fprintf(stderr, "==> Required credentials (%d):\n", manifest.secret_count);
      for (int i = 0; i < manifest.secret_count; i++)
      {
         const manifest_secret_t *s = &manifest.secrets[i];
         if (s->description[0])
            fprintf(stderr, "  %s — %s\n", s->name, s->description);
         else
            fprintf(stderr, "  %s\n", s->name);
      }
   }

   /* 5. Run dependency install commands from manifest */
   if (have_manifest && manifest.dep_cmd_count > 0)
   {
      fprintf(stderr, "==> Installing dependencies (%d command(s))\n", manifest.dep_cmd_count);
      for (int i = 0; i < manifest.dep_cmd_count; i++)
      {
         const char *dep_cmd = manifest.dep_cmds[i].cmd;
         fprintf(stderr, "  $ %s\n", dep_cmd);
         int exit_code = 0;
         char *output = run_cmd(dep_cmd, &exit_code);
         if (output)
         {
            if (output[0])
               fprintf(stderr, "%s", output);
            free(output);
         }
         if (exit_code != 0)
            fprintf(stderr, "  warning: command exited with %d\n", exit_code);
      }
   }

   /* 6. If no workspaces configured, auto-add CWD */
   if (cfg.workspace_count == 0 && have_cwd)
   {
      snprintf(cfg.workspaces[0], MAX_PATH_LEN, "%s", cwd);
      cfg.workspace_count = 1;
      config_save(&cfg);
      fprintf(stderr, "Auto-added workspace: %s\n", cwd);
   }

   /* 7. Discover and index projects in all workspaces */
   int total_projects = 0;
   for (int w = 0; w < cfg.workspace_count; w++)
      setup_register_workspace(&cfg, cfg.workspaces[w], &total_projects);
   db1_stmt_cache_clear();

   /* 8. Generate .aimee-rules from detected stacks when manifest allows */
   if (!have_manifest || manifest.generate_rules)
   {
      if (have_cwd)
      {
         stack_info_t stacks[MAX_DETECTED_STACKS];
         int stack_count = detect_project_stacks(cwd, stacks, MAX_DETECTED_STACKS);
         int rules_rc = write_aimee_rules_file(cwd, stacks, stack_count);
         if (!ctx->json_output && rules_rc == 0)
            fprintf(stderr, "Generated: .aimee-rules (%d stack(s) detected)\n", stack_count);
      }
   }

   if (ctx->json_output)
      emit_ok_kv_ctx("status", "provisioned", ctx->json_fields, ctx->response_profile);
   else
      fprintf(stderr, "==> Setup complete: %d project(s) indexed.\n", total_projects);
}

/* --- cmd_db: status and pragma subcmds --- */

/* --- cmd_version --- */

void cmd_version(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "version", AIMEE_VERSION);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("aimee %s\n", AIMEE_VERSION);
   }
}

/* --- cmd_status --- */

void cmd_status(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   printf("aimee %s\n", AIMEE_VERSION);

   /* Database status */
   config_t cfg;
   config_load(&cfg);
   db1_diag_t diag;
   db1_diag_inspect(cfg.db1_path, 0, &diag);
   if (diag.opened)
      printf("Database:  ok (schema v%d)\n", diag.schema_version);
   else
      printf("Database:  error (cannot open)\n");

   /* Provider status: test each configured agent */
   agent_config_t acfg;
   if (agent_load_config(&acfg) == 0)
   {
      agent_http_init();
      for (int i = 0; i < acfg.agent_count; i++)
      {
         agent_t *ag = &acfg.agents[i];
         if (!ag->enabled)
         {
            printf("Provider:  %s (disabled)\n", ag->name);
            continue;
         }

         agent_result_t result;
         memset(&result, 0, sizeof(result));
         int rc = agent_execute(ag, NULL, "Respond with 'ok'.", 64, 0.0, &result);

         if (rc == 0)
            printf("Provider:  %s (ok, %dms latency)\n", ag->name, result.latency_ms);
         else
         {
            const provider_health_t *h = provider_health_get(ag->provider);
            if (h && h->error[0])
               printf("Provider:  %s (%s)\n", ag->name, h->error);
            else
               printf("Provider:  %s (error: %s)\n", ag->name, result.error);
         }
         free(result.response);
      }
      agent_http_cleanup();
   }

   char cred_state_path[MAX_PATH_LEN];
   const char *dir = config_output_dir();
   snprintf(cred_state_path, sizeof(cred_state_path), "%s/delegate-credential-state.tsv",
            dir ? dir : "/tmp");
   (void)delegate_credentials_load_file(cred_state_path, time(NULL));
   char quota[4096];
   if (delegate_credentials_format_quota(NULL, quota, sizeof(quota)) > 0)
      printf("Credential pool:\n%s", quota);

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

void cmd_hud(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   int json_output = 0;
   int watch_mode = 0;
   unsigned int interval = 2;

   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--json") == 0)
      {
         json_output = 1;
      }
      else if (strcmp(argv[i], "--watch") == 0)
      {
         watch_mode = 1;
      }
      else if (strncmp(argv[i], "--watch=", 8) == 0)
      {
         watch_mode = 1;
         int parsed = atoi(argv[i] + 8);
         if (parsed > 0)
            interval = (unsigned int)parsed;
      }
   }

   config_t cfg;
   config_load(&cfg);
   if (db1_init(cfg.db1_path) != 0)
   {
      fprintf(stderr, "aimee: cannot initialize DB1\n");
      return;
   }

   if (watch_mode)
   {
      for (;;)
      {
         hud_status_t status;
         if (hud_gather(&status) != 0)
         {
            fprintf(stderr, "aimee: failed to gather HUD status\n");
            break;
         }

         char ts[32];
         now_utc(ts, sizeof(ts));
         printf("\033[2J\033[H");
         printf("%s\n\n", ts);
         hud_print(&status);
         fflush(stdout);
         sleep(interval);
      }
      return;
   }

   hud_status_t status;
   if (hud_gather(&status) != 0)
   {
      fprintf(stderr, "aimee: failed to gather HUD status\n");
      return;
   }

   if (json_output)
   {
      char *json = hud_json(&status);
      if (!json)
         fprintf(stderr, "aimee: failed to serialize HUD status\n");
      else
      {
         printf("%s\n", json);
         free(json);
      }
   }
   else
   {
      hud_print(&status);
   }
}

/* --- cmd_usage --- */

void cmd_usage(app_ctx_t *ctx, int argc, char **argv)
{
   config_t cfg;
   config_load(&cfg);
   if (db1_init(cfg.db1_path) != 0)
   {
      fprintf(stderr, "aimee: cannot initialize DB1\n");
      return;
   }

   /* Parse flags */
   const char *window = NULL;
   int by_tool = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strncmp(argv[i], "--last=", 7) == 0)
         window = argv[i] + 7;
      else if (strcmp(argv[i], "--last") == 0 && i + 1 < argc)
         window = argv[++i];
      else if (strcmp(argv[i], "--by-tool") == 0)
         by_tool = 1;
   }

   int since_hours = 0;
   if (window)
   {
      int hours = 24;
      if (strstr(window, "h"))
         hours = atoi(window);
      else if (strstr(window, "d"))
         hours = atoi(window) * 24;
      else if (strstr(window, "w"))
         hours = atoi(window) * 24 * 7;
      else
         hours = atoi(window);
      if (hours <= 0)
         hours = 24;
      since_hours = hours;
   }

   db1_token_audit_totals_t totals;
   memset(&totals, 0, sizeof(totals));
   (void)db1_token_audit_totals(since_hours, &totals);
   db1_token_audit_role_summary_t roles_rows[32];
   int role_count = db1_token_audit_by_role(since_hours, roles_rows, 32);
   if (role_count < 0)
      role_count = 0;
   db1_token_audit_tool_summary_t tool_rows[20];
   int tool_count = 0;
   if (by_tool)
   {
      tool_count = db1_token_audit_by_tool(since_hours, tool_rows, 20);
      if (tool_count < 0)
         tool_count = 0;
   }

   int64_t total_tokens = totals.prompt_tokens + totals.completion_tokens;

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "total_calls", totals.total_calls);
      cJSON_AddNumberToObject(obj, "prompt_tokens", (double)totals.prompt_tokens);
      cJSON_AddNumberToObject(obj, "completion_tokens", (double)totals.completion_tokens);
      cJSON_AddNumberToObject(obj, "cache_write_tokens", (double)totals.cache_write_tokens);
      cJSON_AddNumberToObject(obj, "cache_read_tokens", (double)totals.cache_read_tokens);
      cJSON_AddNumberToObject(obj, "total_tokens", (double)total_tokens);
      cJSON_AddNumberToObject(obj, "estimated_cost_usd", totals.estimated_cost_usd);

      /* By role */
      cJSON *roles = cJSON_AddArrayToObject(obj, "by_role");
      for (int i = 0; i < role_count; i++)
      {
         cJSON *r = cJSON_CreateObject();
         cJSON_AddStringToObject(r, "role", roles_rows[i].role);
         cJSON_AddNumberToObject(r, "calls", roles_rows[i].calls);
         cJSON_AddNumberToObject(r, "prompt_tokens", (double)roles_rows[i].prompt_tokens);
         cJSON_AddNumberToObject(r, "completion_tokens", (double)roles_rows[i].completion_tokens);
         cJSON_AddNumberToObject(r, "estimated_cost_usd", roles_rows[i].estimated_cost_usd);
         cJSON_AddItemToArray(roles, r);
      }

      if (by_tool)
      {
         /* By tool attribution */
         cJSON *tools = cJSON_AddArrayToObject(obj, "by_tool");
         for (int i = 0; i < tool_count; i++)
         {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "tool", tool_rows[i].tool_name);
            cJSON_AddNumberToObject(t, "calls", tool_rows[i].calls);
            cJSON_AddNumberToObject(t, "prompt_tokens", (double)tool_rows[i].prompt_tokens);
            cJSON_AddNumberToObject(t, "completion_tokens", (double)tool_rows[i].completion_tokens);
            cJSON_AddNumberToObject(t, "estimated_cost_usd", tool_rows[i].estimated_cost_usd);
            cJSON_AddItemToArray(tools, t);
         }
      }

      char *json = cJSON_PrintUnformatted(obj);
      printf("%s\n", json);
      free(json);
      cJSON_Delete(obj);
   }
   else
   {
      printf("Token Usage%s:\n", window ? "" : " (all time)");
      printf("  Total calls:       %d\n", totals.total_calls);
      printf("  Prompt tokens:     %lld\n", (long long)totals.prompt_tokens);
      printf("  Completion tokens: %lld\n", (long long)totals.completion_tokens);
      if (totals.cache_write_tokens > 0 || totals.cache_read_tokens > 0)
      {
         printf("  Cache write:       %lld\n", (long long)totals.cache_write_tokens);
         printf("  Cache read:        %lld\n", (long long)totals.cache_read_tokens);
      }
      printf("  Total tokens:      %lld\n", (long long)total_tokens);
      if (totals.estimated_cost_usd > 0.0)
         printf("  Estimated cost:    ~$%.4f\n", totals.estimated_cost_usd);
      printf("\n");

      /* By role */
      printf("By role:\n");
      for (int i = 0; i < role_count; i++)
      {
         printf("  %-16s %5d calls  %8lld tokens  ~$%.4f\n",
                roles_rows[i].role[0] ? roles_rows[i].role : "(none)", roles_rows[i].calls,
                (long long)(roles_rows[i].prompt_tokens + roles_rows[i].completion_tokens),
                roles_rows[i].estimated_cost_usd);
      }

      if (by_tool)
      {
         /* By tool attribution */
         printf("\nBy tool:\n");
         for (int i = 0; i < tool_count; i++)
         {
            printf("  %-20s %5d calls  %8lld tokens  ~$%.4f\n",
                   tool_rows[i].tool_name[0] ? tool_rows[i].tool_name : "(none)",
                   tool_rows[i].calls,
                   (long long)(tool_rows[i].prompt_tokens + tool_rows[i].completion_tokens),
                   tool_rows[i].estimated_cost_usd);
         }
      }
   }

   (void)ctx;
}

/* --- cmd_mode --- */

void cmd_mode(app_ctx_t *ctx, int argc, char **argv)
{
   {
      config_t cfg;
      config_load(&cfg);
      db1_init(cfg.db1_path);
   }
   const char *sid = session_id();

   session_state_t state;
   session_state_load(&state, sid);

   if (argc < 1)
   {
      /* Get mode */
      if (ctx->json_output)
         emit_ok_kv_ctx("mode", state.session_mode[0] ? state.session_mode : MODE_PLAN,
                        ctx->json_fields, ctx->response_profile);
      else
         printf("%s\n", state.session_mode[0] ? state.session_mode : MODE_PLAN);
      return;
   }

   const char *mode = argv[0];
   if (strcmp(mode, MODE_PLAN) != 0 && strcmp(mode, MODE_IMPLEMENT) != 0)
      fatal("invalid mode: %s (must be 'plan' or 'implement')", mode);

   snprintf(state.session_mode, sizeof(state.session_mode), "%s", mode);
   state.dirty = 1;
   session_state_force_save(&state, sid);

   if (ctx->json_output)
      emit_ok_kv_ctx("mode", mode, ctx->json_fields, ctx->response_profile);
   else
      fprintf(stderr, "Mode set to: %s\n", mode);
}

/* --- cmd_primary: pick which pool agent acts as primary for this session --- */

void cmd_primary(app_ctx_t *ctx, int argc, char **argv)
{
   const char *sid = session_id();
   if (!sid || !sid[0])
      fatal("no active session (set AIMEE_SESSION_ID or start a session)");
   char path[256];
   snprintf(path, sizeof(path), "/v1/sessions/%s/primary", sid);

   /* Show the current pinned primary (no args, or --show). */
   if (argc < 1 || strcmp(argv[0], "--show") == 0)
   {
      int st = 0;
      char *resp = aimee_client_request("GET", path, NULL, &st);
      char agent[MAX_AGENT_NAME] = "";
      if (resp && st == 200)
      {
         cJSON *root = cJSON_Parse(resp);
         cJSON *ja = root ? cJSON_GetObjectItemCaseSensitive(root, "agent") : NULL;
         if (cJSON_IsString(ja))
            snprintf(agent, sizeof(agent), "%s", ja->valuestring);
         cJSON_Delete(root);
      }
      free(resp);
      if (ctx->json_output)
         emit_ok_kv_ctx("primary", agent, ctx->json_fields, ctx->response_profile);
      else
         printf("%s\n", agent[0] ? agent : "(none — using default provider)");
      return;
   }

   /* Clear the pin; the session reverts to the default provider. */
   if (strcmp(argv[0], "--clear") == 0)
   {
      int st = 0;
      char *resp = aimee_client_request("DELETE", path, NULL, &st);
      free(resp);
      if (st != 200)
         fatal("failed to clear primary (server status %d; is aimee-server running?)", st);
      if (ctx->json_output)
         emit_ok_kv_ctx("primary", "", ctx->json_fields, ctx->response_profile);
      else
         fprintf(stderr, "Primary cleared; using default provider\n");
      return;
   }

   /* Set the active primary agent. */
   const char *name = argv[0];
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "agent", name);
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   int st = 0;
   char *resp = aimee_client_request("POST", path, body, &st);
   free(body);
   free(resp);
   if (st == 404)
      fatal("no such agent: %s (see 'aimee agent list')", name);
   if (st != 200)
      fatal("failed to set primary (server status %d; is aimee-server running?)", st);
   if (ctx->json_output)
      emit_ok_kv_ctx("primary", name, ctx->json_fields, ctx->response_profile);
   else
      fprintf(stderr, "Primary agent set to: %s\n", name);
}

/* --- cmd_plan / cmd_implement --- */

void cmd_plan(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   char *args[] = {(char *)MODE_PLAN};
   cmd_mode(ctx, 1, args);
}

void cmd_implement(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   char *args[] = {(char *)MODE_IMPLEMENT};
   cmd_mode(ctx, 1, args);
}

/* --- cmd_tdd --- */

void cmd_tdd(app_ctx_t *ctx, int argc, char **argv)
{
   {
      config_t cfg;
      config_load(&cfg);
      db1_init(cfg.db1_path);
   }
   const char *sid = session_id();

   session_state_t state;
   session_state_load(&state, sid);

   if (argc < 1)
   {
      const char *m = state.tdd_mode[0] ? state.tdd_mode : "off";
      if (ctx->json_output)
         emit_ok_kv_ctx("tdd_mode", m, ctx->json_fields, ctx->response_profile);
      else
         printf("%s\n", m);
      return;
   }

   const char *sub = argv[0];
   if (strcmp(sub, "off") == 0)
   {
      snprintf(state.tdd_mode, sizeof(state.tdd_mode), "off");
   }
   else if (strcmp(sub, "warn") == 0)
   {
      snprintf(state.tdd_mode, sizeof(state.tdd_mode), "warn");
   }
   else if (strcmp(sub, "enforce") == 0)
   {
      snprintf(state.tdd_mode, sizeof(state.tdd_mode), "enforce");
   }
   else
   {
      fatal("aimee tdd: unknown subcommand '%s' (use: off, warn, enforce)", sub);
   }

   state.dirty = 1;
   session_state_force_save(&state, sid);

   if (ctx->json_output)
      emit_ok_kv_ctx("tdd_mode", state.tdd_mode, ctx->json_fields, ctx->response_profile);
   else
      fprintf(stderr, "TDD mode set to: %s\n", state.tdd_mode);
}

/* --- cmd_dashboard --- */

void cmd_env(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1 || strcmp(argv[0], "detect") == 0)
   {
      config_t db1_cfg;
      config_load(&db1_cfg);
      if (db1_init(db1_cfg.db1_path) != 0)
         fatal("env detect: could not initialize DB1");
      agent_introspect_env();
      /* Display results */
      db1_env_capability_t caps[64];
      int n = db1_env_capability_list(caps, 64);
      for (int i = 0; i < n; i++)
         printf("%-24s %s\n", caps[i].key, caps[i].value);
   }
}

/* --- cmd_contract --- */

void cmd_contract(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;

   /* Show the project contract for the current directory */
   char *contract = agent_load_project_contract(NULL);
   if (contract)
   {
      printf("%s\n", contract);
      free(contract);
   }
   else
   {
      printf("No project.yaml found in ~/.config/aimee/projects/<name>/ for this project.\n");
   }
}

/* --- cmd_workspace --- */

void cmd_notify(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   notify_config_t cfg;

   if (notify_config_load(&cfg) != 0)
   {
      memset(&cfg, 0, sizeof(cfg));
      cfg.verbosity = NOTIFY_VERBOSITY_STANDARD;
   }

   if (argc < 1 || strcmp(argv[0], "status") == 0)
   {
      const char *state = cfg.enabled ? "enabled" : "disabled";
      const char *verbosity = "standard";

      if (cfg.verbosity == NOTIFY_VERBOSITY_MINIMAL)
         verbosity = "minimal";
      else if (cfg.verbosity == NOTIFY_VERBOSITY_VERBOSE)
         verbosity = "verbose";

      fprintf(stderr, "Notifications: %s\n", state);
      fprintf(stderr, "Verbosity: %s\n", verbosity);
      fprintf(stderr, "Targets: %d\n", cfg.target_count);
      return;
   }

   if (strcmp(argv[0], "test") == 0)
   {
      event_notify(AIMEE_EVENT_JOB_COMPLETE, "aimee: test notification");
      fprintf(stderr, "Sent test notification.\n");
      return;
   }

   if (strcmp(argv[0], "off") == 0)
   {
      cfg.enabled = 0;
      if (notify_config_save(&cfg) != 0)
      {
         fprintf(stderr, "notify: failed to save notification config\n");
         return;
      }
      fprintf(stderr, "Notifications disabled.\n");
      return;
   }

   if (strcmp(argv[0], "on") == 0 || strcmp(argv[0], "--verbose") == 0)
   {
      memset(&cfg, 0, sizeof(cfg));
      cfg.enabled = 1;
      cfg.verbosity = (strcmp(argv[0], "--verbose") == 0) ? NOTIFY_VERBOSITY_VERBOSE
                                                          : NOTIFY_VERBOSITY_STANDARD;
      cfg.target_count = 1;
      cfg.targets[0].is_webhook = 0;
      cfg.targets[0].event_mask = 0;
      snprintf(cfg.targets[0].command, sizeof(cfg.targets[0].command), "%s",
               PLATFORM_DEFAULT_NOTIFY_CMD);
      if (notify_config_save(&cfg) != 0)
      {
         fprintf(stderr, "notify: failed to save notification config\n");
         return;
      }
      fprintf(stderr, "Notifications enabled.\n");
      return;
   }

   fprintf(stderr, "Usage: aimee notify [on|off|test|--verbose|status]\n");
}

/* --- cmd_clarify ---
 *
 * aimee clarify "vague task description"
 *   Start or continue a clarification session.
 *
 * aimee clarify status <id>
 *   Show session status, score, and pending question.
 *
 * aimee clarify answer <id> <answer text>
 *   Record an answer to the current open question.
 *
 * aimee clarify spec <id>
 *   Print the crystallized spec (only available once status=ready).
 */
void cmd_clarify(app_ctx_t *ctx, int argc, char **argv)
{
   config_t db1_cfg;
   config_load(&db1_cfg);
   if (db1_init(db1_cfg.db1_path) != 0)
      fatal("clarify: could not initialize DB1");

   if (argc == 0)
   {
      fprintf(stderr,
              "Usage: aimee clarify \"description\"        — start a clarification session\n"
              "       aimee clarify status <id>            — show session status\n"
              "       aimee clarify answer <id> <text>     — record an answer\n"
              "       aimee clarify spec <id>              — print crystallized spec\n");
      return;
   }

   /* aimee clarify status <id> */
   if (strcmp(argv[0], "status") == 0)
   {
      if (argc < 2)
         fatal("clarify status requires a session id");
      int id = atoi(argv[1]);
      clarify_session_t s;
      if (db1_clarify_get(id, &s) != 0)
         fatal("clarify: session %d not found", id);

      if (ctx->json_output)
      {
         char *json = db1_clarify_to_json(&s);
         if (json)
         {
            puts(json);
            free(json);
         }
      }
      else
      {
         const char *status_str = (s.status == CLARIFY_READY) ? "ready" : "open";
         printf("Session %d  status=%-8s  score=%.2f\n", s.id, status_str, s.score);
         for (int i = 0; i < s.qa_count; i++)
         {
            printf("  [%s] Q: %s\n", s.qa[i].dimension, s.qa[i].question);
            if (s.qa[i].answered)
               printf("       A: %s\n", s.qa[i].answer);
            else
               printf("       (awaiting answer)\n");
         }
      }
      return;
   }

   /* aimee clarify answer <id> <text> */
   if (strcmp(argv[0], "answer") == 0)
   {
      if (argc < 3)
         fatal("clarify answer requires an id and answer text");
      int id = atoi(argv[1]);
      const char *answer_text = argv[2];
      clarify_session_t s;
      if (db1_clarify_answer(id, answer_text, &s) != 0)
         fatal("clarify: failed to record answer for session %d", id);

      if (ctx->json_output)
      {
         char *json = db1_clarify_to_json(&s);
         if (json)
         {
            puts(json);
            free(json);
         }
      }
      else
      {
         if (s.status == CLARIFY_READY)
            printf("Session %d is now ready (score=%.2f). Run 'aimee clarify spec %d' to see the "
                   "spec.\n",
                   s.id, s.score, s.id);
         else
         {
            printf("Session %d score=%.2f — next question:\n", s.id, s.score);
            for (int i = 0; i < s.qa_count; i++)
               if (!s.qa[i].answered)
                  printf("  [%s] %s\n", s.qa[i].dimension, s.qa[i].question);
         }
      }
      return;
   }

   /* aimee clarify spec <id> */
   if (strcmp(argv[0], "spec") == 0)
   {
      if (argc < 2)
         fatal("clarify spec requires a session id");
      int id = atoi(argv[1]);
      clarify_session_t s;
      if (db1_clarify_get(id, &s) != 0)
         fatal("clarify: session %d not found", id);

      if (ctx->json_output)
      {
         cJSON *j = cJSON_CreateObject();
         cJSON_AddNumberToObject(j, "id", s.id);
         cJSON_AddStringToObject(j, "spec", s.spec);
         cJSON_AddStringToObject(j, "status", (s.status == CLARIFY_READY) ? "ready" : "open");
         char *json = cJSON_PrintUnformatted(j);
         cJSON_Delete(j);
         if (json)
         {
            puts(json);
            free(json);
         }
      }
      else
      {
         if (s.status != CLARIFY_READY || !s.spec[0])
            printf("Session %d is not yet ready (score=%.2f). Answer more questions first.\n", s.id,
                   s.score);
         else
            printf("%s\n", s.spec);
      }
      return;
   }

   /* aimee clarify "description" — start a new session */
   {
      const char *description = argv[0];
      clarify_session_t s;
      int id = db1_clarify_start(description, &s);
      if (id < 0)
         fatal("clarify: failed to create session");

      if (ctx->json_output)
      {
         char *json = db1_clarify_to_json(&s);
         if (json)
         {
            puts(json);
            free(json);
         }
      }
      else
      {
         printf("Started clarification session %d\n\n", s.id);
         for (int i = 0; i < s.qa_count; i++)
            if (!s.qa[i].answered)
               printf("[%s] %s\n\nAnswer with: aimee clarify answer %d \"your answer\"\n",
                      s.qa[i].dimension, s.qa[i].question, s.id);
      }
   }
}
