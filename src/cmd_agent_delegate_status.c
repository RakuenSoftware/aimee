/* cmd_agent_delegate_status.c: background delegate status CLI command */
#include "aimee.h"
#include "commands.h"
#include "cmd_agent_delegate_impl.h"
#include "db1.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

/* `aimee delegate` usage/help. Lives here (not in cmd_agent_delegate.c) to keep
 * that translation unit under the 2000-line size cap. */
void delegate_print_help(void)
{
   fprintf(stderr,
           "Usage: aimee delegate <role> [\"prompt\"] --persona NAME [options]\n\n"
           "Delegate a bounded task to a sub-agent. A persona is required.\n\n"
           "Options:\n"
           "  --persona NAME     REQUIRED. Run the delegate as a persona (e.g. engineer, qa,\n"
           "                     security, reviewer, architect, or a custom persona): sets its\n"
           "                     identity + principles\n"
           "  --json             Output result as JSON\n"
           "  --background       Run asynchronously (returns task ID)\n"
           "  --durable          Persist result to disk\n"
           "  --tools[=TOOLSET]  Enable tool use, optionally scoped to a named toolset\n"
           "  --files F          Preload comma-separated file contents\n"
           "  --context-file F   Preload a specific file (repeatable)\n"
           "  --context SYMS     Preload code for comma-separated symbol names via the index\n"
           "  --prompt-file PATH Read user prompt from file (avoids ARG_MAX)\n"
           "  --prompt-stdin     Read user prompt from stdin (safe for shell metacharacters)\n"
           "  --context-dir DIR  Include directory contents as context\n"
           "  --output PATH      Write response to file instead of stdout\n"
           "  --system S         Override system prompt\n"
           "  --max-tokens N     Limit response tokens\n"
           "  --max-turns N      Override the delegate turn limit\n"
           "  --token-budget N   Max tokens for system prompt (overrides project.yaml)\n"
           "  --timeout N        Timeout in milliseconds\n"
           "  --retry N          Retry on failure (up to N times)\n"
           "  --verify CMD       Run verification command after completion\n"
           "  --handoff-json     Require delegate_result_v1 JSON handoff output\n"
           "  --loop             Self-correcting loop: re-run until complete\n"
           "  --max-iter N       Max loop iterations (default 5, implies --loop)\n"
           "  --completion-threshold N  Stop when self-assessed >= N%% (default 95)\n"
           "  --worktree BRANCH  Check out BRANCH in the session worktree\n"
           "  --coordination     Enable multi-agent coordination\n"
           "  --vote N           Run N agents and pick best result\n"
           "  --via AGENT        Route to a specific agent by name (e.g. --via codex)\n"
           "  --provider NAME    Route to the cheapest matching agent for a provider\n"
           "  --model NAME       Override the routed agent's model for this run\n"
           "  --tier N           Route to the best agent at cost tier N\n"
           "  --plan             Use plan-mode execution\n"
           "  --dry-run          Show what would be sent without executing\n"
           "\n"
           "Subcommands:\n"
           "  aimee delegate plan <proposal.md>  Generate read-only work packets\n"
           "  aimee delegate launch <plan.json>  Queue reviewed packets into a coord job\n"
           "  aimee delegate aggregate \"<task>\"  MoA ensemble: diverse models + synthesis\n"
           "  aimee delegate roundtable \"<task>\"  Multi-round collaborative drafting/review\n"
           "  aimee delegate status <background_task_id>   Check background delegate status\n"
           "  aimee delegate --list-roles        List available roles\n"
           "\n"
           "Examples:\n"
           "  aimee delegate code \"Add error handling to src/config.c\" --persona engineer\n"
           "  aimee delegate review \"Review PR #85 for security issues\" --persona security\n"
           "  aimee delegate code --via mistral-plan --persona engineer \"Inspect the change and "
           "summarize the implementation surface\"\n"
           "  aimee delegate summarize --persona engineer --prompt-file /tmp/prompt.txt\n"
           "  printf 'Review `git diff` output' | aimee delegate review --persona reviewer "
           "--prompt-stdin\n"
           "  aimee delegate execute --tools --persona engineer \"Check nginx status on "
           "wol-web\"\n");
}

static int delegate_status_parse_positive_int(const char *s)
{
   if (!s || !s[0])
      return 0;

   errno = 0;
   char *end = NULL;
   long value = strtol(s, &end, 10);
   if (errno != 0 || end == s || *end != '\0' || value <= 0 || value > INT_MAX)
      return 0;
   return (int)value;
}

static int delegate_status_coord_job_id(const char *task_id, const cJSON *root)
{
   if (root)
   {
      const cJSON *coord_job_id = cJSON_GetObjectItem(root, "coord_job_id");
      if (coord_job_id && cJSON_IsNumber(coord_job_id) && coord_job_id->valueint > 0)
         return coord_job_id->valueint;
      if (coord_job_id && cJSON_IsString(coord_job_id) && coord_job_id->valuestring)
      {
         int parsed = delegate_status_parse_positive_int(coord_job_id->valuestring);
         if (parsed > 0)
            return parsed;
      }
   }

   if (!task_id || !task_id[0])
      return 0;
   for (const char *p = task_id; *p; p++)
      if (!isdigit((unsigned char)*p))
         return 0;

   return delegate_status_parse_positive_int(task_id);
}

static void delegate_status_print_coord_hint_if_present(const char *task_id, const cJSON *root)
{
   int job_id = delegate_status_coord_job_id(task_id, root);
   if (job_id <= 0)
      return;

   config_t cfg;
   if (config_load(&cfg) != 0 || db1_init(cfg.db1_path) != 0)
      return;

   db1_coord_job_t job;
   if (db1_coord_job_get(job_id, &job) == 0)
   {
      printf("coord_job_hint: coordinated job #%d also exists; inspect it with `aimee job status "
             "%d`\n",
             job_id, job_id);
   }
}

void cmd_delegate_status(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1 || strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0)
      fatal("usage: aimee delegate status <background_task_id> [--full|--response-limit N]");

   const char *task_id = argv[0];
   int response_limit = 200;
   int full_response = 0;
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--full") == 0)
      {
         full_response = 1;
         response_limit = -1;
      }
      else if (strcmp(argv[i], "--response-limit") == 0)
      {
         if (i + 1 >= argc)
            fatal("usage: aimee delegate status <background_task_id> [--full|--response-limit N]");
         response_limit = atoi(argv[++i]);
         if (response_limit < 0)
            response_limit = 0;
         full_response = 0;
      }
      else
      {
         fatal("usage: aimee delegate status <background_task_id> [--full|--response-limit N]");
      }
   }
   char result_path[MAX_PATH_LEN];
   snprintf(result_path, sizeof(result_path), "%s/tasks/%s.json", config_default_dir(), task_id);

   struct stat st;
   if (stat(result_path, &st) != 0)
   {
      char pid_path[MAX_PATH_LEN];
      snprintf(pid_path, sizeof(pid_path), "%s/tasks/%s.pid", config_default_dir(), task_id);
      if (stat(pid_path, &st) == 0)
         printf("status: running\ntask_id: %s\n", task_id);
      else
         printf("status: not_found\ntask_id: %s\n", task_id);
      delegate_status_print_coord_hint_if_present(task_id, NULL);
      return;
   }

   FILE *f = fopen(result_path, "r");
   if (!f)
   {
      printf("status: unknown\ntask_id: %s\n", task_id);
      return;
   }

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > 1024 * 1024)
   {
      fclose(f);
      printf("status: error\ntask_id: %s\n", task_id);
      return;
   }

   char *data = malloc((size_t)sz + 1);
   if (!data)
   {
      fclose(f);
      return;
   }
   size_t nread = fread(data, 1, (size_t)sz, f);
   data[nread] = '\0';
   fclose(f);

   cJSON *root = cJSON_Parse(data);
   free(data);
   if (!root)
   {
      printf("status: error\ntask_id: %s\n", task_id);
      return;
   }

   cJSON *success = cJSON_GetObjectItem(root, "success");
   cJSON *turns = cJSON_GetObjectItem(root, "turns");
   cJSON *tool_calls = cJSON_GetObjectItem(root, "tool_calls");
   cJSON *response = cJSON_GetObjectItem(root, "response");
   cJSON *error = cJSON_GetObjectItem(root, "error");
   cJSON *launch_worktree_path = cJSON_GetObjectItem(root, "launch_worktree_path");
   cJSON *launch_head = cJSON_GetObjectItem(root, "launch_head");
   cJSON *finish_head = cJSON_GetObjectItem(root, "finish_head");
   cJSON *checkout_drift = cJSON_GetObjectItem(root, "checkout_drift");
   cJSON *parent_worktree_path = cJSON_GetObjectItem(root, "parent_worktree_path");
   cJSON *parent_worktree_head = cJSON_GetObjectItem(root, "parent_worktree_head");
   cJSON *parent_worktree_finish_head = cJSON_GetObjectItem(root, "parent_worktree_finish_head");
   cJSON *parent_worktree_drift = cJSON_GetObjectItem(root, "parent_worktree_drift");
   cJSON *parent_worktree_fingerprint = cJSON_GetObjectItem(root, "parent_worktree_fingerprint");
   cJSON *parent_worktree_finish_fingerprint =
       cJSON_GetObjectItem(root, "parent_worktree_finish_fingerprint");
   cJSON *parent_worktree_status_drift = cJSON_GetObjectItem(root, "parent_worktree_status_drift");

   printf("status: done\ntask_id: %s\n", task_id);
   if (success)
      printf("success: %s\n", cJSON_IsTrue(success) ? "true" : "false");
   if (turns && cJSON_IsNumber(turns))
      printf("turns: %d\n", turns->valueint);
   if (tool_calls && cJSON_IsNumber(tool_calls))
      printf("tool_calls: %d\n", tool_calls->valueint);
   if (launch_worktree_path && cJSON_IsString(launch_worktree_path) &&
       launch_worktree_path->valuestring)
      printf("launch_worktree_path: %s\n", launch_worktree_path->valuestring);
   if (launch_head && cJSON_IsString(launch_head) && launch_head->valuestring)
      printf("launch_head: %s\n", launch_head->valuestring);
   if (finish_head && cJSON_IsString(finish_head) && finish_head->valuestring)
      printf("finish_head: %s\n", finish_head->valuestring);
   if (parent_worktree_path && cJSON_IsString(parent_worktree_path) &&
       parent_worktree_path->valuestring)
      printf("parent_worktree_path: %s\n", parent_worktree_path->valuestring);
   if (parent_worktree_head && cJSON_IsString(parent_worktree_head) &&
       parent_worktree_head->valuestring)
      printf("parent_worktree_head: %s\n", parent_worktree_head->valuestring);
   if (parent_worktree_finish_head && cJSON_IsString(parent_worktree_finish_head) &&
       parent_worktree_finish_head->valuestring)
      printf("parent_worktree_finish_head: %s\n", parent_worktree_finish_head->valuestring);
   if (parent_worktree_fingerprint && cJSON_IsString(parent_worktree_fingerprint) &&
       parent_worktree_fingerprint->valuestring)
      printf("parent_worktree_fingerprint: %s\n", parent_worktree_fingerprint->valuestring);
   if (parent_worktree_finish_fingerprint && cJSON_IsString(parent_worktree_finish_fingerprint) &&
       parent_worktree_finish_fingerprint->valuestring)
      printf("parent_worktree_finish_fingerprint: %s\n",
             parent_worktree_finish_fingerprint->valuestring);
   if (parent_worktree_status_drift && cJSON_IsBool(parent_worktree_status_drift))
   {
      printf("parent_worktree_status_drift: %s\n",
             cJSON_IsTrue(parent_worktree_status_drift) ? "true" : "false");
      if (cJSON_IsTrue(parent_worktree_status_drift))
         printf("parent_worktree_warning: parent dirty state changed during delegation\n");
   }
   if (parent_worktree_drift && cJSON_IsBool(parent_worktree_drift))
   {
      printf("parent_worktree_drift: %s\n", cJSON_IsTrue(parent_worktree_drift) ? "true" : "false");
      if (cJSON_IsTrue(parent_worktree_drift))
         printf("parent_worktree_warning: parent checkout changed during delegation\n");
   }
   if (checkout_drift && cJSON_IsBool(checkout_drift))
   {
      printf("checkout_drift: %s\n", cJSON_IsTrue(checkout_drift) ? "true" : "false");
      if (cJSON_IsTrue(checkout_drift))
         printf("checkout_warning: delegate checkout changed between launch and result write\n");
   }
   if (launch_worktree_path && cJSON_IsString(launch_worktree_path) &&
       launch_worktree_path->valuestring && launch_head && cJSON_IsString(launch_head) &&
       launch_head->valuestring)
   {
      char current_head[64];
      if (delegate_git_head(launch_worktree_path->valuestring, current_head,
                            sizeof(current_head)) == 0)
      {
         printf("current_head: %s\n", current_head);
         if (strcmp(launch_head->valuestring, current_head) != 0)
            printf("checkout_warning: current checkout differs from delegate launch\n");
      }
   }
   if (parent_worktree_path && cJSON_IsString(parent_worktree_path) &&
       parent_worktree_path->valuestring && parent_worktree_head &&
       cJSON_IsString(parent_worktree_head) && parent_worktree_head->valuestring)
   {
      char current_parent_head[64];
      if (delegate_git_head(parent_worktree_path->valuestring, current_parent_head,
                            sizeof(current_parent_head)) == 0)
      {
         printf("parent_worktree_current_head: %s\n", current_parent_head);
         if (strcmp(parent_worktree_head->valuestring, current_parent_head) != 0)
            printf(
                "parent_worktree_warning: current parent checkout differs from delegate launch\n");
      }
   }
   if (error && cJSON_IsString(error) && error->valuestring[0])
      printf("error: %s\n", error->valuestring);
   if (response && cJSON_IsString(response))
   {
      if (full_response || response_limit < 0)
      {
         printf("response: %s\n", response->valuestring);
      }
      else
      {
         int len = (int)strlen(response->valuestring);
         printf("response: %.*s\n", response_limit, response->valuestring);
         if (len > response_limit)
            printf("response_truncated: true\nresponse_chars: %d\n", len);
      }
   }
   delegate_status_print_coord_hint_if_present(task_id, root);
   cJSON_Delete(root);
}
