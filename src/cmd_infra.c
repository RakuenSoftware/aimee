/* cmd_infra.c: infrastructure commands (git, worktree, dashboard, webchat, workspace) */
#include "aimee.h"
#include "db1.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "aux_router.h"
#include "kb_client.h"
#include "log.h"
#include "workspace.h"
#include "commands.h"
#include "dashboard.h"
#include "db1.h"
#include "memory.h"
#include "platform_process.h"
#include "platform_random.h"
#include "cJSON.h"
#include "mcp_git.h"
#include "git_verify.h"
#include "workspace_manifest.h"
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>

/* Platform-specific background index scan (posix/cmd_infra.c) */
void platform_infra_background_scan(const char *cwd);

static const char *resolved_aimee_bin_path(void)
{
   static char path[MAX_PATH_LEN];
   if (path[0])
      return path;

   if (platform_get_exe_path(path, sizeof(path)) == 0)
   {
      char *base = strrchr(path, '/');
      base = base ? base + 1 : path;
      if (strcmp(base, "aimee") == 0 || strcmp(base, "aimee.exe") == 0 ||
          strcmp(base, "aimee-client") == 0 || strcmp(base, "aimee-client.exe") == 0)
         return path;
      if (strcmp(base, "aimee-server") == 0)
      {
         snprintf(base, sizeof(path) - (size_t)(base - path), "aimee");
         return path;
      }
      if (strcmp(base, "aimee-server.exe") == 0)
      {
         snprintf(base, sizeof(path) - (size_t)(base - path), "aimee.exe");
         return path;
      }
   }

   path[0] = '\0';
   const char *home = getenv("HOME");
   if (home)
      snprintf(path, sizeof(path), "%s/.local/bin/aimee", home);
   return path;
}

static void print_mcp_response(cJSON *resp)
{
   if (!resp || !cJSON_IsArray(resp))
      return;
   int count = cJSON_GetArraySize(resp);
   for (int i = 0; i < count; i++)
   {
      cJSON *item = cJSON_GetArrayItem(resp, i);
      cJSON *type = cJSON_GetObjectItem(item, "type");
      cJSON *text = cJSON_GetObjectItem(item, "text");
      if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 && cJSON_IsString(text))
         printf("%s\n", text->valuestring);
   }
}

void cmd_git(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee git <status|commit|push|pull|fetch|branch|log|diff|"
                      "pr|issue|stash|tag|reset|restore|clone|verify>\n");
      return;
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   cJSON *args = cJSON_CreateObject();
   char cwd[MAX_PATH_LEN];
   if (getcwd(cwd, sizeof(cwd)))
   {
      cJSON_AddStringToObject(args, "path", cwd);
      run_cmd_set_cwd(cwd);
   }
   cJSON *resp = NULL;

   if (strcmp(sub, "status") == 0)
   {
      resp = handle_git_status(args);
   }
   else if (strcmp(sub, "commit") == 0)
   {
      int auto_msg = 0;
      const char *msg = NULL;
      int files_start = 0;

      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--auto") == 0)
            auto_msg = 1;
         else if (!msg && argv[i][0] != '-')
         {
            msg = argv[i];
            files_start = i + 1;
         }
      }

      if (!msg && !auto_msg)
      {
         fprintf(stderr, "Usage: aimee git commit <message> [files...]\n");
         fprintf(stderr, "       aimee git commit --auto [files...]\n");
         cJSON_Delete(args);
         return;
      }

      if (auto_msg && !msg)
      {
         /* Generate message using delegate agent */
         fprintf(stderr, "Generating commit message...\n");
         cJSON *diff_args = cJSON_CreateObject();
         cJSON_AddBoolToObject(diff_args, "stat_only", 0);
         cJSON *diff_resp = handle_git_diff_summary(diff_args);
         cJSON_Delete(diff_args);

         char diff_text[4096] = "";
         if (diff_resp && cJSON_IsArray(diff_resp))
         {
            cJSON *item = cJSON_GetArrayItem(diff_resp, 0);
            cJSON *text = cJSON_GetObjectItem(item, "text");
            if (cJSON_IsString(text))
               snprintf(diff_text, sizeof(diff_text), "%s", text->valuestring);
         }
         cJSON_Delete(diff_resp);

         char prompt[5120];
         snprintf(prompt, sizeof(prompt),
                  "Generate a concise, one-line git commit message for these changes:\n\n%s\n\n"
                  "Output ONLY the message, no quotes or prefix.",
                  diff_text);

         /* Try aux router first (cheap local model when configured) */
         if (ctx->cfg)
            msg = aux_call(ctx->cfg, "commit_message", prompt, 128);

         /* Fall back to cheapest configured agent */
         if (!msg)
         {
            agent_config_t acfg;
            agent_result_t result;
            memset(&result, 0, sizeof(result));
            if (agent_load_config(&acfg) == 0)
            {
               agent_t *ag = &acfg.agents[0];
               if (agent_execute(ag, NULL, prompt, 128, 0.0, &result) == 0)
                  msg = result.response;
            }
         }
         if (msg)
            fprintf(stderr, "Auto-message: %s\n", msg);
         if (!msg)
         {
            fprintf(stderr, "Error: failed to generate commit message.\n");
            cJSON_Delete(args);
            return;
         }
      }

      cJSON_AddStringToObject(args, "message", msg);
      if (files_start > 0 && files_start < argc)
      {
         cJSON *files = cJSON_CreateArray();
         for (int i = files_start; i < argc; i++)
         {
            if (argv[i][0] != '-')
               cJSON_AddItemToArray(files, cJSON_CreateString(argv[i]));
         }
         cJSON_AddItemToObject(args, "files", files);
      }
      resp = handle_git_commit(args);
      if (auto_msg)
         free((void *)msg);

      /* Trigger background re-indexing if commit succeeded */
      if (resp && cJSON_IsArray(resp))
      {
         cJSON *item = cJSON_GetArrayItem(resp, 0);
         cJSON *text = cJSON_GetObjectItem(item, "text");
         if (cJSON_IsString(text) && strncmp(text->valuestring, "committed:", 10) == 0)
         {
            char cwd[MAX_PATH_LEN];
            if (getcwd(cwd, sizeof(cwd)))
            {
               platform_infra_background_scan(cwd);
            }
         }
      }
   }
   else if (strcmp(sub, "push") == 0)
   {
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)
            cJSON_AddBoolToObject(args, "force", 1);
      }
      resp = handle_git_push(args);
   }
   else if (strcmp(sub, "verify") == 0)
   {
      /* Unknown args must NOT fall through to a silent default, because the
       * default action is `run` which spawns a fresh (slow, parallel) build.
       * A typo like `aimee git verify --status 1` would otherwise kick off
       * another verify instead of polling.  Reject anything we don't
       * understand and print usage. */
      int parse_err = 0;
      for (int i = 0; i < argc && !parse_err; i++)
      {
         if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
         {
            fprintf(stderr, "Usage: aimee git verify [action=...] [key=value ...]\n"
                            "       aimee git verify --status <job_id>   (poll a background job)\n"
                            "Actions: run (default, async), status, check, env, conflicts,\n"
                            "         prepare-pr, install-hook\n"
                            "Flags:   --async=false  (force synchronous run)\n");
            cJSON_Delete(args);
            return;
         }

         /* --status <id> / --status=<id>: convenience shortcut for
          * action=status job_id=<id> */
         if (strcmp(argv[i], "--status") == 0)
         {
            if (i + 1 >= argc)
            {
               fprintf(stderr, "aimee git verify: --status requires a job id\n");
               parse_err = 1;
               break;
            }
            cJSON_AddStringToObject(args, "action", "status");
            cJSON_AddNumberToObject(args, "job_id", atoi(argv[++i]));
            continue;
         }

         if (strncmp(argv[i], "--", 2) == 0)
         {
            char *eq = strchr(argv[i] + 2, '=');
            if (!eq)
            {
               fprintf(stderr,
                       "aimee git verify: flag '%s' requires a value "
                       "(use '--flag=value' or 'key=value'). Run 'aimee git verify --help'.\n",
                       argv[i]);
               parse_err = 1;
               break;
            }
            *eq = '\0';
            const char *val = eq + 1;
            if (strcmp(val, "true") == 0)
               cJSON_AddBoolToObject(args, argv[i] + 2, 1);
            else if (strcmp(val, "false") == 0)
               cJSON_AddBoolToObject(args, argv[i] + 2, 0);
            else if (isdigit((unsigned char)val[0]))
               cJSON_AddNumberToObject(args, argv[i] + 2, atoi(val));
            else
               cJSON_AddStringToObject(args, argv[i] + 2, val);
            *eq = '=';
         }
         else
         {
            char *eq = strchr(argv[i], '=');
            if (!eq)
            {
               fprintf(stderr,
                       "aimee git verify: unexpected positional arg '%s' "
                       "(use 'key=value'). Run 'aimee git verify --help'.\n",
                       argv[i]);
               parse_err = 1;
               break;
            }
            *eq = '\0';
            const char *val = eq + 1;
            if (strcmp(val, "true") == 0)
               cJSON_AddBoolToObject(args, argv[i], 1);
            else if (strcmp(val, "false") == 0)
               cJSON_AddBoolToObject(args, argv[i], 0);
            else if (isdigit((unsigned char)val[0]))
               cJSON_AddNumberToObject(args, argv[i], atoi(val));
            else
               cJSON_AddStringToObject(args, argv[i], val);
            *eq = '=';
         }
      }
      if (parse_err)
      {
         cJSON_Delete(args);
         return;
      }

      /* This legacy command path is only safe as an in-process handler.  It
       * stores async job state in process-local static arrays, so callers that
       * need durable async verify status must use a typed server RPC instead.
       * Keep this path synchronous until that port exists. */
      if (!cJSON_GetObjectItem(args, "async"))
         cJSON_AddBoolToObject(args, "async", 0);

      /* CLI path: no server context available — verify_run_waves falls back
       * to an ephemeral pool. The vast majority of verify invocations route
       * through cli_v1_lookup and end up server-side via mcp.call (see
       * cli_v1_routes.inc), so this branch is only hit when running aimee
       * directly without a live server. */
      resp = handle_git_verify(NULL, args, NULL);
   }
   else if (strcmp(sub, "branch") == 0)
   {
      if (argc < 1)
      {
         cJSON_AddStringToObject(args, "action", "list");
      }
      else
      {
         const char *action = argv[0];
         if (strcmp(action, "list") == 0)
         {
            cJSON_AddStringToObject(args, "action", "list");
         }
         else if (strcmp(action, "create") == 0 || strcmp(action, "switch") == 0 ||
                  strcmp(action, "delete") == 0)
         {
            if (argc < 2)
            {
               fprintf(stderr, "Usage: aimee git branch %s <name> [base]\n", action);
               cJSON_Delete(args);
               return;
            }
            cJSON_AddStringToObject(args, "action", action);
            cJSON_AddStringToObject(args, "name", argv[1]);
            if (argc > 2 && strcmp(action, "create") == 0)
               cJSON_AddStringToObject(args, "base", argv[2]);
         }
         else
         {
            /* Assume single arg is 'switch' */
            cJSON_AddStringToObject(args, "action", "switch");
            cJSON_AddStringToObject(args, "name", action);
         }
      }
      resp = handle_git_branch(args);
   }
   else if (strcmp(sub, "log") == 0)
   {
      int count = 10;
      const char *ref = NULL;
      for (int i = 0; i < argc; i++)
      {
         if (isdigit(argv[i][0]))
            count = atoi(argv[i]);
         else if (strcmp(argv[i], "--stat") == 0)
            cJSON_AddBoolToObject(args, "diff_stat", 1);
         else
            ref = argv[i];
      }
      cJSON_AddNumberToObject(args, "count", count);
      if (ref)
         cJSON_AddStringToObject(args, "ref", ref);
      resp = handle_git_log(args);
   }
   else if (strcmp(sub, "diff") == 0)
   {
      int summary = 1;
      const char *ref = NULL;
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--full") == 0)
            summary = 0;
         else if (strcmp(argv[i], "--summary") == 0)
            summary = 1;
         else
            ref = argv[i];
      }
      cJSON_AddBoolToObject(args, "stat_only", summary);
      if (ref)
         cJSON_AddStringToObject(args, "ref", ref);
      resp = handle_git_diff_summary(args);
   }
   else if (strcmp(sub, "pr") == 0)
   {
      if (argc < 1)
      {
         cJSON_AddStringToObject(args, "action", "list");
      }
      else
      {
         const char *action = argv[0];
         cJSON_AddStringToObject(args, "action", action);
         if (strcmp(action, "create") == 0)
         {
            opt_parsed_t opts;
            opt_parse(argc - 1, argv + 1, NULL, &opts);
            const char *title = opt_get(&opts, "title");
            const char *body = opt_get(&opts, "body");
            const char *base = opt_get(&opts, "base");
            if (!title)
            {
               fprintf(stderr, "Usage: aimee git pr create --title \"...\" [--body \"...\"] "
                               "[--base \"...\"]\n");
               cJSON_Delete(args);
               return;
            }
            cJSON_AddStringToObject(args, "title", title);
            if (body)
               cJSON_AddStringToObject(args, "body", body);
            if (base)
               cJSON_AddStringToObject(args, "base", base);
         }
         else if (strcmp(action, "edit") == 0)
         {
            opt_parsed_t opts;
            opt_parse(argc - 2, argv + 2, NULL, &opts);
            const char *title = opt_get(&opts, "title");
            const char *body = opt_get(&opts, "body");
            const char *base = opt_get(&opts, "base");

            if (argc < 2)
            {
               fprintf(stderr,
                       "Usage: aimee git pr edit <number> [--title \"...\"] [--body \"...\"] "
                       "[--base \"...\"]\n");
               cJSON_Delete(args);
               return;
            }
            if (!title && !body && !base)
            {
               fprintf(stderr,
                       "Usage: aimee git pr edit <number> [--title \"...\"] [--body \"...\"] "
                       "[--base \"...\"]\n");
               cJSON_Delete(args);
               return;
            }

            cJSON_AddNumberToObject(args, "number", atoi(argv[1]));
            if (title)
               cJSON_AddStringToObject(args, "title", title);
            if (body)
               cJSON_AddStringToObject(args, "body", body);
            if (base)
               cJSON_AddStringToObject(args, "base", base);
         }
         else if (strcmp(action, "view") == 0 || strcmp(action, "merge_status") == 0 ||
                  strcmp(action, "checks") == 0 || strcmp(action, "watch") == 0 ||
                  strcmp(action, "wait") == 0)
         {
            const char *bool_flags[] = {"watch", "wait", NULL};
            opt_parsed_t opts;
            if (argc < 2)
            {
               fprintf(stderr, "Usage: aimee git pr %s <number>\n", action);
               cJSON_Delete(args);
               return;
            }
            cJSON_AddNumberToObject(args, "number", atoi(argv[1]));
            if (strcmp(action, "checks") == 0)
            {
               opt_parse(argc - 2, argv + 2, bool_flags, &opts);
               if (opt_has(&opts, "watch"))
                  cJSON_AddBoolToObject(args, "watch", 1);
               if (opt_has(&opts, "wait"))
                  cJSON_AddBoolToObject(args, "wait", 1);
            }
            else if (strcmp(action, "watch") == 0)
            {
               cJSON_AddBoolToObject(args, "watch", 1);
            }
            else if (strcmp(action, "wait") == 0)
            {
               cJSON_AddStringToObject(args, "action", "checks");
               cJSON_AddBoolToObject(args, "wait", 1);
            }
         }
      }
      resp = handle_git_pr(args);
   }
   else if (strcmp(sub, "issue") == 0)
   {
      if (argc < 1)
      {
         cJSON_AddStringToObject(args, "action", "list");
      }
      else
      {
         const char *action = argv[0];
         cJSON_AddStringToObject(args, "action", action);
         if (strcmp(action, "list") == 0)
         {
            opt_parsed_t opts;
            opt_parse(argc - 1, argv + 1, NULL, &opts);
            const char *state = opt_get(&opts, "state");
            if (state)
               cJSON_AddStringToObject(args, "state", state);
         }
         else
         {
            fprintf(stderr, "Usage: aimee git issue list [--state open|closed|all]\n");
            cJSON_Delete(args);
            return;
         }
      }
      resp = handle_git_issue(args);
   }
   else if (strcmp(sub, "pull") == 0)
   {
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--rebase") == 0 || strcmp(argv[i], "-r") == 0)
            cJSON_AddBoolToObject(args, "rebase", 1);
      }
      resp = handle_git_pull(args);
   }
   else if (strcmp(sub, "fetch") == 0)
   {
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--prune") == 0 || strcmp(argv[i], "-p") == 0)
            cJSON_AddBoolToObject(args, "prune", 1);
         else if (argv[i][0] != '-')
            cJSON_AddStringToObject(args, "remote", argv[i]);
      }
      resp = handle_git_fetch(args);
   }
   else if (strcmp(sub, "stash") == 0)
   {
      if (argc >= 1)
      {
         cJSON_AddStringToObject(args, "action", argv[0]);
         for (int i = 1; i < argc; i++)
         {
            if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
               cJSON_AddStringToObject(args, "message", argv[++i]);
            else if (isdigit(argv[i][0]))
               cJSON_AddNumberToObject(args, "index", atoi(argv[i]));
         }
      }
      else
      {
         cJSON_AddStringToObject(args, "action", "push");
      }
      resp = handle_git_stash(args);
   }
   else if (strcmp(sub, "tag") == 0)
   {
      if (argc < 1)
      {
         cJSON_AddStringToObject(args, "action", "list");
      }
      else if (strcmp(argv[0], "list") == 0)
      {
         cJSON_AddStringToObject(args, "action", "list");
      }
      else if (strcmp(argv[0], "delete") == 0)
      {
         cJSON_AddStringToObject(args, "action", "delete");
         if (argc > 1)
            cJSON_AddStringToObject(args, "name", argv[1]);
      }
      else
      {
         /* aimee git tag <name> [-m message] [ref] */
         cJSON_AddStringToObject(args, "action", "create");
         cJSON_AddStringToObject(args, "name", argv[0]);
         for (int i = 1; i < argc; i++)
         {
            if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
               cJSON_AddStringToObject(args, "message", argv[++i]);
            else if (argv[i][0] != '-')
               cJSON_AddStringToObject(args, "ref", argv[i]);
         }
      }
      resp = handle_git_tag(args);
   }
   else if (strcmp(sub, "reset") == 0)
   {
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--soft") == 0)
            cJSON_AddStringToObject(args, "mode", "soft");
         else if (strcmp(argv[i], "--mixed") == 0)
            cJSON_AddStringToObject(args, "mode", "mixed");
         else if (strcmp(argv[i], "--hard") == 0)
            cJSON_AddStringToObject(args, "mode", "hard");
         else if (argv[i][0] != '-')
            cJSON_AddStringToObject(args, "ref", argv[i]);
      }
      resp = handle_git_reset(args);
   }
   else if (strcmp(sub, "restore") == 0)
   {
      cJSON *files = cJSON_CreateArray();
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--staged") == 0 || strcmp(argv[i], "-S") == 0)
            cJSON_AddBoolToObject(args, "staged", 1);
         else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc)
            cJSON_AddStringToObject(args, "source", argv[++i]);
         else if (argv[i][0] != '-')
            cJSON_AddItemToArray(files, cJSON_CreateString(argv[i]));
      }
      if (cJSON_GetArraySize(files) > 0)
         cJSON_AddItemToObject(args, "files", files);
      else
         cJSON_Delete(files);
      resp = handle_git_restore(args);
   }
   else if (strcmp(sub, "clone") == 0)
   {
      if (argc < 1)
      {
         fprintf(stderr, "Usage: aimee git clone <url> [path] [-b branch] [--depth N]\n");
         cJSON_Delete(args);
         return;
      }
      cJSON_AddStringToObject(args, "url", argv[0]);
      for (int i = 1; i < argc; i++)
      {
         if ((strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--branch") == 0) && i + 1 < argc)
            cJSON_AddStringToObject(args, "branch", argv[++i]);
         else if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc)
            cJSON_AddNumberToObject(args, "depth", atoi(argv[++i]));
         else if (argv[i][0] != '-')
            cJSON_AddStringToObject(args, "path", argv[i]);
      }
      resp = handle_git_clone(args);
   }
   else
   {
      fprintf(stderr, "Unknown git subcommand: %s\n", sub);
   }

   if (resp)
   {
      print_mcp_response(resp);
      cJSON_Delete(resp);
   }
   cJSON_Delete(args);
}

/* Write .mcp.json in the given directory.
 * Uses "aimee-client mcp-serve" which proxies through aimee-server with session awareness. */
void ensure_mcp_json(const char *dir)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/.mcp.json", dir);

   /* Check if it already has the right content. Older .mcp.json files could
    * still point at aimee-server while mentioning the same mcp-serve arg;
    * those must be rewritten to the thin client entrypoint. */
   FILE *fp = fopen(path, "r");
   if (fp)
   {
      cJSON *root = NULL;
      fseek(fp, 0, SEEK_END);
      long sz = ftell(fp);
      fseek(fp, 0, SEEK_SET);
      if (sz > 0 && sz < (long)(1 << 20))
      {
         char *buf = malloc((size_t)sz + 1);
         if (buf)
         {
            size_t n = fread(buf, 1, (size_t)sz, fp);
            buf[n] = '\0';
            root = cJSON_Parse(buf);
            free(buf);
         }
      }
      fclose(fp);
      if (cJSON_IsObject(root))
      {
         cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
         cJSON *aimee =
             cJSON_IsObject(servers) ? cJSON_GetObjectItemCaseSensitive(servers, "aimee") : NULL;
         cJSON *cmd =
             cJSON_IsObject(aimee) ? cJSON_GetObjectItemCaseSensitive(aimee, "command") : NULL;
         cJSON *args =
             cJSON_IsObject(aimee) ? cJSON_GetObjectItemCaseSensitive(aimee, "args") : NULL;
         cJSON *arg0 = cJSON_IsArray(args) && cJSON_GetArraySize(args) == 1
                           ? cJSON_GetArrayItem(args, 0)
                           : NULL;
         int is_correct = cJSON_IsString(cmd) && strcmp(cmd->valuestring, aimee_bin) == 0 &&
                          cJSON_IsString(arg0) && strcmp(arg0->valuestring, "mcp-serve") == 0;
         cJSON_Delete(root);
         if (is_correct)
            return;
      }
   }

   fp = fopen(path, "w");
   if (!fp)
      return;
   fprintf(fp,
           "{\n"
           "  \"mcpServers\": {\n"
           "    \"aimee\": {\n"
           "      \"command\": \"%s\",\n"
           "      \"args\": [\"mcp-serve\"]\n"
           "    }\n"
           "  }\n"
           "}\n",
           aimee_bin);
   fclose(fp);
}

/* --- cmd_init --- */

/* --- cmd_session: list and clean up sessions and their worktrees --- */

#include <time.h>

/* Parse a DB timestamp ("YYYY-MM-DD HH:MM:SS") as UTC. Returns
 * (time_t)-1 on parse failure. */
static time_t parse_db_timestamp_utc(const char *s)
{
   if (!s || !s[0])
      return (time_t)-1;
   struct tm tmv;
   memset(&tmv, 0, sizeof(tmv));
   if (sscanf(s, "%d-%d-%d %d:%d:%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday, &tmv.tm_hour,
              &tmv.tm_min, &tmv.tm_sec) != 6)
      return (time_t)-1;
   tmv.tm_year -= 1900;
   tmv.tm_mon -= 1;
   return timegm(&tmv);
}

static void session_subcmd_list(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;

   {
      config_t cfg;
      config_load(&cfg);
      db1_init(cfg.db1_path);
   }

   db1_session_state_summary_t rows[128];
   int count = db1_session_state_list(rows, 128);
   time_t now = time(NULL);

   for (int i = 0; i < count; i++)
   {
      time_t t = parse_db_timestamp_utc(rows[i].updated_at);
      int age_mins = (t == (time_t)-1) ? -1 : (int)(difftime(now, t) / 60.0);
      int is_current = (strcmp(rows[i].session_id, session_id()) == 0);
      if (age_mins >= 0)
         printf("%.8s  %5dm ago%s\n", rows[i].session_id, age_mins,
                is_current ? "  [current]" : "");
      else
         printf("%.8s  unknown%s\n", rows[i].session_id, is_current ? "  [current]" : "");
   }

   if (count == 0)
      fprintf(stderr, "No active sessions.\n");
   else
      fprintf(stderr, "\n%d session(s)\n", count);

   {
      ensemble_info_t *rows = NULL;
      int wf_count = 0;
      char err[256] = "";

      if (db1_ensemble_list(&rows, &wf_count, err, sizeof(err)) == 0 && rows)
      {
         for (int i = 0; i < wf_count && i < 20; i++)
         {
            if (i == 0)
               printf("\nensembles:\n");
            printf("  #%d  %-8s %-18s channel=%s phase=%d turn=%d expected=%s\n", rows[i].id,
                   rows[i].status, rows[i].template_name, rows[i].channel,
                   rows[i].current_phase + 1, rows[i].current_turn + 1, rows[i].expected_agent);
         }
      }
      free(rows);
   }
}

static void session_subcmd_clean(app_ctx_t *ctx, int argc, char **argv)
{

   int dry_run = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--dry-run") == 0)
         dry_run = 1;
   }

   config_t cfg_buf;
   config_t *cfgp = ctx->cfg;
   if (!cfgp)
   {
      config_load(&cfg_buf);
      cfgp = &cfg_buf;
   }
   config_t cfg = *cfgp;

   int threshold =
       (cfg.worktree_stale_secs > 0) ? cfg.worktree_stale_secs : CONFIG_DEFAULT_STALE_SESSION_SECS;

   db1_init(cfg.db1_path);

   db1_session_state_summary_t rows[256];
   int total = db1_session_state_list(rows, 256);
   time_t now = time(NULL);
   int cleaned = 0;
   int skipped = 0;

   for (int i = 0; i < total; i++)
   {
      const char *sid = rows[i].session_id;
      if (!sid[0] || strcmp(sid, session_id()) == 0)
         continue; /* never clean the current session */

      time_t t = parse_db_timestamp_utc(rows[i].updated_at);
      double age_secs = (t == (time_t)-1) ? 0.0 : difftime(now, t);
      if (age_secs < (double)threshold)
      {
         skipped++;
         continue;
      }

      if (dry_run)
      {
         printf("would clean: %.8s (idle %.1fh)\n", sid, age_secs / 3600.0);
      }
      else
      {
         /* Remove sibling worktrees for this session */
         for (int j = 0; j < cfg.workspace_count; j++)
         {
            char git_root[MAX_PATH_LEN];
            if (git_repo_root(cfg.workspaces[j], git_root, sizeof(git_root)) == 0)
               worktree_cleanup(git_root, sid, NULL);
         }
         db1_session_state_delete(sid);
         printf("cleaned: %.8s (idle %.1fh)\n", sid, age_secs / 3600.0);
      }
      cleaned++;
   }

   fprintf(stderr, "%s%d session(s) %s, %d skipped (idle < %ds)\n", dry_run ? "[dry-run] " : "",
           cleaned, dry_run ? "would be cleaned" : "cleaned", skipped, threshold);
}

/* session show/search/stats handlers are in cmd_session_history.c */

static const subcmd_t session_subcmds[] = {
    {"list", "List active sessions", session_subcmd_list},
    {"clean", "Remove stale sessions and their worktrees [--dry-run]", session_subcmd_clean},
    {"start", "Alias of `ensemble start` (start a multi-agent ensemble)", session_subcmd_start},
    {"status", "Alias of `ensemble status`", session_subcmd_status},
    {"pause", "Alias of `ensemble pause`", session_subcmd_pause},
    {"advance", "Alias of `ensemble advance`", session_subcmd_advance},
    {"show", "Show session details and delegation timeline", session_subcmd_show},
    {"search", "Search session history by keyword", session_subcmd_search},
    {"stats", "Show session and delegation statistics [--since DATE]", session_subcmd_stats},
    {"tokens", "Show supervisor-vs-worker token split for a session [--json]",
     session_subcmd_tokens},
    {"brief", "Show the persisted session-start briefing [--session SID | --list]",
     session_subcmd_brief},
    {NULL, NULL, NULL},
};

const subcmd_t *get_session_subcmds(void)
{
   return session_subcmds;
}

void cmd_session(app_ctx_t *ctx, int argc, char **argv)
{
   config_t db1_cfg;
   config_load(&db1_cfg);
   if (db1_init(db1_cfg.db1_path) != 0)
      fatal("session: could not initialize DB1");

   const char *sub = (argc > 0) ? argv[0] : NULL;
   if (argc > 0)
   {
      argc--;
      argv++;
   }

   if (subcmd_dispatch(session_subcmds, sub, ctx, argc, argv) != 0)
      subcmd_usage("session", session_subcmds);
}

/* --- dashboard, webchat, workspace (moved from cmd_core.c) --- */

static void cmd_dashboard_cors(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee dashboard cors <add|remove|list> [origin]\n");
      exit(1);
   }

   const char *action = argv[0];

   if (strcmp(action, "list") == 0)
   {
      char origins[32][CORS_ORIGIN_LEN];
      int count = dashboard_cors_list(origins, 32);
      if (count == 0)
      {
         printf("No CORS origins configured (localhost-only access).\n");
         return;
      }
      printf("Allowed CORS origins:\n");
      for (int i = 0; i < count; i++)
         printf("  %s\n", origins[i]);
   }
   else if (strcmp(action, "add") == 0)
   {
      if (argc < 2)
         fatal("usage: aimee dashboard cors add <origin>");
      if (dashboard_cors_add(argv[1]) == 0)
         printf("Added CORS origin: %s\n", argv[1]);
      else
         fatal("failed to add origin (max %d reached?)", 32);
   }
   else if (strcmp(action, "remove") == 0)
   {
      if (argc < 2)
         fatal("usage: aimee dashboard cors remove <origin>");
      if (dashboard_cors_remove(argv[1]) == 0)
         printf("Removed CORS origin: %s\n", argv[1]);
      else
         fatal("origin not found: %s", argv[1]);
   }
   else
   {
      fatal("unknown cors action: %s (use add, remove, or list)", action);
   }
}

void cmd_dashboard(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   /* Handle subcommand: aimee dashboard cors ... */
   if (argc >= 1 && strcmp(argv[0], "cors") == 0)
   {
      cmd_dashboard_cors(ctx, argc - 1, argv + 1);
      return;
   }

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   int port = opt_get_int(&opts, "port", 0);
   dashboard_serve(port);
}

/* --- cmd_webchat --- */

#define WEBCHAT_SERVICE_NAME "aimee-webchat.service"
#define WEBCHAT_SERVICE_SRC  "systemd/aimee-webchat.service"
#define WEBCHAT_SERVICE_DEST "/etc/systemd/system/" WEBCHAT_SERVICE_NAME

/* Provision the ingress trusted-proxy secret so the webchat OpenAI proxy actually
 * stamps trusted principal/source metadata in a normal deployment (otherwise the
 * proxy strips identity headers but stamps nothing, since AIMEE_INGRESS_PROXY_SECRET
 * is unset). Generates a per-deployment secret if one is not already configured,
 * persists it to aimee config (so aimee-server trusts it — it reads the secret
 * from config), writes a 0600 EnvironmentFile carrying AIMEE_INGRESS_PROXY_SECRET,
 * and drops a systemd unit override that loads it for aimee-webchat. Best-effort:
 * logs and continues on failure rather than blocking enable. */
static void webchat_provision_proxy_secret(void)
{
   config_t cfg;
   config_load(&cfg);
   if (!cfg.ingress_trusted_proxy_secret[0])
   {
      char secret[33];
      if (platform_random_hex(secret, 32) != 0)
      {
         fprintf(stderr, "webchat: could not generate ingress proxy secret; "
                         "trusted attribution will be off until configured\n");
         return;
      }
      snprintf(cfg.ingress_trusted_proxy_secret, sizeof(cfg.ingress_trusted_proxy_secret), "%s",
               secret);
      if (config_save(&cfg) != 0)
         fprintf(stderr, "webchat: warning: failed to persist ingress proxy secret to config\n");
   }

   /* EnvironmentFile carrying the secret for the webchat process. */
   char envpath[MAX_PATH_LEN];
   snprintf(envpath, sizeof(envpath), "%s/ingress-proxy.env", config_default_dir());
   FILE *ef = fopen(envpath, "w");
   if (ef)
   {
      fprintf(ef, "AIMEE_INGRESS_PROXY_SECRET=%s\n", cfg.ingress_trusted_proxy_secret);
      fclose(ef);
      chmod(envpath, 0600);
   }
   else
   {
      fprintf(stderr, "webchat: warning: could not write %s\n", envpath);
      return;
   }

   /* systemd drop-in so the installed unit loads the EnvironmentFile. */
   const char *dropdir = "/etc/systemd/system/" WEBCHAT_SERVICE_NAME ".d";
   mkdir(dropdir, 0755);
   char dropconf[MAX_PATH_LEN];
   snprintf(dropconf, sizeof(dropconf), "%s/10-ingress-proxy.conf", dropdir);
   FILE *dc = fopen(dropconf, "w");
   if (dc)
   {
      fprintf(dc, "[Service]\nEnvironmentFile=%s\n", envpath);
      fclose(dc);
   }
   else
   {
      fprintf(stderr, "webchat: warning: could not write %s (run as root?)\n", dropconf);
   }
}

static void webchat_enable(void)
{
   /* Find the service file relative to a workspace root or CWD */
   char src[MAX_PATH_LEN];
   config_t ws_cfg;
   config_load(&ws_cfg);
   if (ws_cfg.workspace_count > 0)
      snprintf(src, sizeof(src), "%s/" WEBCHAT_SERVICE_SRC, ws_cfg.workspaces[0]);
   else
      snprintf(src, sizeof(src), WEBCHAT_SERVICE_SRC);

   /* Copy service file to systemd */
   const char *cp_argv[] = {"cp", src, WEBCHAT_SERVICE_DEST, NULL};
   char *out = NULL;
   int rc = safe_exec_capture(cp_argv, &out, 1024);
   free(out);
   if (rc != 0)
   {
      fprintf(stderr, "webchat: failed to copy service file (run as root?)\n");
      return;
   }

   /* Provision the trusted-proxy secret + unit override so trusted principal/
    * source metadata is stamped (proposal #3). */
   webchat_provision_proxy_secret();

   /* Reload systemd, enable, and start */
   const char *reload[] = {"systemctl", "daemon-reload", NULL};
   out = NULL;
   safe_exec_capture(reload, &out, 1024);
   free(out);

   const char *enable[] = {"systemctl", "enable", WEBCHAT_SERVICE_NAME, NULL};
   out = NULL;
   safe_exec_capture(enable, &out, 1024);
   free(out);

   const char *start[] = {"systemctl", "start", WEBCHAT_SERVICE_NAME, NULL};
   out = NULL;
   rc = safe_exec_capture(start, &out, 1024);
   free(out);

   if (rc == 0)
      fprintf(stderr, "webchat: enabled and started\n");
   else
      fprintf(stderr, "webchat: enabled but failed to start (check journalctl)\n");
}

static void webchat_disable(void)
{
   const char *stop[] = {"systemctl", "stop", WEBCHAT_SERVICE_NAME, NULL};
   char *out = NULL;
   safe_exec_capture(stop, &out, 1024);
   free(out);

   const char *disable[] = {"systemctl", "disable", WEBCHAT_SERVICE_NAME, NULL};
   out = NULL;
   safe_exec_capture(disable, &out, 1024);
   free(out);

   fprintf(stderr, "webchat: stopped and disabled\n");
}

static void webchat_status(void)
{
   const char *status[] = {"systemctl", "is-active", WEBCHAT_SERVICE_NAME, NULL};
   char *out = NULL;
   int rc = safe_exec_capture(status, &out, 256);

   if (rc == 0 && out)
   {
      /* Strip trailing newline */
      size_t len = strlen(out);
      while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
         out[--len] = '\0';
      fprintf(stderr, "webchat: %s\n", out);
   }
   else
   {
      fprintf(stderr, "webchat: not running\n");
   }
   free(out);
}

void cmd_webchat(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc >= 1 && strcmp(argv[0], "enable") == 0)
   {
      webchat_enable();
      return;
   }
   if (argc >= 1 && strcmp(argv[0], "disable") == 0)
   {
      webchat_disable();
      return;
   }
   if (argc >= 1 && strcmp(argv[0], "status") == 0)
   {
      webchat_status();
      return;
   }

   fprintf(stderr, "usage: aimee webchat <enable|disable|status>\n");
   fprintf(stderr, "       to run the server directly: aimee-webchat --port <port>\n");
}

/* --- cmd_env --- */

/* Derive a local directory name from a git URL, e.g.
 * "https://github.com/user/repo.git" -> "repo". */
static void repo_name_from_url(const char *url, char *out, size_t outlen)
{
   const char *slash = strrchr(url, '/');
   const char *start = slash ? slash + 1 : url;
   snprintf(out, outlen, "%s", start);
   /* Strip trailing .git */
   size_t n = strlen(out);
   if (n > 4 && strcmp(out + n - 4, ".git") == 0)
      out[n - 4] = '\0';
}

/* Register abs_path as a workspace, discover + index projects, and print results.
 * Shared by both the plain-path and --repo flows. Returns project count or -1. */
static int register_and_index(app_ctx_t *ctx, const char *abs_path)
{
   config_t cfg;
   config_load(&cfg);

   for (int i = 0; i < cfg.workspace_count; i++)
   {
      if (strcmp(cfg.workspaces[i], abs_path) == 0)
      {
         fprintf(stderr, "workspace: already registered: %s\n", abs_path);
         return -1;
      }
   }

   if (cfg.workspace_count >= 64)
   {
      fprintf(stderr, "workspace: maximum workspace count reached (64)\n");
      return -1;
   }

   snprintf(cfg.workspaces[cfg.workspace_count++], MAX_PATH_LEN, "%s", abs_path);
   config_save(&cfg);

   char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
   int count = workspace_discover_projects(abs_path, MAX_WORKSPACE_DEPTH, projects,
                                           MAX_DISCOVERED_PROJECTS);
   if (count < 0)
   {
      fprintf(stderr, "workspace: discovery failed for %s\n", abs_path);
      return -1;
   }

   fprintf(stderr, "workspace: added %s (%d project(s) discovered)\n", abs_path, count);

   for (int i = 0; i < count; i++)
   {
      const char *name = strrchr(projects[i], '/');
      name = name ? name + 1 : projects[i];
      fprintf(stderr, "  indexing: %s\n", name);
      kb_client_index_scan_result_t res;
      if (kb_client_index_scan(name, projects[i], 0, &res) != 0)
         fprintf(stderr, "    knowledge service unavailable — skipped\n");
      else if (res.skipped)
         fprintf(stderr, "    skipped (%s)\n", res.reason[0] ? res.reason : "unknown");
   }

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "path", abs_path);
      cJSON_AddNumberToObject(obj, "projects", count);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      for (int i = 0; i < count; i++)
      {
         const char *name = strrchr(projects[i], '/');
         name = name ? name + 1 : projects[i];
         fprintf(stderr, "  %s\n", name);
      }
   }
   return count;
}

static void workspace_cmd_add(app_ctx_t *ctx, int argc, char **argv)
{
   /* Parse flags: --repo <url> [--path <dest>] */
   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *repo_url = opt_get(&opts, "repo");

   if (repo_url)
   {
      /* --repo <url>: clone the repository then register it */
      char dest[MAX_PATH_LEN];
      const char *dest_opt = opt_get(&opts, "path");
      if (dest_opt)
      {
         /* Resolve relative dest against CWD */
         if (dest_opt[0] == '/')
            snprintf(dest, sizeof(dest), "%s", dest_opt);
         else
         {
            char cwd[MAX_PATH_LEN];
            if (!getcwd(cwd, sizeof(cwd)))
            {
               fprintf(stderr, "workspace: cannot determine current directory\n");
               return;
            }
            snprintf(dest, sizeof(dest), "%s/%s", cwd, dest_opt);
         }
      }
      else
      {
         /* Derive destination from URL */
         char name[256];
         repo_name_from_url(repo_url, name, sizeof(name));
         if (!name[0])
         {
            fprintf(stderr, "workspace: cannot derive repo name from URL: %s\n", repo_url);
            return;
         }
         char cwd[MAX_PATH_LEN];
         if (!getcwd(cwd, sizeof(cwd)))
         {
            fprintf(stderr, "workspace: cannot determine current directory\n");
            return;
         }
         snprintf(dest, sizeof(dest), "%s/%s", cwd, name);
      }

      /* Refuse to clobber an existing path */
      struct stat st;
      if (stat(dest, &st) == 0)
      {
         fprintf(stderr, "workspace: destination already exists: %s\n", dest);
         return;
      }

      /* Run git clone */
      char clone_cmd[MAX_PATH_LEN + 600];
      snprintf(clone_cmd, sizeof(clone_cmd), "git clone -- %s %s", repo_url, dest);
      fprintf(stderr, "workspace: cloning %s -> %s\n", repo_url, dest);
      int exit_code = 0;
      char *output = run_cmd(clone_cmd, &exit_code);
      if (output)
      {
         if (output[0])
            fprintf(stderr, "%s", output);
         free(output);
      }
      if (exit_code != 0)
      {
         fprintf(stderr, "workspace: git clone failed (exit %d)\n", exit_code);
         return;
      }

      register_and_index(ctx, dest);
      return;
   }

   /* Plain path */
   if (opt_pos(&opts, 0) == NULL)
   {
      fprintf(stderr, "Usage: aimee workspace add <path>\n");
      fprintf(stderr, "       aimee workspace add --repo <url> [--path <dest>]\n");
      return;
   }

   char abs[MAX_PATH_LEN];
   if (!realpath(opt_pos(&opts, 0), abs))
   {
      fprintf(stderr, "workspace: cannot resolve path: %s\n", opt_pos(&opts, 0));
      return;
   }

   struct stat st;
   if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode))
   {
      fprintf(stderr, "workspace: not a directory: %s\n", abs);
      return;
   }

   register_and_index(ctx, abs);
}

static void workspace_cmd_list(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   config_t cfg;
   config_load(&cfg);

   if (cfg.workspace_count == 0)
   {
      fprintf(stderr, "No workspaces configured. Use 'aimee workspace add <path>' to add one.\n");
      return;
   }

   project_info_t all_projects[256];
   int pcount = index_list_projects(all_projects, 256);
   if (pcount < 0)
      pcount = 0;

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int w = 0; w < cfg.workspace_count; w++)
      {
         cJSON *ws_obj = cJSON_CreateObject();
         cJSON_AddStringToObject(ws_obj, "path", cfg.workspaces[w]);
         cJSON *projs = cJSON_AddArrayToObject(ws_obj, "projects");
         size_t ws_len = strlen(cfg.workspaces[w]);
         for (int p = 0; p < pcount; p++)
         {
            if (strncmp(all_projects[p].root, cfg.workspaces[w], ws_len) == 0 &&
                (all_projects[p].root[ws_len] == '/' || all_projects[p].root[ws_len] == '\0'))
            {
               cJSON_AddItemToArray(projs, cJSON_CreateString(all_projects[p].name));
            }
         }
         cJSON_AddItemToArray(arr, ws_obj);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      for (int w = 0; w < cfg.workspace_count; w++)
      {
         fprintf(stderr, "%s\n", cfg.workspaces[w]);
         size_t ws_len = strlen(cfg.workspaces[w]);
         for (int p = 0; p < pcount; p++)
         {
            if (strncmp(all_projects[p].root, cfg.workspaces[w], ws_len) == 0 &&
                (all_projects[p].root[ws_len] == '/' || all_projects[p].root[ws_len] == '\0'))
            {
               fprintf(stderr, "  %s\n", all_projects[p].name);
            }
         }
      }
   }
}

static void workspace_cmd_remove(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee workspace remove <path>\n");
      return;
   }

   config_t cfg;
   config_load(&cfg);

   /* Try to match by path (absolute or as provided) */
   char abs[MAX_PATH_LEN];
   const char *target = argv[0];
   if (realpath(argv[0], abs))
      target = abs;

   int found = -1;
   for (int i = 0; i < cfg.workspace_count; i++)
   {
      if (strcmp(cfg.workspaces[i], target) == 0)
      {
         found = i;
         break;
      }
   }

   if (found < 0)
   {
      fprintf(stderr, "workspace: not found: %s\n", argv[0]);
      return;
   }

   /* Shift remaining entries down */
   for (int i = found; i < cfg.workspace_count - 1; i++)
      snprintf(cfg.workspaces[i], MAX_PATH_LEN, "%s", cfg.workspaces[i + 1]);
   cfg.workspace_count--;
   config_save(&cfg);

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      fprintf(stderr, "workspace: removed %s\n", target);
}

void cmd_workspace(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee workspace <add|list|remove> [options]\n");
      return;
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   if (strcmp(sub, "add") == 0)
      workspace_cmd_add(ctx, argc, argv);
   else if (strcmp(sub, "list") == 0)
      workspace_cmd_list(ctx, argc, argv);
   else if (strcmp(sub, "remove") == 0)
      workspace_cmd_remove(ctx, argc, argv);
   else
   {
      fprintf(stderr, "Unknown workspace subcommand: %s\n", sub);
      fprintf(stderr, "Usage: aimee workspace <add|list|remove> [options]\n");
   }
}

/* --- aimee db --- */
