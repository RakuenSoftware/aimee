#ifndef DEC_MCP_GIT_H
#define DEC_MCP_GIT_H 1

#include "cJSON.h"

/* MCP git tool handlers. Each returns a cJSON array of content blocks
 * (text type) suitable for MCP tools/call responses. */

cJSON *handle_git_status(cJSON *args);
cJSON *handle_git_commit(cJSON *args);
cJSON *handle_git_push(cJSON *args);
cJSON *handle_git_branch(cJSON *args);
cJSON *handle_git_log(cJSON *args);
cJSON *handle_git_diff_summary(cJSON *args);
cJSON *handle_git_pr(cJSON *args);
/* handle_git_verify lives in headers/git_verify.h with the canonical
 * signature (takes server_ctx_t* for pool plumbing). */
cJSON *handle_git_pull(cJSON *args);
cJSON *handle_git_clone(cJSON *args);
cJSON *handle_git_stash(cJSON *args);
cJSON *handle_git_tag(cJSON *args);
cJSON *handle_git_fetch(cJSON *args);
cJSON *handle_git_reset(cJSON *args);
cJSON *handle_git_restore(cJSON *args);
cJSON *handle_git_issue(cJSON *args);

/* Run a git tool by NAME through the full git dispatch path: mirror-cwd remap,
 * detached-workspace binding, mcp_chdir_git_root (which REFUSES rather than let a
 * mutating op run against the main repo), the mutating-op context-mismatch guard,
 * and the handler itself — then unwinds all of it. Returns MCP content blocks
 * (caller owns), or NULL for an unknown tool.
 *
 * Exists so the NATIVE agent surface executes git through the SAME path as an
 * external MCP client rather than a second implementation that could drift: the
 * write handlers own the safety rails (branch ownership, the verify gate,
 * AI-attribution stripping), and those must not depend on which surface called in.
 *
 * git_verify is deliberately NOT reachable here — it needs the server ctx/conn the
 * MCP path supplies, and the native agent has its own `verify` tool. `sid`, when
 * non-empty, sets the session-id override for the call. */
cJSON *mcp_git_run_tool(const char *tool, cJSON *args, const char *sid);

/* Track whether the current MCP git operation is running in a worktree. */
void mcp_git_set_worktree(int val);
int mcp_git_get_worktree(void);

/* Run a git/gh shell command-line, routing through the turn's active workspace
 * provider (workspace-resource-plane). For a `shared` (co-located) workspace
 * this is byte-identical to run_cmd(); for a `detached` workspace the command
 * is marshalled to the client-side runner / server-side mirror. The
 * thread-local run_cmd CWD (mcp_chdir_git_root) is honored either way. Drop-in
 * for run_cmd(cmd, &rc) at the mcp_git call sites. */
char *mcp_git_run(const char *cmd, int *exit_code);

/* Git helper utilities shared between mcp_git_query.c and mcp_git_write.c. */
int get_current_branch(char *buf, size_t len);
int check_branch_has_merged_pr_for(const char *branch);

/* Set the thread-local run_cmd CWD to the git root before running git tools.
 * old_cwd and old_cwd_len are kept for API compat but are unused.
 * Returns 1 if a git root was resolved and run_cmd CWD was set, 0 if not.
 * Returns -1 if a session worktree was expected but inaccessible —
 * callers MUST abort rather than operating on the main repo.
 * If mismatch_err is not NULL and a context mismatch is detected, allocates an error string. */
int mcp_chdir_git_root(char *old_cwd, size_t old_cwd_len, cJSON *args, char **mismatch_err);

#endif /* DEC_MCP_GIT_H */
