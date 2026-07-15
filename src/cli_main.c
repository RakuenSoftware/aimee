/* cli_main.c: pure client entry point -- all commands go through aimee-server */
#include "aimee_home.h"
#include "aimee_client.h"
#include "cli_remote.h"
#include "cli_client.h"
#include "cli_agent_keys.h"
#include "cli_session_start.h"
#include "cmd_self_update.h"
#include "cli_attention_guard.h"
#include "cli_agent_setup.h"
#include "cli_code_audit.h"
#include "cli_css.h"
#include "cli_mcp_serve.h"
#include "acp_server.h"
#include "cli_profile.h"
#include "client_constants.h"
#include "client_integrations.h"
#include "headers/util.h"
#include "code_collect.h"          /* code_index_install_branch_hook (index watch) */
#include "harness_memory_audit.h"  /* hmem_audit (diagnostic when project unresolved) */
#include "harness_memory_common.h" /* hmem_resolve_project (client-side project key) */
#include "delegate_plan.h"
#include "cli_chat_stream.h"
#include "platform.h"
#include "platform_path.h" /* platform_getppid */
#include "platform_process.h"
#include "cJSON.h"
#include "memory_redirect.h"
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int client_hook_session_id_safe(const char *sid)
{
   if (!sid || !sid[0])
      return 0;
   for (const unsigned char *p = (const unsigned char *)sid; *p; p++)
      if (!isalnum(*p) && *p != '-' && *p != '_')
         return 0;
   return 1;
}

int client_hook_payload_session_id(const cJSON *hook_json, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';

   const char *sid = NULL;
   const cJSON *jsid = hook_json ? cJSON_GetObjectItemCaseSensitive(hook_json, "session_id") : NULL;
   if (cJSON_IsString(jsid) && jsid->valuestring[0])
      sid = jsid->valuestring;

   static const char *env_keys[] = {"AIMEE_SESSION_ID", "CLAUDE_SESSION_ID", "CODEX_THREAD_ID",
                                    NULL};
   for (int i = 0; (!sid || !sid[0]) && env_keys[i]; i++)
      sid = getenv(env_keys[i]);

   if (!client_hook_session_id_safe(sid))
      return 0;

   snprintf(out, out_len, "%s", sid);
   return 1;
}

/* Emit a structured "server unavailable" error with log path and remediation. */
static void print_server_unavailable(void)
{
   const char *cfg_dir = aimee_home();
   char log_path[512] = "(unknown)";
   if (cfg_dir)
      snprintf(log_path, sizeof(log_path), "%s/server.log", cfg_dir);

   int connect_errno = cli_last_connect_errno();
   if (cli_connect_errno_is_permission_denied(connect_errno))
   {
      const char *sock = cli_last_connect_path();
      fprintf(stderr, "aimee: server socket access denied (client version %s)\n", AIMEE_VERSION);
      if (sock && sock[0])
         fprintf(stderr, "  socket     : %s\n", sock);
      fprintf(stderr, "  reason     : %s\n", strerror(connect_errno));
      fprintf(stderr, "  server log : %s\n", log_path);
      fprintf(stderr,
              "  remediation: allow this command to access the aimee Unix socket, or run it "
              "outside the sandbox\n");
      return;
   }

   fprintf(stderr, "aimee: server unavailable (client version %s)\n", AIMEE_VERSION);
   fprintf(stderr, "  server log : %s\n", log_path);
#if defined(__linux__)
   fprintf(stderr, "  remediation: systemctl --user start aimee-server\n");
   fprintf(stderr, "               or `aimee server start` if not using systemd\n");
#elif defined(__APPLE__)
   fprintf(stderr, "  remediation: launchctl bootstrap gui/$(id -u) "
                   "~/Library/LaunchAgents/com.aimee.server.plist\n");
   fprintf(stderr, "               or `aimee server start` if not using launchd\n");
#elif defined(_WIN32) || defined(_WIN64)
   fprintf(stderr, "  remediation: net start aimee-server\n");
   fprintf(stderr, "               or `aimee server start` if the\n"
                   "               service is not registered (portable install)\n");
#else
   fprintf(stderr, "  remediation: aimee server start\n");
#endif
}

typedef enum
{
   CLIENT_TIER_CORE = 0,
   CLIENT_TIER_ADVANCED = 1,
   CLIENT_TIER_ADMIN = 2,
} client_cmd_tier_t;

typedef struct
{
   const char *name;
   const char *help;
   client_cmd_tier_t tier;
   int hidden_default;
   const char *subcommands;
} client_help_t;

static const client_help_t client_help[] = {
/* Table entries live in cli_help_data.h to keep this file under the source
 * line-count limit (same pattern as agent_help_data.h). */
#include "cli_help_data.h"
};

static void print_client_commands(FILE *out, client_cmd_tier_t tier)
{
   for (int i = 0; client_help[i].name; i++)
   {
      if (client_help[i].tier != tier || client_help[i].hidden_default)
         continue;
      fprintf(out, "  %-16s %s\n", client_help[i].name, client_help[i].help);
   }
}

static void usage(void)
{
   fprintf(stderr, "Usage: aimee [--json] [--fields=FIELDS] [--profile=PROFILE] "
                   "<command> [args...]\n"
                   "\n"
                   "Commands:\n");
   print_client_commands(stderr, CLIENT_TIER_CORE);
   fprintf(stderr, "\n"
                   "Run 'aimee help --all' to see all commands including advanced and admin.\n"
                   "Server is started automatically if not running.\n");
}

static int client_help_command(int argc, char **argv)
{
   if (argc >= 1 && strcmp(argv[0], "--all") == 0)
   {
      fprintf(stderr, "Core commands:\n");
      print_client_commands(stderr, CLIENT_TIER_CORE);
      fprintf(stderr, "\nAdvanced commands:\n");
      print_client_commands(stderr, CLIENT_TIER_ADVANCED);
      fprintf(stderr, "\nAdmin commands:\n");
      print_client_commands(stderr, CLIENT_TIER_ADMIN);
      fprintf(stderr, "\nRun 'aimee help <command>' for details on any command.\n");
      return 0;
   }

   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee help <command>\n");
      fprintf(stderr, "       aimee help --all          Show all commands\n");
      return 0;
   }

   const char *target = argv[0];

   for (int i = 0; client_help[i].name; i++)
   {
      if (strcmp(target, client_help[i].name) != 0)
         continue;
      fprintf(stderr, "aimee %s: %s\n", client_help[i].name, client_help[i].help);
      if (client_help[i].subcommands)
         fprintf(stderr, "\nSubcommands:\n%s", client_help[i].subcommands);
      return 0;
   }

   fprintf(stderr, "Unknown command: %s\n", target);
   return 1;
}

static void client_delegate_plan_usage(void)
{
   fprintf(stderr, "Usage: aimee delegate plan <proposal.md> [--json] [--output PATH] "
                   "[--launch] [--parallel N]\n"
                   "\n"
                   "Generate read-only delegate work packets from a proposal.\n");
}

static void client_delegate_usage(void)
{
   fprintf(stderr, "Usage: aimee delegate <role> [\"prompt\"] --persona NAME [options]\n"
                   "\n"
                   "Delegate a bounded task to a sub-agent. A persona is required.\n"
                   "\n"
                   "Options:\n"
                   "  --persona NAME     REQUIRED. Run the delegate as a persona (e.g. engineer,\n"
                   "                     qa, security, reviewer, architect, or a custom persona):\n"
                   "                     sets its identity + principles\n"
                   "  --json             Output result as JSON\n"
                   "  --background       Run asynchronously (returns job ID)\n"
                   "  --durable          Persist result to disk\n"
                   "  --tools            Enable tool use for the delegate\n"
                   "  --files F          Preload comma-separated file contents\n"
                   "  --context-file F   Preload a specific file (repeatable)\n"
                   "  --context SYMS     Preload code for comma-separated symbols via the index\n"
                   "  --context-dir DIR  Include directory contents as context\n"
                   "  --prompt-file PATH Read user prompt from file\n"
                   "  --prompt-stdin     Read user prompt from stdin\n"
                   "  --system S         Override system prompt\n"
                   "  --max-tokens N     Limit response tokens\n"
                   "  --max-turns N      Override the delegate turn limit\n"
                   "  --timeout N        Timeout in milliseconds\n"
                   "  --handoff-json     Require delegate_result_v1 JSON handoff output\n"
                   "  --worktree BRANCH  Check out BRANCH in the session worktree\n"
                   "  --via AGENT        Route to a specific agent by name\n"
                   "  --acp CMD          Route via an inline ACP agent (e.g. --acp claude)\n"
                   "  --acp-args ARGS    Arguments passed to the --acp command\n"
                   "  --provider NAME    Route through a specific provider\n"
                   "  --model NAME       Override provider model\n"
                   "  --tier N           Route to the best agent at cost tier N\n"
                   "\n"
                   "Subcommands:\n"
                   "  aimee delegate plan <proposal.md>        Generate read-only work packets\n"
                   "  aimee delegate launch <plan.json>        Queue a reviewed packet plan\n"
                   "  aimee delegate status <job_id>           Check background task status\n"
                   "  aimee delegate --list-roles              List available roles\n");
}

static void client_delegate_launch_usage(void)
{
   fprintf(stderr, "Usage: aimee delegate launch <plan.json> [--json] [--parallel N]\n"
                   "\n"
                   "Queue reviewed delegate work packets into a coordinated job.\n"
                   "Inspect queued packet progress with `aimee job status <job_id>`.\n"
                   "`aimee jobs status` is for durable background delegate jobs.\n");
}

static void client_delegate_status_usage(void)
{
   fprintf(stderr, "Usage: aimee delegate status <job_id> [job_id...] [--full|--result-limit N]\n"
                   "\n"
                   "Check the status of one or more background delegate jobs.\n");
}

static void client_delegate_plan_slug(const char *path, char *buf, size_t len)
{
   const char *base = strrchr(path ? path : "", '/');
   base = base ? base + 1 : (path ? path : "delegate-plan");

   char tmp[256];
   snprintf(tmp, sizeof(tmp), "%s", base[0] ? base : "delegate-plan");
   char *dot = strrchr(tmp, '.');
   if (dot)
      *dot = '\0';

   size_t n = 0;
   for (const char *p = tmp; *p && n + 1 < len; p++)
   {
      char c = (char)tolower((unsigned char)*p);
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
         buf[n++] = c;
      else if (n > 0 && buf[n - 1] != '-')
         buf[n++] = '-';
   }
   while (n > 0 && buf[n - 1] == '-')
      n--;
   buf[n] = '\0';
   if (buf[0] == '\0')
      snprintf(buf, len, "delegate-plan");
}

static int client_delegate_plan_write_json(const char *path, cJSON *plan)
{
   char parent[MAX_PATH_LEN];
   snprintf(parent, sizeof(parent), "%s", path);
   char *slash = strrchr(parent, '/');
   if (slash)
   {
      *slash = '\0';
      if (platform_mkdir_p(parent, 0755) != 0)
         return -1;
   }

   char *json = cJSON_Print(plan);
   if (!json)
      return -1;
   FILE *f = fopen(path, "w");
   if (!f)
   {
      free(json);
      return -1;
   }
   fputs(json, f);
   fputc('\n', f);
   fclose(f);
   free(json);
   return 0;
}

static int client_delegate_plan_json_int(cJSON *plan, const char *name)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(plan, name);
   return cJSON_IsNumber(v) ? v->valueint : 0;
}

static int client_delegate_plan(int argc, char **argv, int global_json_output)
{
   const char *proposal = NULL;
   const char *output_path = NULL;
   int json_output = global_json_output;
   int launch = 0;
   int parallel = 0;

   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      {
         client_delegate_plan_usage();
         return 0;
      }
      if (strcmp(argv[i], "--json") == 0)
      {
         json_output = 1;
         continue;
      }
      if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
      {
         output_path = argv[++i];
         continue;
      }
      if (strcmp(argv[i], "--launch") == 0)
      {
         launch = 1;
         continue;
      }
      if (strcmp(argv[i], "--parallel") == 0 && i + 1 < argc)
      {
         parallel = atoi(argv[++i]);
         continue;
      }
      if (!proposal)
      {
         proposal = argv[i];
         continue;
      }
      fprintf(stderr, "aimee: unexpected delegate plan argument: %s\n", argv[i]);
      return 2;
   }

   if (!proposal)
   {
      client_delegate_plan_usage();
      return 2;
   }

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_file(proposal, err, sizeof(err));
   if (!plan)
   {
      fprintf(stderr, "aimee: delegate plan failed: %s\n", err[0] ? err : "unknown error");
      return 1;
   }

   if (json_output && !launch)
   {
      char *json = cJSON_Print(plan);
      if (!json)
      {
         cJSON_Delete(plan);
         fprintf(stderr, "aimee: delegate plan failed: could not serialize JSON\n");
         return 1;
      }
      printf("%s\n", json);
      free(json);
      cJSON_Delete(plan);
      return 0;
   }

   char default_path[MAX_PATH_LEN];
   if (!output_path)
   {
      char slug[256];
      client_delegate_plan_slug(proposal, slug, sizeof(slug));
      snprintf(default_path, sizeof(default_path), ".aimee/delegate-plans/%s.json", slug);
      output_path = default_path;
   }

   if (client_delegate_plan_write_json(output_path, plan) != 0)
   {
      cJSON_Delete(plan);
      fprintf(stderr, "aimee: delegate plan failed: could not write output file\n");
      return 1;
   }

   int packet_count = client_delegate_plan_json_int(plan, "packet_count");
   int parallel_safe = client_delegate_plan_json_int(plan, "parallel_safe_count");
   int needs_sequencing = client_delegate_plan_json_int(plan, "needs_sequencing_count");
   cJSON *reviewer = cJSON_GetObjectItemCaseSensitive(plan, "reviewer_packet_included");
   cJSON *conflicts = cJSON_GetObjectItemCaseSensitive(plan, "conflicts");
   int conflict_count = cJSON_IsArray(conflicts) ? cJSON_GetArraySize(conflicts) : 0;

   if (!json_output)
   {
      printf("Generated %d work packets\n", packet_count);
      printf("Parallel-safe: %d\n", parallel_safe);
      printf("Needs sequencing: %d\n", needs_sequencing);
      printf("Potential file conflicts: %s\n", conflict_count ? "present" : "none");
      printf("Reviewer packet: %s\n", cJSON_IsTrue(reviewer) ? "included" : "not included");
      printf("Plan saved: %s\n", output_path);
      if (!launch)
         printf("Launch command: aimee delegate launch %s --parallel %d\n", output_path,
                parallel > 0 ? parallel : 3);
   }

   if (launch)
   {
      char parbuf[32];
      snprintf(parbuf, sizeof(parbuf), "%d", parallel > 0 ? parallel : 3);
      char *launch_argv[] = {"launch", (char *)output_path, "--parallel", parbuf};
      cli_v1_route_t route;
      if (!cli_v1_lookup("delegate", 4, launch_argv, &route))
      {
         cJSON_Delete(plan);
         fprintf(stderr, "aimee: delegate launch route unavailable\n");
         return 1;
      }
      const char *server_method = route.server_method ? route.server_method : route.method;
      const char *sock = cli_ensure_server_for_method(server_method);
      if (!sock)
      {
         cJSON_Delete(plan);
         print_server_unavailable();
         return 1;
      }
      if (!json_output)
         printf("Launching reviewed packet plan...\n");
      int rc = cli_v1_forward(sock, &route, json_output, NULL, NULL, 4, launch_argv);
      cJSON_Delete(plan);
      return rc >= 0 ? rc : 1;
   }

   cJSON_Delete(plan);
   return 0;
}

static int client_command_has_subcommands(const char *cmd)
{
   if (!cmd)
      return 0;
   for (int i = 0; client_help[i].name; i++)
      if (strcmp(cmd, client_help[i].name) == 0)
         return client_help[i].subcommands != NULL;
   return 0;
}

static int wants_subcommand_help(int sub_argc, char **sub_argv)
{
   if (sub_argc < 1)
      return 1;
   if (!sub_argv || !sub_argv[0])
      return 0;
   return strcmp(sub_argv[0], "--help") == 0 || strcmp(sub_argv[0], "-h") == 0 ||
          strcmp(sub_argv[0], "help") == 0;
}

static int is_help_arg(const char *arg)
{
   return arg && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0 || strcmp(arg, "help") == 0);
}

static int arg_list_contains_help_flag(int argc, char **argv)
{
   if (!argv)
      return 0;
   for (int i = 0; i < argc; i++)
      if (argv[i] && (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0))
         return 1;
   return 0;
}

static int delegate_arg_is_subcommand(const char *arg)
{
   return arg && (strcmp(arg, "plan") == 0 || strcmp(arg, "launch") == 0 ||
                  strcmp(arg, "status") == 0 || strcmp(arg, "log") == 0 ||
                  strcmp(arg, "history") == 0 || strcmp(arg, "aggregate") == 0 ||
                  strcmp(arg, "roundtable") == 0 || strcmp(arg, "--list-roles") == 0);
}

static int arg_list_contains(int argc, char **argv, const char *flag)
{
   if (!argv || !flag)
      return 0;
   for (int i = 0; i < argc; i++)
      if (argv[i] && strcmp(argv[i], flag) == 0)
         return 1;
   return 0;
}

/* Read all of stdin into a buffer. Returns malloc'd string, caller frees. */
char *read_stdin(void)
{
   size_t cap = 65536;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t len = 0;
   while (len < cap - 1)
   {
      int n = (int)read(fileno(stdin), buf + len, (unsigned int)(cap - 1 - len));
      if (n <= 0)
         break;
      len += (size_t)n;
   }
   buf[len] = '\0';
   return buf;
}

/* Unified help for the `ensemble` umbrella verb (a panel of agents). aggregate
 * and roundtable are live modes routed to the delegate.* server methods; the
 * templated turn-based session is agent-driven via the ensemble_* MCP tools. */
static void ensemble_usage(void)
{
   fprintf(stderr,
           "Usage: aimee ensemble <mode>\n"
           "  A panel of agents collaborating on one task. Modes:\n"
           "    aggregate <prompt>   Mixture-of-Agents: fan out to diverse models and\n"
           "                         synthesize one answer (configured by ensemble.*).\n"
           "    roundtable <task> [--mode review|draft] [--turns parallel|sequential]\n"
           "                         a multi-persona review/debate panel.\n"
           "    session <start|status|pause|advance|list>\n"
           "                         a persistent, templated, turn-based session\n"
           "                         (code-review, debate, planning, design-critique).\n"
           "                         Agents drive these via the ensemble_* MCP tools.\n"
           "  See docs/ENSEMBLE.md. `aimee delegate aggregate|roundtable` remain as aliases.\n");
}

static int unsupported_client_command(const char *cmd, int json_output)
{
   const char *name = (cmd && cmd[0]) ? cmd : "launch";
   char msg[256];
   snprintf(msg, sizeof(msg), "command '%s' has no /v1 route; add a /v1 route", name);

   if (json_output)
   {
      cJSON *out = cJSON_CreateObject();
      cJSON_AddStringToObject(out, "status", "error");
      cJSON_AddStringToObject(out, "command", name);
      cJSON_AddStringToObject(out, "message", msg);
      char *s = cJSON_PrintUnformatted(out);
      if (s)
      {
         puts(s);
         free(s);
      }
      cJSON_Delete(out);
   }
   else
   {
      fprintf(stderr, "aimee: %s\n", msg);
   }
   return 1;
}

static int cli_hook_client_uses_pretool_json(void)
{
   const char *client = getenv("AIMEE_HOOK_CLIENT");
   if (client)
      return strcmp(client, "claude") == 0 || strcmp(client, "codex") == 0 ||
             strcmp(client, "copilot") == 0;
   return getenv("CODEX_THREAD_ID") || getenv("CODEX_SANDBOX") || getenv("CODEX_HOME") ||
          getenv("CODEX_CWD") || getenv("CLAUDE_SESSION_ID");
}

static int cli_hook_client_supports_updated_input(void)
{
   const char *client = getenv("AIMEE_HOOK_CLIENT");
   if (client)
      return strcmp(client, "claude") == 0;
   return getenv("CLAUDE_SESSION_ID") != NULL;
}

static void emit_pretool_deny_json(const char *reason)
{
   cJSON *out = cJSON_CreateObject();
   cJSON *hook_out = cJSON_AddObjectToObject(out, "hookSpecificOutput");
   cJSON_AddStringToObject(hook_out, "hookEventName", "PreToolUse");
   cJSON_AddStringToObject(hook_out, "permissionDecision", "deny");
   cJSON_AddStringToObject(hook_out, "permissionDecisionReason",
                           reason && reason[0] ? reason : "Blocked by aimee guardrails.");

   char *s = cJSON_PrintUnformatted(out);
   if (s)
   {
      fputs(s, stdout);
      fputc('\n', stdout);
      free(s);
   }
   cJSON_Delete(out);
}

static void emit_pretool_rewrite_unsupported_json(int rewrite_rc, const char *rewritten)
{
   char reason[2048];
   snprintf(reason, sizeof(reason),
            "BLOCKED: Aimee would rewrite this %s to the session worktree, but this client does "
            "not support PreToolUse input rewrites. Retry with rewritten %s: %s",
            rewrite_rc == 3 ? "command" : "path", rewrite_rc == 3 ? "command" : "path",
            rewritten && rewritten[0] ? rewritten : "<unknown>");
   emit_pretool_deny_json(reason);
}

/* Provider-native sub-agent tools (mirrors the server's is_subagent_tool set).
 * These must never escape the session guardrails. */
static int client_tool_is_subagent(const char *tool_name)
{
   return tool_name && (strcmp(tool_name, "Agent") == 0 || strcmp(tool_name, "spawn_agent") == 0 ||
                        strcmp(tool_name, "RemoteTrigger") == 0 ||
                        strcmp(tool_name, "Subagent") == 0 || strcmp(tool_name, "Task") == 0);
}

/* Fail-closed guard for the pre-hook's fail-open paths: the server normally
 * blocks provider-native sub-agent tools, but `hooks pre` fail-opens when
 * aimee-server is unreachable. A sub-agent spawn must not slip through while the
 * server is down, so deny it here too. Returns the process exit code to use, or
 * -1 when the tool is not a sub-agent tool (caller keeps its fail-open allow). */
static int client_failopen_subagent_deny(const char *phase, const char *tool_name)
{
   if (!phase || strcmp(phase, "pre") != 0 || !client_tool_is_subagent(tool_name))
      return -1;
   const char *reason = "BLOCKED: provider-native sub-agent tools such as spawn_agent or Agent "
                        "are outside aimee's primary-agent guardrail model. Use the aimee "
                        "delegate MCP tool or `aimee delegate <role> \"<task>\"` so the child "
                        "inherits the current session guardrails.";
   if (cli_hook_client_uses_pretool_json())
   {
      emit_pretool_deny_json(reason);
      return 0;
   }
   fprintf(stderr, "aimee: %s\n", reason);
   return 2;
}

/* Worktree isolation (client-side, server-independent — mirrors the sub-agent
 * guard above). aimee isolates its OWN work in worktrees, but the primary
 * session's harness Edit/Write never reach aimee's gateway, so the shared main
 * clone was editable directly — how concurrent sessions entangle uncommitted
 * work on one branch. Enforce it here so it holds even when aimee-server is
 * down. A main clone's .git is a directory; a worktree's is a file. Returns the
 * process exit code, or -1 when not applicable (caller proceeds). */
static int client_failopen_worktree_deny(const char *phase, const char *tool_name,
                                         const char *file_path, const char *payload_cwd)
{
   if (!phase || strcmp(phase, "pre") != 0 || !tool_name)
      return -1;
   if (!(strcmp(tool_name, "Write") == 0 || strcmp(tool_name, "Edit") == 0 ||
         strcmp(tool_name, "MultiEdit") == 0 || strcmp(tool_name, "NotebookEdit") == 0))
      return -1;
   /* Prefer the session cwd from the hook payload (authoritative); fall back to
    * this process's cwd (the hook usually inherits the session's dir). */
   char cwd[MAX_PATH_LEN];
   if (payload_cwd && payload_cwd[0])
      snprintf(cwd, sizeof(cwd), "%s", payload_cwd);
   else if (!getcwd(cwd, sizeof(cwd)))
      return -1;
   /* Key on the file being edited (resolved against cwd), not just the cwd — an
    * absolute Edit into the main clone from a worktree session must still be caught. */
   if (!aimee_edit_target_in_main_clone(file_path, cwd) || aimee_main_clone_edits_allowed(cwd))
      return -1;
   char reason[1024];
   snprintf(reason, sizeof(reason),
            "BLOCKED: %s edits the SHARED MAIN CLONE (%s), not a git worktree — concurrent "
            "sessions entangle uncommitted work this way. Isolate the task in its own worktree:\n"
            "  git -C %s worktree add ../<name>-<task> -b <branch> origin/testing\n"
            "then work there. (Branch-owner override: AIMEE_ALLOW_MAIN_CHECKOUT=1 or "
            "touch %s/.git/aimee-allow-main-edits.)",
            tool_name, cwd, cwd, cwd);
   if (cli_hook_client_uses_pretool_json())
   {
      emit_pretool_deny_json(reason);
      return 0;
   }
   fprintf(stderr, "aimee: %s\n", reason);
   return 2;
}

/* Handle hooks specially -- use dedicated server methods for lower latency */
static int handle_hooks(int argc, char **argv, int json_output)
{
   if (argc < 1)
   {
      fprintf(stderr, "hooks requires 'pre' or 'post'\n");
      return 1;
   }

   const char *phase = argv[0];
   char method[32];
   snprintf(method, sizeof(method), "hooks.%s", phase);

   char *stdin_data = read_stdin();

   /* Parse tool_name and tool_input from stdin */
   cJSON *json = stdin_data ? cJSON_Parse(stdin_data) : NULL;
   const char *tool_name = "";
   const char *tool_input = "{}";
   char *tool_input_heap = NULL;
   if (json)
   {
      cJSON *tn = cJSON_GetObjectItemCaseSensitive(json, "tool_name");
      if (cJSON_IsString(tn))
         tool_name = tn->valuestring;
      cJSON *ti = cJSON_GetObjectItemCaseSensitive(json, "tool_input");
      if (cJSON_IsString(ti))
         tool_input = ti->valuestring;
      else if (cJSON_IsObject(ti) || cJSON_IsArray(ti))
      {
         tool_input_heap = cJSON_PrintUnformatted(ti);
         if (tool_input_heap)
            tool_input = tool_input_heap;
      }
   }

   /* Worktree isolation — pure filesystem check, before we even try the server,
    * so it holds regardless of aimee-server reachability. */
   const char *hook_cwd = NULL;
   const char *hook_file_path = NULL;
   if (json)
   {
      cJSON *cj = cJSON_GetObjectItemCaseSensitive(json, "cwd");
      if (cJSON_IsString(cj))
         hook_cwd = cj->valuestring;
      cJSON *ti = cJSON_GetObjectItemCaseSensitive(json, "tool_input");
      if (cJSON_IsObject(ti))
      {
         cJSON *fp = cJSON_GetObjectItemCaseSensitive(ti, "file_path");
         if (!cJSON_IsString(fp))
            fp = cJSON_GetObjectItemCaseSensitive(ti, "notebook_path");
         if (cJSON_IsString(fp))
            hook_file_path = fp->valuestring;
      }
   }
   int wt_deny = client_failopen_worktree_deny(phase, tool_name, hook_file_path, hook_cwd);
   if (wt_deny >= 0)
   {
      free(stdin_data);
      cJSON_Delete(json);
      free(tool_input_heap);
      return wt_deny;
   }

   const char *sock = cli_ensure_server_for_method(method);
   if (!sock)
   {
      int deny = client_failopen_subagent_deny(phase, tool_name);
      /* No co-located server, but a remote store is configured: the full
       * pre_tool_check needs local session state, yet the memory-file guard can
       * still run against the remote (memory_redirect captures there). Enforce
       * it so an agent can't own its own memory bytes even on a remote-only host
       * (session-start has the analogous handle_session_start_remote path). */
      if (deny < 0 && strcmp(phase, "pre") == 0 && cli_v1_has_remote_endpoint() && json)
      {
         /* memory_redirect_check captures to the configured remote endpoint and
          * denies the raw write (fail-open spill if the store is unreachable).
          * pre_tool_check is skipped — it needs local session state. */
         int rc = 0;
         cJSON *tn = cJSON_GetObjectItemCaseSensitive(json, "tool_name");
         cJSON *tin = cJSON_GetObjectItemCaseSensitive(json, "tool_input");
         if (cJSON_IsString(tn) && tn->valuestring[0] && cJSON_IsObject(tin))
         {
            cJSON *hpj = cJSON_GetObjectItemCaseSensitive(json, "harness_project");
            const char *hp = (cJSON_IsString(hpj) && hpj->valuestring[0]) ? hpj->valuestring : NULL;
            char mr_msg[1024] = "";
            if (memory_redirect_check(tn->valuestring, tin, hook_cwd, hp, mr_msg, sizeof(mr_msg)) ==
                2)
            {
               if (cli_hook_client_uses_pretool_json())
                  emit_pretool_deny_json(mr_msg);
               else
               {
                  fprintf(stderr, "aimee: %s\n", mr_msg);
                  rc = 2;
               }
            }
         }
         free(stdin_data);
         cJSON_Delete(json);
         free(tool_input_heap);
         return rc;
      }
      if (deny < 0)
         fprintf(stderr, "aimee: hooks %s: server unavailable — tool call allowed\n", phase);
      free(stdin_data);
      cJSON_Delete(json);
      free(tool_input_heap);
      return deny < 0 ? 0 : deny;
   }

   char cwd[4096];
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';
   if (json)
   {
      static const char *cwd_keys[] = {"cwd", "workdir", "working_dir", "working_directory", NULL};
      for (int i = 0; cwd_keys[i]; i++)
      {
         cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(json, cwd_keys[i]);
         if (cJSON_IsString(jcwd) && jcwd->valuestring[0])
         {
            snprintf(cwd, sizeof(cwd), "%s", jcwd->valuestring);
            break;
         }
      }
   }

   char hook_sid[64] = "";
   const char *sid =
       client_hook_payload_session_id(json, hook_sid, sizeof(hook_sid)) ? hook_sid : NULL;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddStringToObject(req, "tool_name", tool_name);
   cJSON_AddStringToObject(req, "tool_input", tool_input);
   cJSON_AddStringToObject(req, "cwd", cwd);
   if (sid && sid[0])
      cJSON_AddStringToObject(req, "session_id", sid);

   /* Forward THIS client's identity so the (possibly shared) server scopes memory
    * interception to the right agent — the server must not assume its own
    * AIMEE_HOOK_CLIENT when many agents hook into one server. */
   const char *hook_client = getenv("AIMEE_HOOK_CLIENT");
   if (hook_client && hook_client[0])
      cJSON_AddStringToObject(req, "harness_client", hook_client);

   /* Resolve the harness-memory project key HERE, on the client, against the real
    * cwd / git repo / AIMEE_PROJECT_ID, and forward it: a remote server's
    * filesystem has none of those, so it could not resolve the key itself. Only
    * the pre phase intercepts memory writes, so skip the git fork on post. */
   if (strcmp(phase, "pre") == 0 && cwd[0])
   {
      char hproject[256], hroot[4096];
      if (hmem_resolve_project(cwd, hproject, sizeof(hproject), hroot, sizeof(hroot)) == 0 &&
          hproject[0])
         cJSON_AddStringToObject(req, "harness_project", hproject);
      else
         /* Couldn't resolve client-side: the server falls back to cwd resolution,
          * which fails on a remote server (memory interception then no-ops). Audit
          * so this is observable rather than a silent miss. */
         hmem_audit("project-unresolved", NULL, NULL, "hooks pre");
   }

   cJSON *resp = cli_v1_dispatch_local(req, 5000);
   cJSON_Delete(req);
   free(stdin_data);

   int exit_code = 0;
   if (!resp)
   {
      int deny = client_failopen_subagent_deny(phase, tool_name);
      if (deny >= 0)
      {
         cJSON_Delete(json);
         free(tool_input_heap);
         return deny;
      }
      fprintf(stderr, "aimee: hooks %s: no response from server — tool call allowed\n", phase);
   }
   if (resp)
   {
      cJSON *ec = cJSON_GetObjectItemCaseSensitive(resp, "exit_code");
      if (cJSON_IsNumber(ec))
         exit_code = (int)ec->valuedouble;

      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      const char *msg_str = (cJSON_IsString(msg) && msg->valuestring[0]) ? msg->valuestring : NULL;

      /* exit_code 1 (edit file_path rewrite) and 3 (bash command rewrite) carry
       * a rewritten target in `message`. Emit Claude Code's hookSpecificOutput
       * so the tool call is silently re-targeted; otherwise the rewrite would
       * surface as a "non-blocking hook error" to the user. */
      if ((exit_code == 1 || exit_code == 3) && msg_str)
      {
         if (!cli_hook_client_supports_updated_input())
         {
            emit_pretool_rewrite_unsupported_json(exit_code, msg_str);
            cJSON_Delete(resp);
            cJSON_Delete(json);
            free(tool_input_heap);
            return 0;
         }

         cJSON *orig = cJSON_Parse(tool_input);
         if (orig)
         {
            if (exit_code == 1)
            {
               cJSON *file_path = cJSON_GetObjectItemCaseSensitive(orig, "file_path");
               if (file_path)
                  cJSON_ReplaceItemInObject(orig, "file_path", cJSON_CreateString(msg_str));
               else if (cJSON_GetObjectItemCaseSensitive(orig, "path"))
                  cJSON_ReplaceItemInObject(orig, "path", cJSON_CreateString(msg_str));
               else
                  cJSON_AddStringToObject(orig, "path", msg_str);
            }
            else
               cJSON_ReplaceItemInObject(orig, "command", cJSON_CreateString(msg_str));

            cJSON *out = cJSON_CreateObject();
            cJSON *hook_out = cJSON_AddObjectToObject(out, "hookSpecificOutput");
            cJSON_AddStringToObject(hook_out, "hookEventName", "PreToolUse");
            cJSON_AddStringToObject(hook_out, "permissionDecision", "allow");
            cJSON_AddItemToObject(hook_out, "updatedInput", orig);

            char *s = cJSON_PrintUnformatted(out);
            if (s)
            {
               fputs(s, stdout);
               fputc('\n', stdout);
               free(s);
            }
            cJSON_Delete(out);
            cJSON_Delete(resp);
            cJSON_Delete(json);
            free(tool_input_heap);
            return 0;
         }
      }

      if (strcmp(phase, "pre") == 0 && exit_code == 2 && msg_str &&
          cli_hook_client_uses_pretool_json())
      {
         emit_pretool_deny_json(msg_str);
         cJSON_Delete(resp);
         cJSON_Delete(json);
         free(tool_input_heap);
         return 0;
      }

      if (msg_str)
         fprintf(stderr, "aimee: %s\n", msg_str);

      if (json_output)
      {
         cJSON *out = cJSON_CreateObject();
         cJSON_AddNumberToObject(out, "exit_code", exit_code);
         if (msg_str)
            cJSON_AddStringToObject(out, "message", msg_str);
         char *s = cJSON_PrintUnformatted(out);
         if (s)
         {
            puts(s);
            free(s);
         }
         cJSON_Delete(out);
      }

      cJSON_Delete(resp);
   }

   cJSON_Delete(json);
   free(tool_input_heap);
   return exit_code;
}

/* `aimee session-start`: SessionStart hook entry point.
 *
 * Reads the host's hook JSON payload from stdin (Claude Code etc. send
 * {"source":"startup"|"resume"|"compact","session_id":"...",...}) and
 * forwards it to hooks.session_start. The server runs cmd_session_start's
 * body in-process and returns the captured stdout as response.output, which
 * we replay so the host injects it into the session prompt. Long timeout
 * because session_start_emit can do `git pull --ff-only` over the network.
 *
 * When AIMEE_SESSION_ID is set the hook is running inside a claude --print
 * subprocess that was spawned by chat_stream_worker.  In that context a
 * failure must not block claude: send "nonblocking": true so the server runs
 * session_start_emit on a background thread and responds immediately, and
 * treat any error (server unavailable, connection failure, request timeout) as a
 * soft failure (exit 0) so claude is never aborted by the hook. */
/* Minimal growing string buffer for assembling the session-start context
 * without depending on open_memstream's feature-test macros. */

/* --- aimee clean [--force] --- */

/* Remove aimee hooks and MCP entries from a JSON settings file. */
static int clean_settings_file(const char *path)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
      return 0;
   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz <= 0 || sz >= (1L << 20))
   {
      fclose(fp);
      return 0;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return -1;
   }
   size_t n = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[n] = '\0';

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
      return 0;

   int changed = 0;

   /* Remove aimee entries from all hook events */
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   if (hooks && cJSON_IsObject(hooks))
   {
      cJSON *event = NULL;
      cJSON_ArrayForEach(event, hooks)
      {
         if (!cJSON_IsArray(event))
            continue;
         for (int i = cJSON_GetArraySize(event) - 1; i >= 0; i--)
         {
            cJSON *entry = cJSON_GetArrayItem(event, i);
            cJSON *hlist = cJSON_GetObjectItemCaseSensitive(entry, "hooks");
            if (!hlist || !cJSON_IsArray(hlist))
               continue;
            cJSON *h = NULL;
            cJSON_ArrayForEach(h, hlist)
            {
               cJSON *c = cJSON_GetObjectItemCaseSensitive(h, "command");
               if (c && cJSON_IsString(c) && strstr(c->valuestring, "aimee"))
               {
                  cJSON_DeleteItemFromArray(event, i);
                  changed = 1;
                  break;
               }
            }
         }
      }
   }

   /* Remove aimee MCP server */
   cJSON *mcp = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (mcp && cJSON_HasObjectItem(mcp, "aimee"))
   {
      cJSON_DeleteItemFromObjectCaseSensitive(mcp, "aimee");
      changed = 1;
   }

   if (changed)
   {
      char *out = cJSON_Print(root);
      cJSON_Delete(root);
      if (!out)
         return -1;
      fp = fopen(path, "w");
      if (!fp)
      {
         free(out);
         return -1;
      }
      fputs(out, fp);
      fputc('\n', fp);
      fclose(fp);
      free(out);
      fprintf(stderr, "  cleaned %s\n", path);
   }
   else
   {
      cJSON_Delete(root);
   }
   return 0;
}

static int cli_clean(int argc, char **argv)
{
   int force = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--force") == 0)
         force = 1;
   }

   /* Two paths in play here:
    *   `base` = aimee config root (profile-aware via aimee_home())
    *   `home` = real $HOME — for paths to OTHER tools' settings
    *            (claude/gemini/codex/copilot) which legitimately
    *            ignore aimee's profile system. */
   const char *base = aimee_home();
   if (!base)
      base = "/tmp/.config/aimee";
   const char *home = platform_home_dir();
   if (!home)
      home = "/tmp";

   char config_dir[MAX_PATH_LEN];
   snprintf(config_dir, sizeof(config_dir), "%s", base);

   if (!force)
   {
      fprintf(stderr,
              "This will remove all aimee data:\n"
              "  %s/          (config, state, keys, logs)\n\n"
              "And clean aimee hooks/MCP entries from:\n"
              "  ~/.claude/settings.json\n"
              "  ~/.gemini/settings.json\n"
              "  ~/.codex/hooks.json, ~/.codex/mcp-config.json\n"
              "  ~/.copilot/config.json, ~/.copilot/mcp-config.json\n\n"
              "Run with --force to proceed.\n",
              config_dir);
      return 0;
   }

   fprintf(stderr, "Cleaning aimee installation...\n");

   /* Remove aimee config directory */
   char cmd[MAX_PATH_LEN + 32];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", config_dir);
   (void)system(cmd);
   fprintf(stderr, "  removed %s\n", config_dir);

   /* Clean hooks and MCP entries from tool settings files */
   static const char *settings_files[] = {
       "%s/.claude/settings.json",  "%s/.gemini/settings.json", "%s/.codex/hooks.json",
       "%s/.codex/mcp-config.json", "%s/.copilot/config.json",  "%s/.copilot/mcp-config.json",
   };
   char path[MAX_PATH_LEN];
   for (size_t i = 0; i < sizeof(settings_files) / sizeof(settings_files[0]); i++)
   {
      snprintf(path, sizeof(path), settings_files[i], home);
      clean_settings_file(path);
   }

   fprintf(stderr, "Done. Run the installer to set up fresh.\n");
   return 0;
}

static int client_session_brief_is_file(const char *name)
{
   if (!name || strncmp(name, "session-", 8) != 0)
      return 0;
   const char *suffix = strstr(name, ".brief.md");
   return suffix && suffix[9] == '\0' && suffix > name + 8;
}

static int client_session_brief_list(const char *config_dir)
{
   DIR *d = opendir(config_dir);
   if (!d)
   {
      fprintf(stderr, "No session briefs found.\n");
      return 1;
   }

   struct dirent *ent;
   int count = 0;
   while ((ent = readdir(d)) != NULL)
   {
      if (!client_session_brief_is_file(ent->d_name))
         continue;
      const char *suffix = strstr(ent->d_name, ".brief.md");
      char sid[128];
      size_t id_len = (size_t)(suffix - (ent->d_name + 8));
      if (id_len == 0 || id_len >= sizeof(sid))
         continue;
      memcpy(sid, ent->d_name + 8, id_len);
      sid[id_len] = '\0';

      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/%s", config_dir, ent->d_name);
      struct stat st;
      if (stat(path, &st) != 0)
         continue;

      struct tm tm_buf;
      gmtime_r(&st.st_mtime, &tm_buf);
      char ts[24];
      strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M UTC", &tm_buf);
      printf("%s  %s  %ld bytes\n", sid, ts, (long)st.st_size);
      count++;
   }
   closedir(d);
   if (count == 0)
   {
      fprintf(stderr, "No session briefs found.\n");
      return 1;
   }
   return 0;
}

static char *client_session_brief_latest_sid(const char *config_dir)
{
   DIR *d = opendir(config_dir);
   if (!d)
      return NULL;

   char best_sid[128] = "";
   time_t best_mtime = 0;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL)
   {
      if (!client_session_brief_is_file(ent->d_name))
         continue;

      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/%s", config_dir, ent->d_name);
      struct stat st;
      if (stat(path, &st) != 0 || st.st_mtime <= best_mtime)
         continue;

      const char *suffix = strstr(ent->d_name, ".brief.md");
      size_t id_len = (size_t)(suffix - (ent->d_name + 8));
      if (id_len == 0 || id_len >= sizeof(best_sid))
         continue;
      memcpy(best_sid, ent->d_name + 8, id_len);
      best_sid[id_len] = '\0';
      best_mtime = st.st_mtime;
   }
   closedir(d);
   return best_sid[0] ? strdup(best_sid) : NULL;
}
static char *client_session_brief_sid_from_cwd(const char *config_dir)
{
   char cwd[MAX_PATH_LEN];
   const char *marker = NULL;
   if (!getcwd(cwd, sizeof(cwd)) || !(marker = strstr(cwd, "/.aimee/worktrees/")))
      return NULL;
   const char *start = marker + strlen("/.aimee/worktrees/");
   const char *end = strchr(start, '/');
   size_t len = end ? (size_t)(end - start) : strlen(start);
   char prefix[128];
   if (len < 8 || len >= sizeof(prefix))
      return NULL;
   memcpy(prefix, start, len);
   prefix[len] = '\0';
   DIR *d = opendir(config_dir);
   if (!d)
      return strdup(prefix);
   char best_sid[128] = "";
   time_t best_mtime = 0;
   size_t prefix_len = strlen(prefix);
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL)
   {
      if (!client_session_brief_is_file(ent->d_name))
         continue;

      const char *suffix = strstr(ent->d_name, ".brief.md");
      char sid[128];
      size_t id_len = (size_t)(suffix - (ent->d_name + 8));
      if (id_len == 0 || id_len >= sizeof(sid))
         continue;
      memcpy(sid, ent->d_name + 8, id_len);
      sid[id_len] = '\0';
      if (strncmp(sid, prefix, prefix_len) != 0)
         continue;
      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/%s", config_dir, ent->d_name);
      struct stat st;
      if (stat(path, &st) != 0 || st.st_mtime <= best_mtime)
         continue;
      snprintf(best_sid, sizeof(best_sid), "%s", sid);
      best_mtime = st.st_mtime;
   }
   closedir(d);
   return strdup(best_sid[0] ? best_sid : prefix);
}

static int client_session_brief(int argc, char **argv)
{
   const char *sid_arg = NULL;
   int list_mode = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--session") == 0 && i + 1 < argc)
         sid_arg = argv[++i];
      else if (strcmp(argv[i], "--list") == 0)
         list_mode = 1;
      else if (argv[i][0] != '-' && !sid_arg)
         sid_arg = argv[i];
      else
      {
         fprintf(stderr, "usage: aimee session brief [--session <sid>] [--list]\n");
         return 2;
      }
   }

   const char *config_dir = aimee_home();
   if (!config_dir || !config_dir[0])
   {
      fprintf(stderr, "aimee home is unavailable; set AIMEE_HOME or HOME\n");
      return 1;
   }

   if (list_mode)
      return client_session_brief_list(config_dir);

   char *resolved_sid = NULL;
   if (!sid_arg)
   {
      resolved_sid = client_session_brief_sid_from_cwd(config_dir);
      if (!resolved_sid)
         resolved_sid = client_session_brief_latest_sid(config_dir);
      if (!resolved_sid)
      {
         fprintf(stderr, "No session briefs found. Run `aimee session-start` first.\n");
         return 1;
      }
      sid_arg = resolved_sid;
   }

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/session-%s.brief.md", config_dir, sid_arg);
   FILE *fp = fopen(path, "r");
   if (!fp)
   {
      fprintf(stderr, "No brief for session %s (expected at %s).\n", sid_arg, path);
      free(resolved_sid);
      return 1;
   }

   char buf[4096];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
      fwrite(buf, 1, n, stdout);
   fclose(fp);
   free(resolved_sid);
   return 0;
}

/* Per-delta callback: emit one ACP session/update notification to stdout as the
 * turn streams. `ud` carries the session id. */
static void acp_stream_delta(const char *delta, void *ud)
{
   if (!delta || !delta[0])
      return;
   char out[8192];
   if (acp_build_update_notification((const char *)ud, delta, out, sizeof(out)) == 1 && out[0])
   {
      fputs(out, stdout);
      fputc('\n', stdout);
      fflush(stdout);
   }
}

/* Per-tool callback: emit a standard ACP session/update tool_call notification
 * as each tool starts/completes during the turn. `ud` carries the session id. */
static void acp_stream_tool(const char *phase, const char *name, void *ud)
{
   char out[1024];
   if (acp_build_tool_update((const char *)ud, phase, name, out, sizeof(out)) == 1 && out[0])
   {
      fputs(out, stdout);
      fputc('\n', stdout);
      fflush(stdout);
   }
}

/* ACP prompt/submit dispatch: run the turn through aimee-server's chat path,
 * streaming each chunk as a session/update notification, and return the full
 * reply (heap, caller frees) or NULL. Injected into acp_server_run so
 * acp_server.c stays free of server/chat dependencies. */
static char *acp_dispatch_prompt(const char *content, const char *session_id)
{
   /* ACP turns run the agent locally (see launch_session_with_input); a remote
    * /v1 endpoint cannot serve them. */
   if (cli_v1_remote_endpoint_is_network())
      return NULL;
   const char *sock = cli_ensure_server_for_method("chat.send_stream");
   if (!sock)
      return NULL;
   /* Single-session ACP clients never call session/new and omit sessionId, so
    * session_id arrives empty and the turn would carry no aimee_session_id -- the
    * server could not log it in server_sessions. Derive a stable per-editor id for
    * LOGGING (keyed on the parent pid, mirroring the MCP serve proxy's
    * session-ppid-*) so every turn from one editor process is recorded under one
    * recoverable id. The outbound session/update notifications keep echoing the
    * client's original sessionId (empty when none was sent), so the ACP wire
    * contract is unchanged -- only the server-side logging id is synthesized. */
   char fallback[32];
   const char *log_sid = session_id;
   if (!log_sid || !log_sid[0])
   {
      snprintf(fallback, sizeof(fallback), "acp-ppid-%d", platform_getppid());
      log_sid = fallback;
   }
   return cli_chat_stream(sock, log_sid, content, acp_stream_delta, acp_stream_tool,
                          (void *)session_id);
}

/* `aimee claude-proxy enable|disable [server_url] [token]` — opt-in routing of
 * Claude Code through aimee's Anthropic Messages ingress (POST /v1/messages).
 * Off by default; enabling rewrites ~/.claude/settings.json env to reroute ALL
 * Claude Code traffic (including live sessions) to aimee's primary model.
 * URL/token come from positional args, else AIMEE_SERVER_URL /
 * AIMEE_SERVER_TOKEN. Runs locally — no server round-trip. */
static int cli_claude_proxy(int argc, char **argv)
{
   const char *action = (argc >= 1) ? argv[0] : "";
   int enable;

   if (strcmp(action, "enable") == 0)
      enable = 1;
   else if (strcmp(action, "disable") == 0)
      enable = 0;
   else
   {
      fprintf(stderr, "Usage: aimee claude-proxy enable|disable [server_url] [token]\n");
      return 2;
   }

   if (!enable)
   {
      if (claude_code_proxy_configure(NULL, NULL, 0) != 0)
      {
         fprintf(stderr, "claude-proxy: failed to update ~/.claude/settings.json\n");
         return 1;
      }
      fprintf(stderr, "claude-proxy: disabled — Claude Code restored to its default endpoint.\n");
      return 0;
   }

   const char *url = (argc >= 2) ? argv[1] : getenv("AIMEE_SERVER_URL");
   const char *token = (argc >= 3) ? argv[2] : getenv("AIMEE_SERVER_TOKEN");
   if (!url || !url[0])
   {
      fprintf(stderr, "claude-proxy: no server URL — pass one or set AIMEE_SERVER_URL\n"
                      "  e.g. aimee claude-proxy enable http://127.0.0.1:8910 <bearer>\n");
      return 2;
   }
   if (claude_code_proxy_configure(url, token, 1) != 0)
   {
      fprintf(stderr, "claude-proxy: failed to update ~/.claude/settings.json\n");
      return 1;
   }
   fprintf(stderr,
           "claude-proxy: enabled — Claude Code now routes through aimee (%s) to your primary "
           "agent.\n  This reroutes ALL Claude Code sessions, including running ones. "
           "Run 'aimee claude-proxy disable' to revert.\n",
           url);
   return 0;
}

#ifdef AIMEE_POSIX
/* `aimee index watch <name> <root>`: a client-LOCAL command (no /v1 route) that installs
 * git post-merge + post-checkout hooks so the project re-indexes itself when its default
 * branch advances. The repo + the dev's git live on this host, so the install must happen
 * client-side; the hooks invoke `aimee index scan` (which does route to the server).
 * POSIX-only — code_index_install_branch_hook (git hooks) is not built on Windows. */
static int cli_index_watch(int argc, char **argv, int json_output)
{
   if (argc < 2)
   {
      fprintf(stderr, "usage: aimee index watch <name> <root>\n"
                      "  Install post-merge + post-checkout git hooks that re-index the\n"
                      "  project whenever a merge/pull or branch switch advances the default\n"
                      "  branch (the scan no-ops cheaply when the branch SHA is unchanged).\n");
      return 1;
   }
   const char *name = argv[0], *root = argv[1];
   int rc = code_index_install_branch_hook(root, name);
   if (json_output)
   {
      printf("{\"status\":\"%s\"}\n", rc == 0 ? "ok" : "error");
      return rc == 0 ? 0 : 1;
   }
   if (rc == 0)
      printf("==> Installed git hooks (post-merge, post-checkout) — '%s' re-indexes when %s's "
             "default branch advances\n",
             name, root);
   else if (rc == -2)
      fprintf(stderr, "aimee: a non-aimee git hook is already present in %s — left untouched\n",
              root);
   else
      fprintf(stderr, "aimee: could not install git hooks in %s (not a git repo or unwritable)\n",
              root);
   return rc == 0 ? 0 : 1;
}
#endif /* AIMEE_POSIX */

int main(int argc, char **argv)
{
   /* Ignore SIGPIPE so write() to a dead aimee-server socket returns EPIPE
    * instead of terminating the CLI mid-request. cli_client retries via
    * cli_v1_forward; mcp-serve's reconnect loop reuses the same path.
    * Without this, a server restart between two CLI calls killed the
    * caller and (for mcp-serve) showed up to Claude as
    * "MCP server disconnected". */
#ifdef SIGPIPE
   signal(SIGPIPE, SIG_IGN); /* not defined on Windows; sockets there don't raise it */
#endif

   int json_output = 0;
   int debug = 0;
   int show_advanced = 0;
   int cmd_start = 1;
   const char *json_fields = NULL;
   const char *response_profile = NULL;
   const char *server_url = NULL;   /* --server / --server= override */
   const char *server_token = NULL; /* --server-token= override */

   /* Profile flag (-p NAME / --profile NAME / --profile=NAME) is
    * consumed BEFORE any state import so AIMEE_PROFILE propagates
    * into aimee_home() and onward to every config-dir resolution.
    * The flag and its value are removed from argv in-place; the rest
    * of main() never sees them. */
   {
      char profile_name[128] = {0};
      if (cli_extract_profile(&argc, argv, profile_name, sizeof(profile_name)))
         platform_setenv("AIMEE_PROFILE", profile_name);
   }

   /* See main.c: client integrations run only in cmd_init / cmd_setup. */

   /* Parse global flags */
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--json") == 0)
      {
         json_output = 1;
         cmd_start = i + 1;
      }
      else if (strcmp(argv[i], "--debug") == 0)
      {
         debug = 1;
         cmd_start = i + 1;
      }
      else if (strcmp(argv[i], "--advanced") == 0)
      {
         show_advanced = 1;
         cmd_start = i + 1;
      }
      else if (strncmp(argv[i], "--fields=", 9) == 0 || strncmp(argv[i], "--profile=", 10) == 0)
      {
         if (strncmp(argv[i], "--fields=", 9) == 0)
            json_fields = argv[i] + 9;
         else
            response_profile = argv[i] + 10;
         cmd_start = i + 1;
      }
      /* Remote aimee-server selection (--server[=URL] / --server-token=). */
      else if (aimee_client_parse_flag(argv, &i, argc, &server_url, &server_token))
         cmd_start = i + 1;
      else
         break;
   }

   /* Apply any --server / --server-token override (env fallback inside). */
   aimee_client_apply_override(server_url, server_token);
   /* Fall back to <aimee_home>/remote.conf when no flag/env override. */
   cli_remote_load_persisted();

   /* Keep local client registrations pointed at the thin client binary the
    * user actually invoked, even when the command itself forwards to a
    * long-lived server process. */
   if (cmd_start < argc && strcmp(argv[cmd_start], "mcp-serve") != 0 &&
       strcmp(argv[cmd_start], "acp-serve") != 0)
      ensure_client_integrations();

   if (cmd_start >= argc)
   {
      /* --advanced with no command: show full command list locally. */
      if (show_advanced)
      {
         char *all_arg[] = {(char *)"--all"};
         (void)json_output;
         (void)json_fields;
         (void)response_profile;
         return client_help_command(1, all_arg);
      }
      /* No subcommand: print usage. `aimee` used to forward a "launch" to the
       * server and exec a provider TUI, which is not a sensible default for a
       * CLI — it made the bare command do the single heaviest thing available,
       * and off a TTY it exited 0 having printed nothing at all. `aimee chat`
       * still launches a session; the bare command now says what it can do. */
      (void)debug;
      usage();
      return 0;
   }

   const char *cmd = argv[cmd_start];
   int sub_argc = argc - cmd_start - 1;
   char **sub_argv = argv + cmd_start + 1;

   if (!json_output && strcmp(cmd, "index") == 0 && sub_argc >= 2 &&
       (strcmp(sub_argv[0], "overview") == 0 || strcmp(sub_argv[0], "list") == 0) &&
       arg_list_contains(sub_argc - 1, sub_argv + 1, "--json"))
      json_output = 1;

   /* Local commands (no server needed) */
   if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0)
   {
      const char *prog = strrchr(argv[0], '/');
      prog = prog ? prog + 1 : argv[0];
      if (json_output)
         printf("{\"version\":\"%s\",\"binary\":\"%s\"}\n", AIMEE_VERSION, prog);
      else
         printf("%s %s\n", prog, AIMEE_VERSION);
      return 0;
   }
   if (strcmp(cmd, "self-update") == 0)
      return cmd_self_update(sub_argc, sub_argv);
   if (strcmp(cmd, "--help") == 0)
   {
      usage();
      return 0;
   }
   if (strcmp(cmd, "help") == 0)
   {
      (void)json_output;
      (void)json_fields;
      (void)response_profile;
      return client_help_command(sub_argc, sub_argv);
   }

   /* These run locally, no server needed, so they see the client's tree. */
   if (strcmp(cmd, "clean") == 0)
      return cli_clean(sub_argc, sub_argv);
   if (strcmp(cmd, "profile") == 0)
      return cmd_profile_run(sub_argc, sub_argv);
   if (strcmp(cmd, "claude-proxy") == 0)
      return cli_claude_proxy(sub_argc, sub_argv);
   /* optimize: dispatches optimize.export to its first-class /v1 route, then
    * renders points/baseline/replay client-side. */
   if (strcmp(cmd, "optimize") == 0)
      return cmd_optimize_run(sub_argc, sub_argv, json_output);
   /* workflow definitions are local files; inspect/validate without a server. */
   if (strcmp(cmd, "workflow") == 0)
      return cmd_workflow_client_run(sub_argc, sub_argv, json_output);
   /* `ensemble` umbrella: give the verb unified help; the session subcommands are
    * agent-driven (ensemble_* MCP tools), so point there rather than emitting a raw
    * "no /v1 route" error. aggregate/roundtable fall through to /v1 routing below. */
   if (strcmp(cmd, "ensemble") == 0)
   {
      const char *sub = (sub_argc >= 1) ? sub_argv[0] : NULL;
      if (!sub || is_help_arg(sub) || strcmp(sub, "session") == 0)
      {
         ensemble_usage();
         return sub ? 0 : 2;
      }
      if (strcmp(sub, "start") == 0 || strcmp(sub, "status") == 0 || strcmp(sub, "pause") == 0 ||
          strcmp(sub, "advance") == 0 || strcmp(sub, "list") == 0)
      {
         fprintf(stderr,
                 "aimee ensemble %s: templated session ensembles are agent-driven via the\n"
                 "  ensemble_%s MCP tool (or a delegate bound to a channel). See "
                 "docs/ENSEMBLE.md.\n",
                 sub, sub);
         return 2;
      }
      /* aggregate / roundtable (and anything else) fall through to the /v1 routes. */
   }
#ifndef _WIN32
   /* manuscript mode talks to the server over the Unix-domain /v1 socket
    * (http_uds_client, AF_UNIX), which the Windows client does not build. */
   if (strcmp(cmd, "remote") == 0)
      return cli_remote_cmd(sub_argc, sub_argv, json_output);
   if (strcmp(cmd, "manuscript") == 0)
      return cmd_manuscript_run(sub_argc, sub_argv, json_output);
   /* persona management likewise goes over the /v1 socket (server-owned config). */
   if (strcmp(cmd, "persona") == 0)
      return cmd_persona_client_run(sub_argc, sub_argv, json_output);
   /* roles (delegate role templates) — same server-owned-config story. */
   if (strcmp(cmd, "roles") == 0)
      return cmd_roles_client_run(sub_argc, sub_argv, json_output);
#endif

   if (strcmp(cmd, "session") == 0 && sub_argc >= 1 && strcmp(sub_argv[0], "brief") == 0)
      return client_session_brief(sub_argc - 1, sub_argv + 1);

   /* Read-only planning runs locally so it sees the client's working tree. */
   if (strcmp(cmd, "delegate") == 0 && sub_argc >= 1 && strcmp(sub_argv[0], "plan") == 0)
      return client_delegate_plan(sub_argc - 1, sub_argv + 1, json_output);
   if (strcmp(cmd, "delegate") == 0 && sub_argc >= 1 && strcmp(sub_argv[0], "launch") == 0 &&
       sub_argc >= 2 && is_help_arg(sub_argv[1]))
   {
      (void)json_output;
      (void)json_fields;
      (void)response_profile;
      client_delegate_launch_usage();
      return 0;
   }
   if (strcmp(cmd, "delegate") == 0 && sub_argc >= 1 && strcmp(sub_argv[0], "status") == 0)
   {
      (void)json_output;
      (void)json_fields;
      (void)response_profile;
      if (sub_argc >= 2 && is_help_arg(sub_argv[1]))
      {
         client_delegate_status_usage();
         return 0;
      }
      if (sub_argc < 2)
      {
         client_delegate_status_usage();
         return 2;
      }
   }
   if (strcmp(cmd, "delegate") == 0 && sub_argc >= 2 && is_help_arg(sub_argv[1]) &&
       !delegate_arg_is_subcommand(sub_argv[0]))
   {
      (void)json_output;
      (void)json_fields;
      (void)response_profile;
      client_delegate_usage();
      return 0;
   }
   /* MCP stdio proxy (replaces the retired standalone MCP binary) */
   if (strcmp(cmd, "mcp-serve") == 0)
      return cli_mcp_serve();

   /* ACP stdio server: lets ACP editors (Zed, Aider) use aimee as a backend.
    * Tag every chat turn this loop dispatches as "acp" so the session is logged
    * under its real surface (builtin_chat_send_ex forwards AIMEE_CLIENT_TYPE into
    * the chat request; the server records it on the server_sessions row). */
   if (strcmp(cmd, "acp-serve") == 0)
   {
      /* platform_setenv: portable (POSIX setenv / Windows SetEnvironmentVariable). */
      platform_setenv("AIMEE_CLIENT_TYPE", "acp");
      return acp_server_run(acp_dispatch_prompt);
   }

   /* Explicit server lifecycle. The CLI no longer auto-spawns on demand
    * (#1660); systemd / launchd / SCM owns the lifecycle on each platform,
    * with these subcommands as the manual escape hatch. */
   if (strcmp(cmd, "server") == 0 && sub_argc >= 1 && strcmp(sub_argv[0], "restart") == 0)
      return cli_restart_server();
   if (strcmp(cmd, "server") == 0 && sub_argc >= 1 && strcmp(sub_argv[0], "start") == 0)
      return cli_start_server();

   /* Hooks: use dedicated server methods */
   if (strcmp(cmd, "hooks") == 0)
      return handle_hooks(sub_argc, sub_argv, json_output);

   /* Code audit (P4): local file-health scan plus kb graph checks when available. */
   if (strcmp(cmd, "code") == 0)
   {
      if (sub_argc >= 1 && strcmp(sub_argv[0], "audit") == 0)
         return handle_code_audit(sub_argc - 1, sub_argv + 1, json_output);
      fprintf(stderr,
              "usage: aimee code audit [dir] [--json] [--project NAME] [--graph] [--fix]\n");
      return 2;
   }

   /* CSS migration assistant signals (style graph + component join + pipeline). */
   if (strcmp(cmd, "css") == 0)
      return handle_css(sub_argc, sub_argv, json_output);

   /* SessionStart hook (settings.json wires it as `aimee session-start`). */
   if (strcmp(cmd, "session-start") == 0)
      return handle_session_start(json_output);

   /* UserPromptSubmit hook (P1 per-turn context pre-injection for Claude Code;
    * settings.json wires it as `aimee user-prompt-submit`). */
   if (strcmp(cmd, "user-prompt-submit") == 0)
      return handle_user_prompt_submit();

   /* PreCompact hook (P3 re-prime; settings.json wires it as `aimee pre-compact`). */
   if (strcmp(cmd, "pre-compact") == 0)
      return handle_pre_compact();

   /* PreToolUse attention guard (P3; settings.json wires it as `aimee attention-guard`). */
   if (strcmp(cmd, "attention-guard") == 0)
      return handle_attention_guard();

   /* agent token: read access_token from local auth file; no server needed.
    * This command is called by agent_resolve_auth via safe_exec_capture so it
    * must exit 0 and print only the raw token on stdout. */
   if (strcmp(cmd, "agent") == 0 && sub_argc >= 1 && strcmp(sub_argv[0], "token") == 0)
   {
      const char *agent_name = sub_argc >= 2 ? sub_argv[1] : NULL;
      if (!agent_name || !agent_name[0])
      {
         fprintf(stderr, "usage: aimee agent token <name>\n");
         return 1;
      }
      const char *cfg_dir = aimee_home();
      if (!cfg_dir || !cfg_dir[0])
      {
         fprintf(stderr, "aimee home is unavailable; set AIMEE_HOME or HOME\n");
         return 1;
      }
      char auth_path[512];
      snprintf(auth_path, sizeof(auth_path), "%s/%s-auth.json", cfg_dir, agent_name);
      FILE *fp = fopen(auth_path, "r");
      if (!fp)
      {
         fprintf(stderr, "no auth file at %s (run: aimee agent setup %s-oauth)\n", auth_path,
                 agent_name);
         return 1;
      }
      char buf[65536];
      size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
      fclose(fp);
      buf[n] = '\0';
      cJSON *root = cJSON_Parse(buf);
      if (!root)
      {
         fprintf(stderr, "failed to parse %s\n", auth_path);
         return 1;
      }
      cJSON *at = cJSON_GetObjectItem(root, "access_token");
      if (!cJSON_IsString(at) || !at->valuestring[0])
      {
         cJSON_Delete(root);
         fprintf(stderr, "auth file missing access_token\n");
         return 1;
      }
      printf("%s", at->valuestring);
      cJSON_Delete(root);
      return 0;
   }

   if (client_command_has_subcommands(cmd) && wants_subcommand_help(sub_argc, sub_argv))
   {
      char *help_arg[] = {(char *)cmd};
      (void)json_output;
      (void)json_fields;
      (void)response_profile;
      return client_help_command(1, help_arg);
   }
   if (arg_list_contains_help_flag(sub_argc, sub_argv))
   {
      char *help_arg[] = {(char *)cmd};
      (void)json_output;
      (void)json_fields;
      (void)response_profile;
      return client_help_command(1, help_arg);
   }

   /* agent key import: client-orchestrated migration of the local keyring into the
    * server vault (P3) — read agent-keys.json here, push each via vault.set_server. */
   if (strcmp(cmd, "agent") == 0 && sub_argc >= 2 && strcmp(sub_argv[0], "key") == 0 &&
       strcmp(sub_argv[1], "import") == 0)
      return cli_agent_key_import(sub_argc - 2, sub_argv + 2, json_output);

   /* agent setup: two-step OAuth device flow, needs special client orchestration */
   if (strcmp(cmd, "agent") == 0 && sub_argc >= 1 && strcmp(sub_argv[0], "setup") == 0)
      return handle_agent_setup_cmd(sub_argc - 1, sub_argv + 1, json_output);

   /* `aimee codex reauth` — re-authenticate codex after a rejected refresh (D6). */
   if (strcmp(cmd, "codex") == 0)
      return handle_codex_cmd(sub_argc, sub_argv, json_output);

   /* workspace serve: a long-running client-side loop (poll -> run -> respond),
    * not a single forwarded /v1 call, so it is driven here rather than via a route. */
   if (strcmp(cmd, "workspace") == 0 && sub_argc >= 1 && strcmp(sub_argv[0], "serve") == 0)
      return cmd_workspace_serve(sub_argc >= 2 ? sub_argv[1] : NULL);

#ifdef AIMEE_POSIX
   /* `index watch` installs git hooks on the LOCAL repo — a filesystem op with no server
    * round-trip, so it is handled client-side before the /v1 routing (POSIX-only). */
   if (strcmp(cmd, "index") == 0 && sub_argc >= 1 && strcmp(sub_argv[0], "watch") == 0)
      return cli_index_watch(sub_argc - 1, sub_argv + 1, json_output);
#endif

   /* Thin-client workspace push: against a remote (tcp) server the workspace
    * root lives on THIS host, which the server cannot read. Resolve + register +
    * ingest from the client rather than forwarding the raw path (which the
    * server would try to realpath against its own filesystem and reject). */
   if (cli_v1_remote_endpoint_is_network())
   {
      if (strcmp(cmd, "workspace") == 0 && sub_argc >= 1 && strcmp(sub_argv[0], "add") == 0)
         return cli_workspace_add_remote(sub_argc >= 2 ? sub_argv[1] : NULL);
      if (strcmp(cmd, "index") == 0 && sub_argc >= 1 && strcmp(sub_argv[0], "scan") == 0)
         return cli_index_scan_remote(sub_argc - 1, sub_argv + 1);
      /* `agent add ... --key K` is forwarded verbatim: the server vaults the key
       * (the permanent credential store, P4) over an attested connection — a
       * native-TLS provisioning conn (#304) or local UDS — and REFUSES it over a
       * plaintext TCP bearer (D2b), so the secret is never localized client-side
       * or written to agents.json. The legacy client-held localize path was
       * retired in P4b. */
   }

   /* Route through the server's native /v1 HTTP endpoints when possible. */
   {
      cli_v1_route_t route;
      if (cli_v1_lookup(cmd, sub_argc, sub_argv, &route))
      {
         const char *server_method = route.server_method ? route.server_method : route.method;
         /* A configured remote /v1 endpoint is served over TCP by cli_v1_forward;
          * there is no local socket to discover, so skip the socket preflight
          * (which would print "server unavailable") in that mode. */
         const char *sock = NULL;
         if (!cli_v1_has_remote_endpoint())
         {
            sock = cli_ensure_server_for_method(server_method);
            if (!sock)
            {
               print_server_unavailable();
               return 1;
            }
         }
         int rc = cli_v1_forward(sock, &route, json_output, json_fields, response_profile, sub_argc,
                                 sub_argv);
         if (rc >= 0)
            return rc;
         fprintf(stderr, "aimee: server /v1 request failed for '%s'\n", cmd);
         return 1;
      }
   }

   return unsupported_client_command(cmd, json_output);
}
