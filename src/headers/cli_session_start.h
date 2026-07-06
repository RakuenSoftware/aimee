#ifndef DEC_CLI_SESSION_START_H
#define DEC_CLI_SESSION_START_H 1

#include "cJSON.h"
#include <stddef.h>

/* Defined in cli_main.c; shared with cli_session_start.c (also used by the
 * pre/post-tool hook handlers that remain in cli_main.c). */
char *read_stdin(void);
int client_hook_payload_session_id(const cJSON *hook_json, char *out, size_t out_len);

/* memory_md_retire config flag, fetched from the server via config.get (the thin
 * CLI does not link the config module). Returns 1 (retire on) by default and
 * whenever the server is unreachable, so a fresh/offline session never
 * re-materializes memory files. Defined in cli_session_start.c; used by the
 * session-start hydrate gate and the cli_main.c remote-only memory guard. */
int cli_memory_md_retire(void);

/* `aimee session-start` SessionStart-hook entry (cli_session_start.c). Reads the
 * host hook JSON from stdin and either forwards hooks.session_start to a
 * co-located server or, against a remote /v1 endpoint, emits proactive recall
 * directly. Returns the process exit code. */
int handle_session_start(int json_output);

/* `aimee user-prompt-submit` UserPromptSubmit-hook entry (cli_session_start.c).
 * Reads the host hook JSON (with `prompt`) from stdin and, against a remote /v1
 * endpoint, emits a per-turn <aimee-context> pre-injection envelope as the
 * hook's additionalContext. Always soft-fails (returns 0). */
int handle_user_prompt_submit(void);

/* `aimee pre-compact` PreCompact-hook entry (cli_session_start.c). Re-emits the
 * durable recall as additionalContext just before Claude Code compacts, so the
 * post-compaction context isn't lost. Always soft-fails (returns 0). */
int handle_pre_compact(void);

#endif /* DEC_CLI_SESSION_START_H */
