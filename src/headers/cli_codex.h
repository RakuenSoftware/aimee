/* Codex app-server protocol helpers for the owned canonical native scheduler. */
#ifndef DEC_CLI_CODEX_H
#define DEC_CLI_CODEX_H 1

#include "aimee.h"
#include "agent_types.h"
#include "cli_native_spawn_guard.h"
#include <aimee/ir/aimee_ir.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Host-owned resources for the canonical native scheduler. Descriptor and
    * environment custody outlive the synchronous child. The adapter verifies
    * the actual effective SDK config before ready can consume a cache claim. */
   typedef struct
   {
      const aimee_request_t *history;
      const char *resume_id;
   } cli_codex_host_replay_t;
   typedef struct cli_codex_host_launch
   {
      int executable_fd, cwd_fd;
      const char *home, *cwd, *build_digest, *gateway_url, *gateway_token;
      char *const *environment;
      const struct cJSON *expected_config;
      const cli_native_spawn_guard_t *spawn_guard;
      int (*ready)(const char *launch_digest, cli_codex_host_replay_t *replay, void *context);
      void *context;
   } cli_codex_host_launch_t;

   /* Explicit Codex SDK tool boundaries. Output JSON/blocks are owned by caller.
    * Tool definitions come only from typed catalog fields. Calls require an
    * exact catalog namespace/name match; these helpers do not authorize effects. */
   struct cJSON *cli_codex_dynamic_tools(const aimee_request_t *catalog);
   int cli_codex_dynamic_call(const struct cJSON *params, const aimee_request_t *catalog,
                              aimee_block_t *out);
   struct cJSON *cli_codex_dynamic_result(const aimee_block_t *result);

   /* --- Pure JSON-RPC parse helpers (testable without a subprocess) ---
    * These read one JSON-RPC frame line emitted by `codex app-server`
    * and pull the bits aimee cares about. Each returns 1 when the line
    * matched the expected shape and the out-params were populated, 0
    * otherwise. They never block, never touch the filesystem, and are
    * safe to call on partial / unrelated frames (returns 0). */

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

#ifdef __cplusplus
}
#endif

#endif /* DEC_CLI_CODEX_H */
