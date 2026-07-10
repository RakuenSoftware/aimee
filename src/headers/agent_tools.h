#ifndef DEC_AGENT_TOOLS_H
#define DEC_AGENT_TOOLS_H 1

#include "agent_protocol.h"
#include "agent_types.h"
#include <stdint.h>

/* Tool execution (Unix only) */
char *tool_bash(const char *command, int timeout_ms);
char *tool_execute_script(const char *language, const char *body, int timeout_secs,
                          const char *workdir, const char *env_json);
char *tool_read_file(const char *path, int offset, int limit);
/* Extended read: when `anchored` is set, each emitted line is prefixed with its
 * composite `LINE:HASH| ` anchor and a read snapshot is minted (scoped to `sid`,
 * which may be NULL) so a subsequent edit_file can reference the anchors. When
 * `anchored` is 0 the output is byte-identical to tool_read_file(). */
char *tool_read_file_ex(const char *path, int offset, int limit, int anchored, const char *sid);
/* Anchor-based edit: apply `edits` (a cJSON array of {op, at|from,to, text}) to
 * `path` against the read snapshot `snapshot_id` (scoped to `sid`). Returns a
 * JSON result string (caller frees): the write payload on success, a structured
 * stale_anchor / conflict / bad-op payload otherwise. dry_run computes the diff +
 * blast advisory without writing. `edits` is a `struct cJSON *`. */
char *tool_edit_file_anchored(const char *path, const char *snapshot_id, const struct cJSON *edits,
                              int dry_run, const char *sid);
/* Anchored grep: like tool_grep but, when `anchored`, groups hits by file with a
 * per-file snapshot id and prefixes each hit with its `N:hash` edit anchor so a
 * match is directly editable via edit_file. anchored=0 returns raw grep output. */
char *tool_grep_ex(const char *path, const char *pattern, int max_results, int anchored,
                   const char *sid);
char *tool_write_file(const char *path, const char *content);
/* Surgical edit: replace old_string with new_string in an existing file.
 * old_string must occur exactly once unless replace_all is non-zero (then all
 * occurrences are replaced). Returns the same structured JSON result as
 * tool_write_file on success, or an "error: ..." string. */
char *tool_edit_file(const char *path, const char *old_string, const char *new_string,
                     int replace_all);
char *tool_list_files(const char *path, const char *pattern);
char *tool_verify(const char *check_type, const char *target, const char *expected);
char *tool_git_log(const char *repo_path, int count);
char *tool_grep(const char *path, const char *pattern, int max_results);
char *tool_git_diff(const char *repo_path, const char *ref);
char *tool_git_status(const char *repo_path);
char *tool_env_get(const char *name);
char *tool_test(const char *path, const char *check);
char *tool_request_input(const char *question);
char *tool_code_search(const char *query, const char *project, int max_results);
char *tool_find_symbol(const char *identifier);
/* Fetch a single symbol's definition span (via the code index), anchored, so the
 * caller can edit it by reference without reading the whole file. Ambiguous names
 * return the candidate sites for disambiguation. `sid` scopes the read snapshot. */
char *tool_read_symbol(const char *identifier, const char *sid);
char *tool_create_note(const char *title, const char *content, const char *tags);
char *tool_list_notes(const char *tag, int limit);
char *tool_search_notes(const char *query);
char *dispatch_tool_call(const char *name, const char *arguments_json, int timeout_ms);

/* Tool definition builders */
struct cJSON *build_tools_array(void);
struct cJSON *build_tools_array_responses(void);
struct cJSON *build_tools_array_anthropic(void);
struct cJSON *delegate_respond_spec(void);
int agent_tools_append_delegate_respond_tool(struct cJSON *tools);
int agent_tools_strip_delegate_respond(parsed_response_t *parsed);
int agent_tools_role_current_code_only(const char *role);
int agent_tools_tool_allowed_for_role(const char *role, const char *tool_name);
void agent_tools_filter_for_role(struct cJSON *tools, const char *role);

/* Look up a tool's parameter schema (the `function.parameters` object) by
 * tool name. Returns a borrowed pointer into a process-lifetime cache; the
 * caller MUST NOT cJSON_Delete it. Returns NULL if the tool is not in the
 * registry. The cache is built lazily on the first call from
 * build_tools_array() and is safe to call concurrently — first caller
 * wins; subsequent callers reuse the same pointer. */
struct cJSON *agent_tool_get_schema_cached(const char *tool_name);

/* Walk an OpenAI-format tools array (each element {type:"function",
 * function:{name, description, parameters}}) and rewrite each tool's
 * `function.parameters` schema in place via tool_schema_sanitize for
 * the given provider. No-op for providers that pass schemas through
 * unchanged (openai, openrouter, codex, gemini). Active for
 * llama_native, llama-eval, and ollama. The array is modified in place; ownership
 * unchanged. */
void agent_tools_sanitize_for_provider(struct cJSON *tools, const char *provider_name);
void agent_tools_sanitize_for_agent(struct cJSON *tools, const agent_t *agent);

char *dispatch_tool_call_ctx(const char *name, const char *arguments_json, int timeout_ms);
void agent_tools_set_dispatch_role(const char *role);
const char *agent_tools_dispatch_role(void);

/* Tool-call lifecycle hook. A streaming chat worker or a /v1/runs worker
 * installs a thread-local callback (NULL by default — every other caller is
 * unaffected) before running a turn; dispatch_tool_call_ctx fires it as each
 * tool starts and completes so the turn's tool activity can be surfaced to the
 * chat SSE / ACP session/update stream and to /v1/runs events. `phase` is
 * "started" or "completed". Mirrors the agent_tools_set_dispatch_role pattern. */
typedef void (*agent_tool_event_cb_t)(const char *phase, const char *tool_name, void *ud);
void agent_tools_set_tool_event_cb(agent_tool_event_cb_t cb, void *ud);

/* Auto-snapshot turn context: call before each tool-call round so that all
 * write_file / edit_file calls in the round share one fsnap snapshot. */
void agent_tools_begin_turn(int turn);
int agent_tools_get_turn(void);
int64_t agent_tools_get_snap_id(void);
void agent_tools_set_snap_id(int64_t id);

/* Delegate parent-worktree write guard.
 * read_only_root remains readable but must not be writable through local tools.
 * write_root is an optional exception, normally the delegate's isolated worktree. */
void agent_tools_parent_write_guard_set(const char *read_only_root, const char *write_root);
void agent_tools_parent_write_guard_clear(void);
const char *agent_tools_parent_write_guard_root(void);
const char *agent_tools_parent_write_guard_write_root(void);
int agent_tools_parent_write_guard_blocks(const char *path, const char *cwd);

/* Read-only-delegate gate (backend-agnostic write capability). A delegate that
 * is not write-capable (see the write_capable field, derived once at dispatch
 * from role + write policy) is blocked from ALL file writes on the native tool
 * backend — the same read-only posture the codex sandbox enforces for the CLI
 * backend. Set once per delegation at the write-guard seam; reset by _clear. */
void agent_tools_write_capable_set(int capable);
int agent_tools_readonly_delegate_blocks(void);

/* Session-isolation backstop (Layer 2, opt-in via require_session_worktree):
 * returns 1 to BLOCK a server-side agent write whose normalized target is not
 * inside an aimee-managed worktree, else 0. No-op (returns 0) unless the
 * require_session_worktree config flag is enabled. Mirrors the client-side
 * attention-guard isolation policy for aimee's own in-process agent writes. */
int agent_tools_session_isolation_blocks(const char *path, const char *cwd);

#endif /* DEC_AGENT_TOOLS_H */
