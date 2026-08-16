/* branch_ownership.h: per-session branch ownership for the MCP git tools.
 *
 * Records which session created or claimed a branch so concurrent sessions
 * cannot stomp on each other's in-flight work. Storage lives behind the
 * DB2 git ownership API; this header exposes the operations and the guard
 * helpers used by mcp_git_*. */
#ifndef DEC_BRANCH_OWNERSHIP_H
#define DEC_BRANCH_OWNERSHIP_H 1

#include "cJSON.h"
#include <stddef.h>

/* Get the canonical git repo root for the current cwd. In a worktree this
 * resolves to the main checkout via --git-common-dir, so ownership records
 * are consistent across main and worktrees. Returns 0 on success. */
int get_repo_path(char *buf, size_t len);

/* Register branch ownership for the current session. Returns 0 on success. */
int branch_own_register(const char *branch);

/* Delete a branch ownership record. */
void branch_own_delete(const char *branch);

/* Check whether the current session can write to `branch`. Returns 1 if
 * allowed, 0 if blocked (owner_out filled with the owning session ID). */
int branch_own_check(const char *branch, char *owner_out, size_t owner_len);

/* Register branch ownership for an explicit repo path. Used by worktree
 * creation, which creates branches outside the normal flow. */
int mcp_git_branch_own_register(const char *repo_path, const char *branch);

/* Look up the branch owned by the current session in the current repo.
 * Returns 0 and writes the branch name to branch_out if found, -1 otherwise. */
int branch_own_get_session_branch(char *branch_out, size_t branch_len);

/* Record / look up the feature branch this session's PRs target (aimee/feat/<slug>),
 * so a feature's slices accumulate on one branch instead of each aiming at the
 * repository's default branch. Stored in db1 alongside branch ownership because the
 * PR path runs on aimee-server, which for a detached or mirrored workspace cannot
 * read anything under the checkout itself. _set returns 0 on success, -1 otherwise;
 * _for_session returns 0 when found, 1 when this session has none, -1 on error. */
int feature_branch_set(const char *branch);
int feature_branch_for_session(char *branch_out, size_t branch_len);

/* Check branch ownership for the current branch. Returns NULL if allowed,
 * or an error cJSON response if blocked by another session's ownership. */
cJSON *branch_own_guard(const char *operation);

/* Like branch_own_guard but takes a pre-fetched branch name to avoid a
 * redundant subprocess call when the caller already knows the branch. */
cJSON *branch_own_guard_for(const char *branch, const char *operation);

#endif /* DEC_BRANCH_OWNERSHIP_H */
