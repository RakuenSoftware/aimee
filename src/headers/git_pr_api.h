#ifndef GIT_PR_API_H
#define GIT_PR_API_H 1

#include <stddef.h>

/* git_pr_api — open a GitHub pull request via the REST API, IN-PROCESS, so the
 * forge token rides the Authorization header (aimee-server memory only) and
 * never reaches a child process's environment or argv. This replaces the
 * `gh pr create` path for the webchat open-PR op, closing the last spot where a
 * webchat git action put the token in a child's /proc/<pid>/environ.
 *
 * Resolves owner/repo from `repo_dir`'s github.com origin, head = current
 * branch, base = origin's default branch (fallback "main"); title defaults to
 * the last commit subject when empty. The token is resolved vault-first (per-host
 * github token, else the principal's personal vaulted token). GitHub remotes
 * only — a non-github origin returns a clean error.
 *
 * On success writes the new PR's html_url to `out` and returns 0. On failure
 * returns -1 with a short message in `err`. */
int git_pr_create_via_api(const char *principal, const char *repo_dir, const char *title,
                          const char *body, char *out, size_t out_cap, char *err, size_t errlen);

#endif /* GIT_PR_API_H */
