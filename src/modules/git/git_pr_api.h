#ifndef GIT_PR_API_H
#define GIT_PR_API_H 1

#include <stddef.h>

/* GitHub otherwise synthesizes a squash commit body from the child commits.
 * That can re-introduce attribution trailers which Aimee already strips from
 * its own commit/PR text and which protected branches reject. Keep the body
 * explicitly empty; the PR title remains GitHub's default squash subject. */
#define GIT_PR_SQUASH_MERGE_JSON "{\"merge_method\":\"squash\",\"commit_message\":\"\"}"

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

/* Resolve the repository's authoritative GitHub default_branch in-process.
 * Unlike origin/HEAD this value cannot be rewritten by checkout-local refs. */
int git_pr_default_branch_via_api(const char *principal, const char *repo_dir, char *out,
                                  size_t out_cap, char *err, size_t errlen);

/* Like git_pr_create_via_api, but with an EXPLICIT head branch and base — the
 * head need not be checked out in repo_dir (the wfe forge opens PRs for
 * work-item branches while the shared checkout sits on the base). NULL/"" head
 * falls back to the current branch; NULL/"" base falls back to origin/HEAD. */
int git_pr_create_via_api_ex(const char *principal, const char *repo_dir, const char *head,
                             const char *base, const char *title, const char *body, char *out,
                             size_t out_cap, char *err, size_t errlen);

/* Explicit-draft variant used by workflow handoffs. Final feature->trunk PRs
 * are created as drafts so automation cannot accidentally merge them as soon
 * as CI turns green; a human must perform the separately audited ready action.
 * Slice->feature PRs pass draft=0 and retain their autonomous CI-gated path. */
int git_pr_create_via_api_ex_draft(const char *principal, const char *repo_dir, const char *head,
                                   const char *base, const char *title, const char *body, int draft,
                                   char *out, size_t out_cap, char *err, size_t errlen);

/* Open a PR for an owner/repo slug ("owner/repo") with NO local checkout.
 *
 * Every function above resolves the repository by running git in `repo_dir`, in
 * aimee-server's own process. That is right for webchat and the workflow forge,
 * whose repo_dir is a path the server holds. It is WRONG for the MCP git tools:
 * those run against the caller's checkout through the workspace provider, and a
 * DETACHED workspace keeps the filesystem on the client, so the server cannot see
 * that path at all -- `git config --get remote.origin.url` there reports "no
 * origin remote" and the create fails outright (regression #2386, reverted).
 *
 * Such a caller resolves owner/repo through the same runner it runs every other
 * git command with, and hands the slug here. head, base and title are REQUIRED:
 * there is no checkout to infer a current branch, an origin/HEAD or a last commit
 * subject from, and guessing them is how a PR lands on the wrong base.
 *
 * The credential ladder is unchanged -- the token is resolved for the slug's host
 * and rides the Authorization header in this process only. */
int git_pr_create_via_api_slug(const char *principal, const char *slug, const char *head,
                               const char *base, const char *title, const char *body, int draft,
                               char *out, size_t out_cap, char *err, size_t errlen);

/* The read/merge ops in the same shape, for the same reason. Each repo_dir entry
 * point below now resolves the slug and delegates to its _slug sibling, so the
 * request bodies and the credential ladder have one copy and a caller with no
 * server-visible checkout has a way in. See git_pr_create_via_api_slug above. */
int git_pr_find_open_via_api_slug(const char *principal, const char *slug, const char *head,
                                  const char *base, char *out, size_t out_cap, int *number_out,
                                  char *err, size_t errlen);
int git_pr_update_via_api_slug(const char *principal, const char *slug, int number,
                               const char *title, const char *body, char *err, size_t errlen);

/* Find the existing open PR for an exact head/base pair. Returns 1 + URL,
 * 0 when absent, or -1 on API/validation failure. */
int git_pr_find_open_via_api(const char *principal, const char *repo_dir, const char *head,
                             const char *base, char *out, size_t out_cap, int *number_out,
                             char *err, size_t errlen);

/* Refresh reviewer-facing metadata on an existing workflow PR. The draft state
 * is intentionally untouched; this only makes idempotent replays repair stale
 * titles and bodies after the branch or target moved. */
int git_pr_update_via_api(const char *principal, const char *repo_dir, int number,
                          const char *title, const char *body, char *err, size_t errlen);

/* One GET /pulls/<n> snapshot: is the PR open, merged, mergeable? */
typedef struct
{
   int open;          /* state == "open" */
   int merged;        /* merged flag */
   int mergeable;     /* 1 mergeable, 0 conflicting, -1 unknown (GitHub still computing) */
   char head_sha[72]; /* head commit (for CI lookups) */
   char head[128];    /* head.ref: source branch */
   char base[128];    /* base.ref: the branch this PR merges INTO (empty if unknown) */
} git_pr_info_t;

int git_pr_info_via_api(const char *principal, const char *repo_dir, int number, git_pr_info_t *out,
                        char *err, size_t errlen);
int git_pr_info_via_api_slug(const char *principal, const char *slug, int number,
                             git_pr_info_t *out, char *err, size_t errlen);

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
git_pr_ci_t git_pr_ci_via_api_slug(const char *principal, const char *slug, int number, char *err,
                                   size_t errlen);

/* Pure aggregation of the two API payloads (exposed for unit tests): check-runs
 * JSON first; when it lists zero runs, the combined-status JSON decides; both
 * empty/NULL -> NONE. Any failed/cancelled/timed-out run -> FAILURE; else any
 * queued/in-progress -> PENDING; else SUCCESS. */
git_pr_ci_t git_pr_ci_grade_json(const char *check_runs_json, const char *combined_status_json);

/* 1 if this CI verdict permits a merge, 0 if it must not (operator ruling
 * 2026-07-15: a merge requires fully green CI). SUCCESS merges; NONE merges too —
 * a PR with no CI reported has nothing to fail. PENDING, FAILURE and ERROR all
 * refuse: "unknown" is never "pass".
 *
 * The single home for that ruling: every merge seam asks here rather than re-deriving
 * it, so the three cannot drift apart. Each seam still chooses how to COME BACK from
 * a refusal (the engine loops the node, the pipeline gate parks for the next
 * advance) — only the go/no-go lives here. Pure; unit-tested. */
int git_pr_ci_permits_merge(git_pr_ci_t ci);

/* Squash-merge PUT /pulls/<n>/merge with an explicitly empty synthesized
 * commit body. Returns 0 merged, 1 already merged, 2 not mergeable (405/409),
 * 3 merge CONFLICT, -1 error.
 *
 * 2 vs 3 matters to every caller that retries. GitHub answers 405/409 for two
 * unrelated situations: a lost race (the head or base moved between the
 * mergeability check and the PUT — "Head branch was modified", "Base branch was
 * modified"), and a genuine content conflict ("Pull Request has merge
 * conflicts"). The first resolves itself on a retry; the second is a property of
 * the two trees and is identical on every retry, forever. Collapsing them made
 * the engine re-attempt an unwinnable merge indefinitely (observed: 15 attempts
 * over 3 hours on one run, holding the single active-root slot). Callers must
 * treat 3 as terminal. */

/* Does this 405/409 merge error describe a content CONFLICT (terminal) rather
 * than a lost race (retryable)? Text-matching a forge message is unlovely, but
 * GitHub returns the same status for both and the message is the only signal it
 * gives. Fails SAFE: an unrecognised message is reported as NOT a conflict, so
 * an unfamiliar phrasing degrades to today's retry behaviour rather than
 * terminating a run that could have succeeded. Pure; unit-tested. */
int git_pr_merge_err_is_conflict(const char *err);
int git_pr_merge_via_api(const char *principal, const char *repo_dir, int number, char *err,
                         size_t errlen);
int git_pr_merge_via_api_slug(const char *principal, const char *slug, int number, char *err,
                              size_t errlen);

#endif /* GIT_PR_API_H */
