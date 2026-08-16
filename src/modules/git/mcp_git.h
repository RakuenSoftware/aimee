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

/* Bring one line of history into another. Each takes the target ref (`ref`, or
 * `base` for rebase) to start, or action=continue/abort/skip to drive one that
 * stopped on a conflict. aimee fetches a remote-looking ref first, never opens an
 * editor, reports conflicts as the list of conflicted files, and by default
 * ABORTS on conflict so the caller is never handed a half-applied tree. */
cJSON *handle_git_merge(cJSON *args);
cJSON *handle_git_rebase(cJSON *args);
cJSON *handle_git_cherry_pick(cJSON *args);
cJSON *handle_git_revert(cJSON *args);

/* "Make this branch current with the branch it will merge into": resolve the base
 * (given, else origin's default branch), fetch it, rebase (default) or merge it
 * in, and report the gap it closed. The whole errand in one call. */
cJSON *handle_git_sync(cJSON *args);

/* Stage without committing — including new files, which git_commit cannot reach
 * (it stages tracked changes or the paths it was handed). `files` or `all`. */
cJSON *handle_git_add(cJSON *args);

/* Routing to the handler that already owns the behaviour, so the caller does not
 * have to know where it lives: switch -> git_branch action=switch; checkout ->
 * git_restore when `files` is given, otherwise switch. */
cJSON *handle_git_switch(cJSON *args);
cJSON *handle_git_checkout(cJSON *args);

/* 1 when a git tool's response reports failure. The tools say so in the text they
 * return (it leads with "error" or "conflict"), and composed operations — sync,
 * pr action=ready — have to branch on that. Defined once so a wrapper cannot
 * accidentally bury the marker and make a failure read as success. */
int mcp_git_response_failed(cJSON *resp);

/* Unstage anything in the index that is_sensitive_file() matches, naming them in
 * report (may be NULL). Returns how many. Screens the INDEX rather than a caller's
 * path list, which is what `add all=true` needs. */
int mcp_git_unstage_sensitive(char *report, size_t report_len);

/* The `-c user.name=... -c user.email=...` prefix aimee commits with, resolved
 * from the vault/checkout config. Returns 1 with the flags in out, 0 when no
 * identity is configured, -1 when the vault could not be read; pass the
 * non-positive result to mcp_git_identity_error for the message to return.
 * Shared by every path that creates a commit, so a conflict continuation is
 * authored exactly like a git_commit. */
int mcp_git_identity_flags(char *out, size_t out_len);
const char *mcp_git_identity_error(int rc);

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
/* Would merging HEAD into `base` conflict? 1 yes (conflicting paths appended to
 * `files`), 0 no, -1 cannot tell. A merge-tree dry run: it does not touch the
 * working tree or the branch. Exposed for the PR-create gate's test -- an
 * inconclusive answer must stay "proceed", and only a test pins that. */
int mcp_git_conflicts_with_base(const char *base, char *files, size_t files_cap);

int get_current_branch(char *buf, size_t len);
int check_branch_has_merged_pr_for(const char *branch);

/* Set the thread-local run_cmd CWD to the git root before running git tools.
 * old_cwd and old_cwd_len are kept for API compat but are unused.
 * Returns 1 if a git root was resolved and run_cmd CWD was set, 0 if not.
 * Returns -1 if a session worktree was expected but inaccessible —
 * callers MUST abort rather than operating on the main repo.
 * If mismatch_err is not NULL and a context mismatch is detected, allocates an error string. */
int mcp_chdir_git_root(char *old_cwd, size_t old_cwd_len, cJSON *args, char **mismatch_err);

/* Refuse an operation that writes LOCAL git state while this workspace is served as a
 * MIRROR — the server rebuilds its own worktree from a bare mirror plus the client's
 * diff, and that rebuild is replaced on the client's next change, so the write is
 * reported as success and then discarded. Returns an error response to hand back, or
 * NULL when the operation may proceed. Publishing operations (push, pr) must NOT call
 * this: their result leaves the reconstruction and is durable. */
cJSON *mcp_git_durability_guard(const char *operation);

#endif /* DEC_MCP_GIT_H */
