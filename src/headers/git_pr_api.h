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

/* Resolve repo_dir's `origin` to its canonical HTTPS URL
 * (https://github.com/<owner>/<repo>.git), regardless of whether origin is an
 * https or an SSH/scp URL. Lets a git network op push over HTTPS — where the
 * vaulted forge token authenticates via the askpass shim — instead of over an
 * SSH origin the server has no key for. Returns 0 + out, or -1 + err. */
int git_pr_https_origin_url(const char *repo_dir, char *out, size_t out_cap, char *err,
                            size_t errlen);

/* Like git_pr_create_via_api, but with an EXPLICIT head branch and base — the
 * head need not be checked out in repo_dir (the wfe forge opens PRs for
 * work-item branches while the shared checkout sits on the base). NULL/"" head
 * falls back to the current branch; NULL/"" base falls back to origin/HEAD. */
int git_pr_create_via_api_ex(const char *principal, const char *repo_dir, const char *head,
                             const char *base, const char *title, const char *body, char *out,
                             size_t out_cap, char *err, size_t errlen);

/* One GET /pulls/<n> snapshot: is the PR open, merged, mergeable? */
typedef struct
{
   int open;          /* state == "open" */
   int merged;        /* merged flag */
   int mergeable;     /* 1 mergeable, 0 conflicting, -1 unknown (GitHub still computing) */
   char head_sha[72]; /* head commit (for CI lookups) */
} git_pr_info_t;

int git_pr_info_via_api(const char *principal, const char *repo_dir, int number, git_pr_info_t *out,
                        char *err, size_t errlen);

/* Aggregate CI verdict for the PR's head commit, from the Checks API
 * (GET /commits/<sha>/check-runs) falling back to the legacy combined status
 * (GET /commits/<sha>/status) when no check runs exist. */
typedef enum
{
   GIT_PR_CI_ERROR = -1, /* could not determine (auth/network/API) */
   GIT_PR_CI_NONE = 0,   /* no CI reported for the head commit */
   GIT_PR_CI_PENDING,
   GIT_PR_CI_SUCCESS,
   GIT_PR_CI_FAILURE
} git_pr_ci_t;

git_pr_ci_t git_pr_ci_via_api(const char *principal, const char *repo_dir, int number, char *err,
                              size_t errlen);

/* Pure aggregation of the two API payloads (exposed for unit tests): check-runs
 * JSON first; when it lists zero runs, the combined-status JSON decides; both
 * empty/NULL -> NONE. Any failed/cancelled/timed-out run -> FAILURE; else any
 * queued/in-progress -> PENDING; else SUCCESS. */
git_pr_ci_t git_pr_ci_grade_json(const char *check_runs_json, const char *combined_status_json);

/* Squash-merge PUT /pulls/<n>/merge. Returns 0 merged, 1 already merged,
 * 2 not mergeable (405/409), -1 error. */
int git_pr_merge_via_api(const char *principal, const char *repo_dir, int number, char *err,
                         size_t errlen);

#endif /* GIT_PR_API_H */
