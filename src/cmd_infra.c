/* cmd_infra.c: infrastructure commands (git, worktree, dashboard, webchat, workspace) */
#include "aimee.h"
#include "db1.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "aux_router.h"
#include "kb_client.h"
#include "log.h"
#include <aimee/workspace/workspace.h>
#include "commands.h"
#include "dashboard.h"
#include "db1.h"
#include "memory.h"
#include "platform_process.h"
#include "platform_random.h"
#include "cJSON.h"
#include "modules/git/mcp_git.h"
#include "modules/git/git_verify.h"
#include "modules/workspace/workspace_manifest.h"
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

static cJSON *git_sub_status(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
{
   return handle_git_status(args);
}

static cJSON *git_sub_commit(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
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
      return NULL;
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

      /* Try aux router first (cheap local model when configured). aux_call reads
       * auxiliary.* itself and returns NULL when it is off, so the old
       * "does the CLI context carry a config" guard no longer decides anything. */
      msg = aux_call("commit_message", prompt, 128);

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
         return NULL;
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
   cJSON *resp = handle_git_commit(args);
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
   return resp;
}

static cJSON *git_sub_push(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
{
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)
         cJSON_AddBoolToObject(args, "force", 1);
   }
   return handle_git_push(args);
}

static cJSON *git_sub_verify(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
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
         return NULL;
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
      return NULL;
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
   return handle_git_verify(NULL, args, NULL);
}

static cJSON *git_sub_branch(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
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
            return NULL;
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
   return handle_git_branch(args);
}

static cJSON *git_sub_log(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
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
   return handle_git_log(args);
}

static cJSON *git_sub_diff(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
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
   return handle_git_diff_summary(args);
}

static cJSON *git_sub_pr(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
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
            return NULL;
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
            fprintf(stderr, "Usage: aimee git pr edit <number> [--title \"...\"] [--body \"...\"] "
                            "[--base \"...\"]\n");
            return NULL;
         }
         if (!title && !body && !base)
         {
            fprintf(stderr, "Usage: aimee git pr edit <number> [--title \"...\"] [--body \"...\"] "
                            "[--base \"...\"]\n");
            return NULL;
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
            return NULL;
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
   return handle_git_pr(args);
}

static cJSON *git_sub_issue(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
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
         return NULL;
      }
   }
   return handle_git_issue(args);
}

static cJSON *git_sub_pull(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
{
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--rebase") == 0 || strcmp(argv[i], "-r") == 0)
         cJSON_AddBoolToObject(args, "rebase", 1);
   }
   return handle_git_pull(args);
}

static cJSON *git_sub_fetch(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
{
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--prune") == 0 || strcmp(argv[i], "-p") == 0)
         cJSON_AddBoolToObject(args, "prune", 1);
      else if (argv[i][0] != '-')
         cJSON_AddStringToObject(args, "remote", argv[i]);
   }
   return handle_git_fetch(args);
}

static cJSON *git_sub_stash(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
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
   return handle_git_stash(args);
}

static cJSON *git_sub_tag(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
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
   return handle_git_tag(args);
}

static cJSON *git_sub_reset(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
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
   return handle_git_reset(args);
}

static cJSON *git_sub_restore(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
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
   return handle_git_restore(args);
}

static cJSON *git_sub_clone(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee git clone <url> [path] [-b branch] [--depth N]\n");
      return NULL;
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
   return handle_git_clone(args);
}

/* The history-integration commands. Each takes its target ref positionally and
 * continue/abort/skip as a bare word, because that is how they are spoken:
 * `aimee git merge origin/testing`, `aimee git rebase --continue`. */
static cJSON *git_sub_integrate(cJSON *args, int argc, char **argv, cJSON *(*handler)(cJSON *),
                                const char *what)
{
   for (int i = 0; i < argc; i++)
   {
      const char *a = argv[i];
      while (*a == '-')
         a++;
      if (strcmp(a, "continue") == 0 || strcmp(a, "abort") == 0 || strcmp(a, "skip") == 0)
         cJSON_AddStringToObject(args, "action", a);
      else if (strcmp(argv[i], "--keep-conflicts") == 0)
         cJSON_AddBoolToObject(args, "abort_on_conflict", 0);
      else if (argv[i][0] != '-')
         cJSON_AddStringToObject(args, "ref", argv[i]);
   }
   if (!cJSON_GetObjectItemCaseSensitive(args, "ref") &&
       !cJSON_GetObjectItemCaseSensitive(args, "action"))
   {
      fprintf(stderr,
              "Usage: aimee git %s <ref>            (start one)\n"
              "       aimee git %s continue|abort   (drive one that hit a conflict)\n"
              "Add --keep-conflicts to stop in the conflicted state instead of undoing.\n",
              what, what);
      return NULL;
   }
   return handler(args);
}

static cJSON *git_sub_merge(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
{
   return git_sub_integrate(args, argc, argv, handle_git_merge, "merge");
}

static cJSON *git_sub_rebase(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
{
   /* rebase's target is `base`; the handler accepts either name. */
   return git_sub_integrate(args, argc, argv, handle_git_rebase, "rebase");
}

static cJSON *git_sub_cherry_pick(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
{
   return git_sub_integrate(args, argc, argv, handle_git_cherry_pick, "cherry-pick");
}

static cJSON *git_sub_revert(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
{
   return git_sub_integrate(args, argc, argv, handle_git_revert, "revert");
}

static cJSON *git_sub_sync(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
{
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--merge") == 0)
         cJSON_AddStringToObject(args, "mode", "merge");
      else if (argv[i][0] != '-')
         cJSON_AddStringToObject(args, "base", argv[i]);
   }
   return handle_git_sync(args);
}

static cJSON *git_sub_add(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
{
   cJSON *files = NULL;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "-A") == 0 || strcmp(argv[i], "--all") == 0)
         cJSON_AddBoolToObject(args, "all", 1);
      else if (argv[i][0] != '-')
      {
         if (!files)
            files = cJSON_AddArrayToObject(args, "files");
         cJSON_AddItemToArray(files, cJSON_CreateString(argv[i]));
      }
   }
   if (!files && !cJSON_GetObjectItemCaseSensitive(args, "all"))
   {
      fprintf(stderr, "Usage: aimee git add <paths...>\n"
                      "       aimee git add -A\n");
      return NULL;
   }
   return handle_git_add(args);
}

static cJSON *git_sub_switch(app_ctx_t *ctx, cJSON *args, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee git switch <branch>\n");
      return NULL;
   }
   cJSON_AddStringToObject(args, "ref", argv[0]);
   return handle_git_switch(args);
}

typedef cJSON *(*git_sub_fn)(app_ctx_t *ctx, cJSON *args, int argc, char **argv);

static const struct
{
   const char *name;
   git_sub_fn fn;
} git_subcmds[] = {
    {"status", git_sub_status},
    {"commit", git_sub_commit},
    {"push", git_sub_push},
    {"verify", git_sub_verify},
    {"branch", git_sub_branch},
    {"log", git_sub_log},
    {"diff", git_sub_diff},
    {"pr", git_sub_pr},
    {"issue", git_sub_issue},
    {"pull", git_sub_pull},
    {"fetch", git_sub_fetch},
    {"stash", git_sub_stash},
    {"tag", git_sub_tag},
    {"reset", git_sub_reset},
    {"restore", git_sub_restore},
    {"clone", git_sub_clone},
    {"merge", git_sub_merge},
    {"rebase", git_sub_rebase},
    {"cherry-pick", git_sub_cherry_pick},
    {"cherry_pick", git_sub_cherry_pick},
    {"revert", git_sub_revert},
    {"sync", git_sub_sync},
    {"add", git_sub_add},
    {"switch", git_sub_switch},
    {NULL, NULL},
};

void cmd_git(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee git <status|commit|push|pull|fetch|branch|log|diff|"
                      "pr|issue|stash|tag|reset|restore|clone|verify|\n"
                      "                 merge|rebase|sync|cherry-pick|revert|add|switch>\n");
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

   git_sub_fn fn = NULL;
   for (int i = 0; git_subcmds[i].name; i++)
   {
      if (strcmp(sub, git_subcmds[i].name) == 0)
      {
         fn = git_subcmds[i].fn;
         break;
      }
   }

   cJSON *resp = NULL;
   if (fn)
      resp = fn(ctx, args, argc, argv);
   else
      fprintf(stderr, "Unknown git subcommand: %s\n", sub);

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
   /* Shared parser (space form only here before). Keep this function's -1
    * sentinel: its callers distinguish "no timestamp" from a real value. */
   time_t parsed = parse_utc_ts(s);
   return parsed > 0 ? parsed : (time_t)-1;
}

static void session_subcmd_list(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;

   {
      db1_init(config_db1_path());
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

   int threshold = (config_worktree_stale_secs() > 0) ? config_worktree_stale_secs()
                                                      : CONFIG_DEFAULT_STALE_SESSION_SECS;

   db1_init(config_db1_path());

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
         for (int j = 0; j < config_workspace_count(); j++)
         {
            char git_root[MAX_PATH_LEN];
            if (git_repo_root(config_workspaces(j), git_root, sizeof(git_root)) == 0)
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
   cmd_require_db1("session: could not initialize DB1");

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

#define WEBCHAT_SERVICE_NAME "aimee-runtime-web.service"
#define WEBCHAT_SERVICE_SRC  "systemd/aimee-runtime-web.service"
#define WEBCHAT_SERVICE_DEST "/etc/systemd/system/" WEBCHAT_SERVICE_NAME

/* Local webchat identity is authenticated by kernel Unix-socket peer credentials.
 * Keep this compatibility hook intentionally empty: older releases wrote the
 * shared proxy credential to both aimee.yaml and a systemd EnvironmentFile. */
static void webchat_provision_proxy_secret(void)
{
   return;
}

static void webchat_enable(void)
{
   /* Find the service file relative to a workspace root or CWD */
   char src[MAX_PATH_LEN];
   if (config_workspace_count() > 0)
      snprintf(src, sizeof(src), "%s/" WEBCHAT_SERVICE_SRC, config_workspaces(0));
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

static void webchat_enable_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   webchat_enable();
}

static void webchat_disable_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   webchat_disable();
}

static void webchat_status_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   webchat_status();
}

static const subcmd_t cmd_webchat_subs[] = {
    {"enable", "enable webchat", webchat_enable_cmd},
    {"disable", "disable webchat", webchat_disable_cmd},
    {"status", "show webchat status", webchat_status_cmd},
    {NULL, NULL, NULL},
};

void cmd_webchat(app_ctx_t *ctx, int argc, char **argv)
{
   const char *sub = argc > 0 ? argv[0] : NULL;
   if (argc > 0)
   {
      argc--;
      argv++;
   }

   if (!sub || subcmd_dispatch(cmd_webchat_subs, sub, ctx, argc, argv) != 0)
   {
      fprintf(stderr, "usage: aimee webchat <enable|disable|status>\n");
      fprintf(stderr, "       to run the server directly: aimee-runtime-web --port <port>\n");
   }
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
   int add_rc = config_workspace_add(abs_path, NULL, NULL, NULL);
   /* Already registered is the state the caller asked for, so it is not an
    * error. Failing here made `workspace add` non-idempotent, and scripted
    * setup issues it unconditionally: re-running any automation that had
    * already registered its path aborted at setup with exit 1, before doing
    * any work. Fall through to discovery so a repeat run still re-indexes,
    * which is the half of the command that actually matters on a second call.
    *
    * This is the same read-your-writes hazard documented for `workspace
    * remove` in server_state.c -- a state change a caller cannot immediately
    * act on is worse than a slow one. */
   if (add_rc == -2)
      fprintf(stderr, "workspace: already registered: %s (re-indexing)\n", abs_path);
   else if (add_rc == -3)
   {
      fprintf(stderr, "workspace: maximum workspace count reached (64)\n");
      return -1;
   }
   if (add_rc != 0 && add_rc != -2)
   {
      fprintf(stderr, "workspace: failed to save config\n");
      return -1;
   }

   char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
   int count = workspace_discover_projects(abs_path, MAX_WORKSPACE_DEPTH, projects,
                                           MAX_DISCOVERED_PROJECTS);
   if (count < 0)
   {
      fprintf(stderr, "workspace: discovery failed for %s\n", abs_path);
      return -1;
   }

   fprintf(stderr, "workspace: %s %s (%d project(s) discovered)\n",
           add_rc == -2 ? "re-indexed" : "added", abs_path, count);

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

   if (config_workspace_count() == 0)
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
      for (int w = 0; w < config_workspace_count(); w++)
      {
         cJSON *ws_obj = cJSON_CreateObject();
         cJSON_AddStringToObject(ws_obj, "path", config_workspaces(w));
         cJSON *projs = cJSON_AddArrayToObject(ws_obj, "projects");
         size_t ws_len = strlen(config_workspaces(w));
         for (int p = 0; p < pcount; p++)
         {
            if (strncmp(all_projects[p].root, config_workspaces(w), ws_len) == 0 &&
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
      for (int w = 0; w < config_workspace_count(); w++)
      {
         fprintf(stderr, "%s\n", config_workspaces(w));
         size_t ws_len = strlen(config_workspaces(w));
         for (int p = 0; p < pcount; p++)
         {
            if (strncmp(all_projects[p].root, config_workspaces(w), ws_len) == 0 &&
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

   /* Try to match by path (absolute or as provided) */
   char abs[MAX_PATH_LEN];
   const char *target = argv[0];
   if (realpath(argv[0], abs))
      target = abs;

   int rm_rc = config_workspace_remove(target);
   if (rm_rc == -2)
   {
      fprintf(stderr, "workspace: not found: %s\n", argv[0]);
      return;
   }
   if (rm_rc != 0)
   {
      fprintf(stderr, "workspace: failed to save config\n");
      return;
   }

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      fprintf(stderr, "workspace: removed %s\n", target);
}

static const subcmd_t cmd_workspace_subs[] = {
    {"add", "add a workspace", workspace_cmd_add},
    {"list", "list workspaces", workspace_cmd_list},
    {"remove", "remove a workspace", workspace_cmd_remove},
    {NULL, NULL, NULL},
};

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

   if (subcmd_dispatch(cmd_workspace_subs, sub, ctx, argc, argv) != 0)
   {
      fprintf(stderr, "Unknown workspace subcommand: %s\n", sub);
      fprintf(stderr, "Usage: aimee workspace <add|list|remove> [options]\n");
   }
}

/* --- aimee db --- */
