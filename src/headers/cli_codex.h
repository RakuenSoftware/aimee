/* cli_codex.h: legacy codex `app-server` adapter for provider-backed chat/delegation.
 *
 * This is used only for explicit provider-CLI configs such as `codex-cli`.
 * Direct Codex adapter configs go through the primary session adapter instead.
 */
#ifndef DEC_CLI_CODEX_H
#define DEC_CLI_CODEX_H 1

#include "aimee.h"
#include "agent_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Run a single delegate turn against `codex app-server`. Spawns the
    * subprocess, drives initialize → thread/start → turn/start, reads
    * notifications until turn/completed, and fills `out` with the final
    * agent_message text + token usage + latency. Returns 0 on success,
    * -1 on error (out->error populated).
    *
    * The signature mirrors agent_execute_cli_session so this slots into
    * the same dispatch site in agent_execute_with_tools. system_prompt
    * is wired through as the codex `developerInstructions` field on
    * thread/start; user_prompt becomes the first turn's text input. */
   int agent_execute_cli_codex(const agent_t *agent, const char *system_prompt,
                               const char *user_prompt, agent_result_t *out);

   int agent_execute_cli_codex_at_cwd(const agent_t *agent, const char *cwd,
                                      const char *system_prompt, const char *user_prompt,
                                      agent_result_t *out);

   typedef int (*cli_codex_chat_event_cb)(const char *event, const char *value, void *userdata);

   typedef struct
   {
      const char *thread_id;     /* optional: resume this Codex thread */
      const char *cwd;           /* optional: child process working directory */
      const char *system_prompt; /* optional: developerInstructions on new threads */
      const char *user_prompt;
      const char *model;            /* optional: turn-level model override */
      const char *reasoning_effort; /* optional: low/medium/high/xhigh */
      int timeout_ms;               /* idle timeout; <0 disables while caller stays attached */
      int autonomous;               /* auto-approve Codex client approval requests */
   } cli_codex_chat_request_t;

   typedef struct
   {
      char thread_id[128];
      int prompt_tokens;
      int completion_tokens;
      int tool_calls;
      int latency_ms;
      char error[512];
   } cli_codex_chat_result_t;

   typedef struct
   {
      const char *thread_id; /* required: Codex thread to compact */
      const char *cwd;       /* optional: child process working directory */
      int timeout_ms;        /* idle timeout; <0 disables while caller stays attached */
      int autonomous;
   } cli_codex_compact_request_t;

   typedef struct
   {
      char thread_id[128];
      int latency_ms;
      char error[512];
   } cli_codex_compact_result_t;

   /* Stream one primary chat turn through `codex app-server`. Starts a new
    * thread when req->thread_id is empty, otherwise resumes that thread, then
    * emits "turn_start", "text", and "turn_end" callbacks as Codex notifies.
    * The resulting thread id is always copied into out->thread_id on success. */
   int cli_codex_chat_stream(const cli_codex_chat_request_t *req, cli_codex_chat_event_cb cb,
                             void *userdata, cli_codex_chat_result_t *out);

   int cli_codex_compact_thread(const cli_codex_compact_request_t *req,
                                cli_codex_compact_result_t *out);

   /* --- Pure JSON-RPC parse helpers (testable without a subprocess) ---
    * These read one JSON-RPC frame line emitted by `codex app-server`
    * and pull the bits aimee cares about. Each returns 1 when the line
    * matched the expected shape and the out-params were populated, 0
    * otherwise. They never block, never touch the filesystem, and are
    * safe to call on partial / unrelated frames (returns 0). */

   /* If `line` is an `item/completed` notification whose `params.item`
    * has `type == "agentMessage"` and a non-empty `text` field, copy the
    * text into a malloc'd buffer in *out_text (caller frees) and return
    * 1. On no match or any cJSON parse error, returns 0 and leaves
    * *out_text untouched. */
   int cli_codex_parse_agent_message(const char *line, char **out_text);

   /* If `line` is an `item/completed` notification whose `params.item.type`
    * is anything OTHER than `agentMessage` (i.e. a tool action like a
    * shell exec, file edit, or web fetch), return 1 so the caller can
    * count it as a tool call. Returns 0 for non-matching lines, parse
    * errors, or items that ARE agentMessages. */
   int cli_codex_parse_tool_action(const char *line);

   /* If `line` is a `thread/tokenUsage/updated` notification, copy the
    * `params.tokenUsage.last.{inputTokens, outputTokens}` values into
    * *prompt and *completion and return 1. Either out-param may be
    * NULL. Missing sub-fields are silently treated as zero. */
   int cli_codex_parse_token_usage(const char *line, int *prompt, int *completion);

   /* If `line` carries a Codex app-server error notification or a failed
    * turn/completed error, copy a human-readable error into *out_text
    * (caller frees) and return 1. Nested Responses API JSON errors are
    * unwrapped to their inner error.message when present. */
   int cli_codex_parse_error_message(const char *line, char **out_text);

   /* If `line` carries a JSON-RPC `method` string (request or
    * notification), copy it (NUL-terminated, truncated to outsz) into
    * out and return 1. Used to detect the `turn/completed` sentinel
    * that ends the drain loop. */
   int cli_codex_parse_method(const char *line, char *out, size_t outsz);

   /* If `line` is a JSON-RPC request emitted by `codex app-server`
    * (has both "id" and "method"), copy the method string into out and
    * return 1. Notifications have no id; responses have no method. */
   int cli_codex_parse_server_request_method(const char *line, char *out, size_t outsz);

   /* Return the Codex approval decision string for a known approval
    * request method under the given autonomous mode. */
   int cli_codex_approval_decision(const char *method, int autonomous, char *out, size_t outsz);

   /* Return 1 if a shell command would restart/stop/kill the local
    * aimee-server. Used to keep autonomous delegates from terminating
    * their own hosting server mid-turn. */
   int cli_codex_command_restarts_aimee_server(const char *command);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CLI_CODEX_H */
