#ifndef DEC_AGENT_TOOLS_H
#define DEC_AGENT_TOOLS_H 1

#include "agent_protocol.h"
#include "agent_types.h"
#include <stdint.h>

/* Tool execution (Unix only) */
char *tool_bash(const char *command, int timeout_ms);
char *tool_execute_script(const char *language, const char *body, int timeout_secs,
                          const char *workdir, const char *env_json);
/* Read a file. When raw==0, each line is prefixed with a "LINE:HASH| " anchor
 * and an immutable read snapshot is minted (its id echoed in a header line) so
 * edit_file can edit by anchor. raw==1 restores the un-anchored byte output for
 * grep pipelines / binary sniffing. */
char *tool_read_file(const char *path, int offset, int limit, int raw);
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

/* The toolset THIS THREAD's turn resolves against, overriding the role. Thread-local
 * because delegate turns run on pooled worker threads and overlap: the process-wide
 * AIMEE_ACTIVE_TOOLSET env var this replaces was set per turn with a save/restore
 * bracket, which looked scoped while a concurrent delegate's setenv changed what
 * this one resolved — a reviewer could resolve a coder's toolset. The env var is
 * still honoured (it is how the single-process CLI passes --toolset); this takes
 * precedence. Set NULL/"" to clear. */
void agent_tools_set_active_toolset(const char *toolset);
const char *agent_tools_active_toolset(void);
const char *agent_tools_dispatch_role(void);

/* Git-write seam (git_commit / git_push / git_branch / git_pr).
 *
 * Those tools are implemented in the SERVER tier (the MCP git dispatch, which owns
 * the worktree refusal, branch-ownership and verify rails), but the agent tool
 * surface lives in the agent tier and is linked by binaries and tests that carry no
 * server objects. Calling across that boundary directly would break their links and
 * invert the tier order, so the server REGISTERS its dispatcher here at startup and
 * the agent tier calls through the pointer — the same shape as wfe_set_forge_provider
 * and workspace_provider_active.
 *
 * Unregistered (thin client, unit tests) the git-write tools are neither ADVERTISED
 * nor dispatchable: an agent is never offered a tool that cannot work. Returns MCP
 * content blocks (caller owns), or NULL for an unknown tool. */
typedef struct cJSON *(*agent_git_write_fn)(const char *tool, struct cJSON *args, const char *sid);
void agent_tools_set_git_write_provider(agent_git_write_fn fn);
agent_git_write_fn agent_tools_git_write_provider(void);

/* 1 if `name` is one of the git-write tools that ride the seam above. */
int agent_tools_is_git_write(const char *name);

/* MCP-derived tools ────────────────────────────────────────────────────────
 *
 * aimee's MCP dispatch table is the single source of truth for which tools exist.
 * Entries marked native are registered here at startup so aimee's OWN agents get
 * them too, deriving the advert, the schema and the dispatch from the one
 * declaration instead of restating each by hand in a separate registry.
 *
 * That restating is not a hypothetical cost. git_commit/git_push/git_pr were
 * MCP-only, so the implement delegate's only route to land work was shelling out
 * to git — the exact thing require_aimee_git forbids. index_find_callers was
 * MCP-only, so a review panel asked "is this still called?" had no tool that could
 * answer and hedged on a symbol with twelve callers one query away. Both shipped
 * green and were found by watching a delegate on real hardware.
 *
 * Reusing the MCP schema rather than writing a native one is deliberate: a second
 * hand-written schema is how git_commit came to advertise parameters (add_all,
 * set_upstream) that its handler had never accepted.
 *
 * `call` runs the tool; `advert` returns its MCP tools/list entry ({"description",
 * "inputSchema"}, caller owns) or NULL if unknown. Unregistered — thin client,
 * unit tests — no MCP-derived tool is advertised or dispatchable, so a binary
 * without the server tier links and behaves exactly as before. */
typedef struct cJSON *(*agent_mcp_call_fn)(const char *tool, struct cJSON *args, const char *sid);
typedef struct cJSON *(*agent_mcp_advert_fn)(const char *tool);
void agent_tools_set_mcp_provider(agent_mcp_call_fn call, agent_mcp_advert_fn advert);
agent_mcp_call_fn agent_tools_mcp_call_provider(void);

/* Declare an MCP tool as part of aimee's native surface. Idempotent. Must be
 * called before the first build_tools_array() so the schema cache sees it. */
void agent_tools_register_mcp_tool(const char *name);

/* 1 if `name` was registered above and so dispatches through the MCP provider. */
int agent_tools_is_mcp_derived(const char *name);

/* Shell-git gate seam: 1 if this shell command must be refused because git belongs
 * to aimee (require_aimee_git). Registered by the server for the same reason as the
 * git-write provider — the decision needs the config dial, the forge credential and
 * the command classifier, which live in tiers the agent surface cannot link.
 *
 * `cwd` is the directory the command would run in, and is REQUIRED for the decision
 * to be correct rather than merely safe: "can aimee do git here?" is answered per
 * repo (the credential ladder's per-host vault rung keys on the checkout's origin).
 * Passing no directory collapses the question to "does the server have its own
 * identity?", which most deployments never configure — the gate then concludes
 * aimee has no git and never fires, on exactly the boxes where it should.
 *
 * Unregistered, there is no gate — a rule with no working alternative is breakage,
 * not policy, so the absence of the alternative disables the rule by construction. */
typedef int (*agent_shell_git_gate_fn)(const char *command, const char *cwd);
void agent_tools_set_shell_git_gate(agent_shell_git_gate_fn fn);
agent_shell_git_gate_fn agent_tools_shell_git_gate(void);

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
