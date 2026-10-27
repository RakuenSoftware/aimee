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

   /* Read an already decoded app-server edge frame. Copy a human-readable
    * notification/terminal error into *out_text (caller frees); return 1 when
    * present, 0 for unrelated frames. Nested Responses error strings are
    * decoded only here at the external boundary. No frame ownership transfer. */
   int cli_codex_parse_error_message(const struct cJSON *frame, char **out_text);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CLI_CODEX_H */
