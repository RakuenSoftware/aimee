#ifndef GIT_OPS_H
#define GIT_OPS_H 1

#include <stddef.h>

/* git_ops — run a git operation on a webchat user's project (webchat-git WP-E).
 * The project dir is resolved + validated by workspace_scope (must already
 * exist, no cross-tenant escape), the op is checked against a fixed allowlist,
 * arguments are validated (no shell — argv only), and remote ops get the user's
 * vaulted git credentials injected (WP-C). Read ops: status, log, diff, branch.
 * Write/remote ops: fetch, pull, commit, push, checkout. */

/* Run `op` on `principal`'s `project`. `text_arg` carries the commit message
 * (op="commit") or target branch (op="checkout"); `num_arg` the log entry count
 * (op="log"; <=0 -> default). On success returns 0 and *out = a malloc'd, NUL-
 * terminated capture of git's output (caller frees; may be ""). On failure
 * returns -1 with a short message in err[errlen] and *out left NULL. */
int git_ops_run(const char *principal, const char *project, const char *op, const char *text_arg,
                int num_arg, char **out, char *err, size_t errlen);

#endif /* GIT_OPS_H */
