/* cli_claude.c: legacy Claude provider-CLI adapter. */
#include "provider_cli_adapter.h"
#include "claude_headless.h"
#include "cli_claude_native.h"

#include <stddef.h>

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

#define CLAUDE_ARG_MAX 64

/* Build `claude -p` argv into tokens[]. The leading *split_count tokens (the
 * parsed cli_cmd, default "claude") are heap-allocated; the rest are borrowed.
 * The prompt is NOT in argv — it is fed on stdin by the caller. Returns argc or
 * -1. Shared by the local spawn and the detached (thin-client) exec_stream path. */
/* Tools the provider CLI is allowed to run.
 *
 * DATA, not a sequence of calls, so a test can enumerate it. Every entry has to
 * be accounted for by the workflow egress gate: either recognised as
 * externalization, recognised as a shell tool (and therefore gated by command
 * inspection), or on the reviewed local-only list in
 * test_cli_claude_allowlist.c. Adding a tool here without doing that fails the
 * build's test suite, which is the point -- a name-matched gate is fail-open for
 * anything added after the list was written, and this list is exactly where
 * "added after" happens.
 *
 * `Bash(*)` carries a Claude Code argument pattern; the gate sees the bare tool
 * name, so the accounting test strips the suffix. */
static const char *const CLAUDE_ALLOWED_TOOLS[] = {
    "Bash(*)", "Edit",         "Read",       "Write", "Glob",
    "Grep",    "NotebookEdit", "mcp__aimee", /* the aimee MCP server's tools (same trust domain) */
};

const char *const *cli_claude_allowed_tools(void)
{
   return CLAUDE_ALLOWED_TOOLS;
}

size_t cli_claude_allowed_tools_count(void)
{
   return sizeof(CLAUDE_ALLOWED_TOOLS) / sizeof(CLAUDE_ALLOWED_TOOLS[0]);
}

static int claude_build_argv(const provider_cli_cfg_t *cfg, char **tokens, int cap,
                             int *split_count)
{
   char err[128];
   const agent_t *agent = cfg ? cfg->agent : NULL;
   const char *cmd = (agent && agent->cli_cmd[0]) ? agent->cli_cmd : "claude";
   int count = provider_cli_split_command(cmd, tokens, cap, err, sizeof(err));
   if (count < 0)
      return -1;
   if (split_count)
      *split_count = count;

   int argc = count;

#define CLAUDE_ADD_ARG(s)                                                                          \
   do                                                                                              \
   {                                                                                               \
      if (argc >= cap)                                                                             \
      {                                                                                            \
         provider_cli_free_tokens(tokens, count);                                                  \
         return -1;                                                                                \
      }                                                                                            \
      tokens[argc++] = (char *)(s);                                                                \
      tokens[argc] = NULL;                                                                         \
   } while (0)

   CLAUDE_ADD_ARG("-p");
   CLAUDE_ADD_ARG("--output-format");
   CLAUDE_ADD_ARG("stream-json");
   CLAUDE_ADD_ARG("--verbose");
   CLAUDE_ADD_ARG("--include-partial-messages");
   if (cfg && cfg->control_channel)
   {
      CLAUDE_ADD_ARG("--input-format");
      CLAUDE_ADD_ARG("stream-json");
      CLAUDE_ADD_ARG("--permission-prompt-tool");
      CLAUDE_ADD_ARG("stdio");
   }

   /* Hand the delegate aimee. `aimee mcp serve` is the stdio MCP proxy that
    * forwards tool calls to this server over /v1, so a spawned CLI delegate reaches
    * the same tools a native agent does — git_commit / git_push / git_pr, memory,
    * the code index — instead of the nothing it had before. Without this a delegate
    * has no aimee tools at all, and its only route to git is a raw shell with
    * whatever credentials it inherits: exactly backwards.
    *
    * The binary ships in the server image (Dockerfile.server) and resolves its
    * endpoint + bearer from AIMEE_API_ENDPOINT and AIMEE_HOME's aimee.yaml, both of
    * which the spawn exports (provider_cli_adapter). */
   CLAUDE_ADD_ARG("--mcp-config");
   CLAUDE_ADD_ARG(
       "{\"mcpServers\":{\"aimee\":{\"command\":\"aimee\",\"args\":[\"mcp\",\"serve\"]}}}");
   CLAUDE_ADD_ARG("--allowedTools");
   for (size_t ti = 0; ti < cli_claude_allowed_tools_count(); ti++)
      CLAUDE_ADD_ARG(cli_claude_allowed_tools()[ti]);
   CLAUDE_ADD_ARG("--disallowedTools");
   /* Enforce delegate-only: block Claude Code's CLIENT-SIDE subagent tools so the
    * primary must use aimee delegates. `Task` is Claude Code's real sub-agent
    * spawn tool (Agent/RemoteTrigger are the legacy/aimee names) -- omitting it
    * let the primary keep spawning its own subagents despite the gateway request
    * strip, which only removes Task from the API request, not from the CLI's own
    * built-in tool registry. Mirrors guardrails' Task->Subagent canonicalization. */
   CLAUDE_ADD_ARG(cfg && cfg->control_channel ? "Task,Agent,RemoteTrigger"
                                              : "AskUserQuestion,Task,Agent,RemoteTrigger");
   if (cfg && cfg->primary && !cfg->control_channel && !strstr(cmd, "--permission-mode") &&
       !strstr(cmd, "--dangerously-skip-permissions"))
   {
      CLAUDE_ADD_ARG("--permission-mode");
      CLAUDE_ADD_ARG("bypassPermissions");
   }
   if (cfg && cfg->resume_id && cfg->resume_id[0])
   {
      CLAUDE_ADD_ARG("--resume");
      CLAUDE_ADD_ARG(cfg->resume_id);
   }
   if (cfg && cfg->system_prompt && cfg->system_prompt[0])
   {
      CLAUDE_ADD_ARG("--append-system-prompt");
      CLAUDE_ADD_ARG(cfg->system_prompt);
   }
   if (agent && agent->model[0])
   {
      CLAUDE_ADD_ARG("--model");
      CLAUDE_ADD_ARG(agent->model);
   }
#undef CLAUDE_ADD_ARG
   return argc;
}

static int claude_spawn(const provider_cli_cfg_t *cfg, const char *task_prompt, int *stdin_fd,
                        int *stdout_fd, pid_t *pid_out)
{
   (void)task_prompt;
   if (cfg && cfg->claude_host)
      return cli_claude_native_spawn(cfg, stdin_fd, stdout_fd, pid_out);
   char *tokens[CLAUDE_ARG_MAX + 1] = {0};
   int split = 0;
   int argc = claude_build_argv(cfg, tokens, CLAUDE_ARG_MAX, &split);
   if (argc < 0)
      return -1;

   int rc = provider_cli_spawn_argv(cfg, tokens, stdin_fd, stdout_fd, pid_out);
   provider_cli_free_tokens(tokens, split);
   return rc;
}

static int claude_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   return claude_headless_run(cfg, cfg ? cfg->bind_session : NULL, cfg ? cfg->bind_context : NULL,
                              out);
}

const provider_cli_adapter_t claude_provider_cli_adapter = {
    .cli_kind = "claude",
    .display_name = "Claude CLI",
    .caps = {.max_context_tokens = 200000,
             .supports_tool_use = 1,
             .proto_stability = PROVIDER_CLI_PROTO_STABLE,
             .write_confidence = 0.95f},
    .spawn = claude_spawn,
    .execute = claude_execute,
};
