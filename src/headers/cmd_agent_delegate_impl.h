/* cmd_agent_delegate_impl.h: private declarations shared between cmd_agent_delegate.c
 * and its platform implementations (posix/, windows/). */
#ifndef DEC_CMD_AGENT_DELEGATE_IMPL_H
#define DEC_CMD_AGENT_DELEGATE_IMPL_H 1

#include "aimee.h"
#include "agent.h"
#include "agent_config.h"
#include "cJSON.h"

/* Defined in cmd_agent_delegate.c */
int delegate_git_head(const char *path, char *buf, size_t len);
int delegate_head_changed(const char *path, const char *old_head);
int delegate_git_worktree_fingerprint(const char *path, char *buf, size_t len);
void delegate_checkout_add_result_ex(cJSON *resp, const char *launch_worktree_path,
                                     const char *launch_head, const char *parent_worktree_path,
                                     const char *parent_worktree_head,
                                     const char *parent_worktree_fingerprint);
void write_result_json(const char *path, const agent_result_t *result);
void write_result_json_with_checkout(const char *path, const agent_result_t *result,
                                     const char *launch_worktree_path, const char *launch_head);
void write_result_json_with_checkout_ex(const char *path, const agent_result_t *result,
                                        const char *launch_worktree_path, const char *launch_head,
                                        const char *parent_worktree_path,
                                        const char *parent_worktree_head,
                                        const char *parent_worktree_fingerprint);
void delegate_worktree_restore(const char *orig_cwd, const char *git_root, const char *work_name,
                               int keep);

/* Delegation chain guard.
 *
 * Reads AIMEE_DELEGATE_DEPTH from the environment to determine whether the
 * caller is already a delegate. Delegates may spawn sub-delegates until the
 * configured max delegation depth is reached.
 *
 * On success:
 *   - Increments AIMEE_DELEGATE_DEPTH in the environment so child processes
 *     inherit the current chain depth.
 *   - Returns 0.
 *
 * On failure:
 *   - Returns -1 without modifying the environment.
 *   - If errbuf is non-NULL, writes a human-readable message into it.
 */
int delegate_check_chain_depth(int max_depth, char *errbuf, size_t errbuf_sz);
int delegate_chain_env_should_clear(const char *depth_env, const char *parent_env,
                                    int parent_active_known, int parent_active);

typedef struct
{
   const char *task_prompt;
   const char *user_prompt;
   char *owned_user_prompt;
} delegate_prompt_plan_t;

#define DELEGATE_HANDOFF_STATUS_LEN 32
#define DELEGATE_HANDOFF_ERROR_LEN  256

typedef struct
{
   int valid;
   int repair_attempted;
   int done_without_verification;
   int needs_supervisor_review;
   int changed_files_count;
   int outside_ownership_count;
   int passed_tests;
   int commands_run;
   char status[DELEGATE_HANDOFF_STATUS_LEN];
   char raw_status[DELEGATE_HANDOFF_STATUS_LEN];
   char error[DELEGATE_HANDOFF_ERROR_LEN];
} delegate_handoff_validation_t;

/* Resolve the delegate task prompt and user prompt.
 *
 * - With only cli_prompt: both task_prompt and user_prompt are cli_prompt.
 * - With only file_prompt: task_prompt becomes a generic "see user prompt" descriptor,
 *   while user_prompt is the file contents.
 * - With both: task_prompt stays as cli_prompt and user_prompt becomes a combined
 *   prompt that includes the file contents, owned by the plan.
 */
int delegate_resolve_prompt_inputs(const char *cli_prompt, const char *file_prompt,
                                   delegate_prompt_plan_t *out);

/* Build the final-output contract appended to prompts that require structured
 * delegate handoffs.  |packet_id| may be NULL for direct one-off delegations. */
char *delegate_handoff_append_contract(const char *prompt, const char *packet_id);

/* Build a one-shot repair prompt for a malformed handoff. */
char *delegate_handoff_repair_prompt(const char *previous_response, const char *validation_error);

/* Validate delegate_result_v1 JSON text.  When |owned_files_json| is a non-empty
 * JSON array, changed files outside that list are reported as supervisor-review
 * items.  If |require_verification| is true, status=done without a passed test is
 * downgraded to partial in |out->status|. */
int delegate_handoff_validate_text(const char *text, const char *owned_files_json,
                                   int require_verification, delegate_handoff_validation_t *out);

void delegate_handoff_add_validation_json(cJSON *obj, const delegate_handoff_validation_t *v);

/* Platform-specific background dispatch.
 * POSIX: forks; parent prints task info and returns; child runs agent and _exit()s.
 * Windows: prints task info, runs agent synchronously, frees effective_prompt/file_prompt,
 *          and returns.
 * The caller should return immediately after this call. */
void platform_delegate_run_background(
    const char *tasks_dir, const char *task_id, const char *result_path, int json_output,
    agent_config_t *cfg, const char *role, const char *sys_prompt, const char *final_prompt,
    int max_tokens, int force_tools, const char *original_cwd, const char *delegate_git_root,
    const char *delegate_work_name, int keep_worktree, const char *launch_worktree_path,
    const char *launch_head, const char *parent_worktree_path, const char *parent_worktree_head,
    const char *parent_worktree_fingerprint, char *effective_prompt, char *file_prompt);

/* Platform-specific background task dispatch for cmd_dispatch.
 * POSIX: forks; parent returns 0; child runs agent and _exit()s.
 *        Returns -1 on fork failure (caller should skip this task).
 * Windows: runs agent synchronously and returns 0. */
int platform_trace_run_task(agent_config_t *cfg, const char *role, const char *sys,
                            const char *prompt, int task_timeout, int use_tools,
                            const char *result_path, int task_idx);

/* ---- Named-file drift detection (delegate_prompt.c) ---- */

/* Maximum number of named file paths extracted from a delegate prompt. */
#define DELEGATE_DRIFT_MAX_PATHS 16
/* Maximum path length stored per extracted path. */
#define DELEGATE_DRIFT_PATH_MAX 256

/* Extract repo-relative file paths (those containing '/' and a known source
 * extension) from a delegate prompt string.  Populates paths[0..max_paths-1]
 * and returns the number of distinct paths found.  Each entry in paths must be
 * at least DELEGATE_DRIFT_PATH_MAX bytes. */
int delegate_extract_named_paths(const char *prompt, char paths[][DELEGATE_DRIFT_PATH_MAX],
                                 int max_paths);

/* Returns 1 when a delegate prompt asks for writes/edits, 0 when it is
 * explicitly read-only ("read-only", "do not edit", etc.). Empty prompts are
 * treated as write-capable to preserve legacy behavior. */
int delegate_prompt_allows_writes(const char *prompt);

/* Named-file drift guard.
 *
 * Pre-flight check (response == NULL, call before running the agent):
 *   For each named path that does not exist on the filesystem, verifies that
 *   prompt contains an explicit create intent ("create", "new file", etc.).
 *   Relative paths are resolved under wt_path when provided. Returns -1 and
 *   fills errbuf when a nonexistent path lacks create intent.
 *
 * Post-run check (response != NULL, call after the agent completes):
 *   When wt_path is non-NULL, uses `git diff --name-only HEAD` in that
 *   worktree as ground truth:
 *     - path in diff                → ok (was created or modified)
 *     - path NOT in diff, exists    → context/reference file, skip (soft warn)
 *     - path NOT in diff, !exists   → hard drift (new file was not created)
 *   When wt_path is NULL, falls back to response-text matching:
 *     full-path match → ok; basename-only → soft drift (rc=1);
 *     absent from response → hard drift (rc=-1).
 *   Returns -1 on hard drift, 1 on soft drift, 0 when all paths are ok. */
int delegate_check_named_file_drift(const char *const *paths, int path_count, const char *prompt,
                                    const char *response, const char *wt_path, char *errbuf,
                                    size_t errbuf_size);

/* Build a compact validation evidence bundle for validate/review delegates.
 * The bundle includes HEAD ref, diff --stat, and changed files.
 * Returns a heap-allocated string; caller must free.  Returns NULL on failure. */
char *delegate_build_validation_bundle(const char *cwd);
char *delegate_maybe_append_validation_bundle(const char *role, const char *cwd, char *owned_prompt,
                                              const char *fallback_prompt, int target_provided);
/* Validate Location-backed review snippets against the current checkout.
 * Returns 1 and fills errbuf when a fenced code block following a
 * `Location: `path:line`` marker does not match that file near the cited line,
 * 0 when no stale evidence is found, and -1 for invalid inputs. */
int delegate_check_review_evidence_drift(const char *response, const char *repo_root, char *errbuf,
                                         size_t errbuf_size);
void delegate_apply_review_evidence_guard(const char *role, const char *repo_root, int *rc,
                                          agent_result_t *result, int target_provided);

/* Returns 1 if the worktree at wt_path has any tracked-file diff or
 * untracked-file (vs HEAD), 0 otherwise. Used by the no-op detector to
 * distinguish a delegate that genuinely did nothing from one whose
 * named-path heuristic mis-matched (the named paths were context refs,
 * not write targets). Safe to call with empty/NULL wt_path — returns 0. */
int delegate_worktree_has_changes(const char *wt_path);

/* Build orchestration context for a tools-enabled mid-tier delegate.
 *
 * If the resolved agent has tools_enabled and lower-tier agents exist, returns
 * a heap-allocated markdown block instructing the delegate to fan out sub-tasks
 * via `aimee delegate --tier K <role> "..."`.  Returns NULL otherwise.
 *
 *   via_name      — pin by agent name (mutually exclusive with tier_override)
 *   tier_override — pin by tier number (-1 = use default routing)
 *   role          — the delegation role (used to resolve the agent) */
char *delegate_build_tier_context(const char *via_name, int tier_override, const char *role);

/* Route override helpers shared by the direct CLI and server-routed delegate
 * paths. Provider routing picks the cheapest enabled agent for the requested
 * provider and role, preferring the configured default agent on equal tier. */
agent_t *delegate_route_by_provider(agent_config_t *cfg, const char *role, const char *provider);
/* Highest cost_tier among enabled, role-capable agents (or -1 if none). */
int delegate_max_cost_tier(agent_config_t *cfg, const char *role);
/* Synthesize an ephemeral ACP delegate agent from an inline `--acp <command>`
 * (+ optional `--acp-args <args>`), append it to cfg, and return its generated
 * name via name_out. The agent uses the provider-cli backend with the "acp"
 * adapter, carries the requested role, enables tools, and declares no turn cap.
 * It is the inline equivalent of an agents.json entry of kind:"acp"; routing
 * then selects it by name like any --via target. Returns 0 on success, -1 if
 * command is empty or the config is full. */
int delegate_add_inline_acp_agent(agent_config_t *cfg, const char *command, const char *args,
                                  const char *role, char *name_out, size_t name_out_sz);
int delegate_apply_route_overrides(agent_config_t *cfg, const char *role, const char *via_name,
                                   int tier_override, const char *provider_override,
                                   const char *model_override, char *errbuf, size_t errbuf_sz);
int delegate_route_preflight(agent_config_t *cfg, const char *role, char *errbuf, size_t errbuf_sz);
void delegate_infer_capability_requirements(const char *prompt, int tools_enabled,
                                            unsigned *required_caps_out, int *min_context_out);
int delegate_filter_route_capabilities(agent_config_t *cfg, const char *role,
                                       unsigned required_caps, int min_context, int drop_deprecated,
                                       char *errbuf, size_t errbuf_sz);

/* Build a ## Context block by querying the code index for terms in the prompt.
 * Returns a heap-allocated string (caller must free) or NULL if kb is unreachable
 * or the query yields no results. */
char *delegate_inject_code_context(const char *prompt);

/* §7 graph-informed delegation: build a "## Structural context (code graph)" block
 * from the callers/dependencies of the file paths the prompt references (joined
 * with cwd). Opt-in via delegate_graph_context_enabled; structural-only,
 * fail-open. Heap string (caller frees) or NULL. */
char *delegate_inject_graph_context(const char *prompt, const char *cwd);

/* Rewrite every occurrence of `cwd` in `prompt` to `worktree_path` so a delegate
 * running in an isolated sibling worktree sees paths under its own checkout.
 * Returns a newly malloc'd string (caller frees) and sets *occurrences_out to
 * the count, or NULL when nothing was rewritten (no match / empty inputs / alloc
 * failure) — in which case the caller keeps the original prompt. */
char *delegate_rewrite_prompt_cwd(const char *prompt, const char *cwd, const char *worktree_path,
                                  int *occurrences_out);

/* Concatenate `block` onto `base` (treating NULL `base` as empty), returning a
 * newly malloc'd buffer the caller owns, or NULL on NULL block / alloc failure.
 * The "append a section to the system prompt" idiom used across the delegate
 * prompt assembly. */
char *delegate_prompt_append_block(const char *base, const char *block);

/* Assemble a delegate's system prompt: the request-supplied `in` (may be NULL),
 * else a per-role template, else a static fallback; with the persona's
 * identity/principles composed on and the role's token budget applied. Returns
 * the active prompt and sets *owned_out to the malloc'd buffer the caller must
 * free (NULL when the static fallback literal is in use). */
const char *delegate_assemble_system_prompt(const char *in, const char *role, const char *prompt,
                                            const char *cwd, const char *persona,
                                            const char *worktree_path, char **owned_out);

/* For read-only inspection roles (review/validate/diagnose/test), prepend the
 * PARENT worktree's uncommitted-diff bundle (with a labelled preamble) to the
 * prompt so the delegate reports on the parent's changes, not its own isolated
 * checkout. Returns a newly malloc'd prompt the caller owns, or NULL when no
 * bundle applies (wrong role, write delegate, no cwd, empty bundle, or alloc
 * failure) — caller keeps the original. Logs a WARN when skipped for missing cwd. */
char *delegate_prepend_parent_diff_evidence(const char *prompt, const char *role, int allows_writes,
                                            const char *cwd, const char *deleg_id);

/* Print `aimee delegate` usage/help to stderr. Defined in the companion
 * cmd_agent_delegate_status.c to keep cmd_agent_delegate.c under the size cap. */
void delegate_print_help(void);

#endif /* DEC_CMD_AGENT_DELEGATE_IMPL_H */
