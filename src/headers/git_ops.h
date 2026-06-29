#ifndef GIT_OPS_H
#define GIT_OPS_H 1

#include <stddef.h>

/* git_ops — run a git operation on a webchat user's project (webchat-git WP-E).
 * The project dir is resolved + validated by workspace_scope (must already
 * exist, no cross-tenant escape), the op is checked against a fixed allowlist,
 * arguments are validated (no shell — argv only), and remote ops get the user's
 * vaulted git credentials injected (WP-C). Read ops: status, log, diff, branch.
 * Write/remote ops: fetch, pull, commit, push, checkout. Forge ops: pr (open a
 * pull request for the current branch via gh; GitHub remotes). */

/* Run `op` on `principal`'s `project`. `text_arg` carries the commit message
 * (op="commit") or target branch (op="checkout"); `num_arg` the log entry count
 * (op="log"; <=0 -> default). On success returns 0 and *out = a malloc'd, NUL-
 * terminated capture of git's output (caller frees; may be ""). On failure
 * returns -1 with a short message in err[errlen] and *out left NULL. */
int git_ops_run(const char *principal, const char *project, const char *op, const char *text_arg,
                int num_arg, char **out, char *err, size_t errlen);

/* Like git_ops_run, but when `session_id` is non-empty the op runs in that
 * session's isolated sibling worktree of the project (off the default branch,
 * created on demand) — the same tree the session's agent edits — instead of the
 * shared project checkout. session_id NULL/"" → identical to git_ops_run.
 * Requires git_ops_register_session_isolation (else session_id is ignored). */
int git_ops_run_session(const char *principal, const char *project, const char *session_id,
                        const char *op, const char *text_arg, int num_arg, char **out, char *err,
                        size_t errlen);

/* Resolve the absolute working directory a session acts in for `project`: that
 * session's sibling worktree (off the default branch, created on demand) when
 * `session_id` is non-empty and a resolver is registered, else the project
 * checkout. Writes an absolute path to out. Returns 0 on success, -1 + err. */
int git_ops_session_dir(const char *principal, const char *project, const char *session_id,
                        char *out, size_t out_len, char *err, size_t errlen);

/* Register the per-session worktree resolver (workspace.c:session_isolation_target).
 * Called once by the server at startup; unregistered → session ops/dirs fall back
 * to the shared project checkout. */
void git_ops_register_session_isolation(int (*fn)(const char *cwd, const char *sid, char *out,
                                                  size_t out_len, int create_if_missing));

#endif /* GIT_OPS_H */
