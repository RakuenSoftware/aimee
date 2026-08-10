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

/* PATCH a PR with ANY SUBSET of title/body/base -- a NULL or empty field is left
 * out of the payload rather than sent empty, so editing a title cannot blank a
 * body. At least one must be present. `base` retargets the PR.
 *
 * git_pr_update_via_api{,_slug} above demand BOTH title and body and cannot touch
 * base; they stay as they are because the workflow forge relies on that
 * all-or-nothing contract for idempotent replays. This is the general form and
 * they delegate to it, so there is one PATCH. */
int git_pr_edit_via_api_slug(const char *principal, const char *slug, int number, const char *title,
                             const char *body, const char *base, char *err, size_t errlen);

/* One CI check for a PR's head commit, in the shape `gh pr checks` printed it.
 * The field values and their spelling were derived from gh's actual output rather
 * than from its source: 85 rows across three PRs matched field-for-field.
 *
 * Row CONTENTS match; row ORDER is ours, not gh's. gh's ordering could not be
 * reproduced and does not look reproducible: for one PR it grouped skipped checks
 * ahead of passing ones, for another it was plain alphabetical, and for a third
 * with CI in flight it was neither. Consumers parse these by field, so a stable
 * order is worth more than chasing one gh does not itself hold still. */
typedef struct
{
   char name[256];
   char status[16];  /* pass / fail / pending / skipping */
   char elapsed[16]; /* "45s", "2m54s", or "0" when pending or zero-length */
   char url[512];    /* details_url */
} git_pr_check_t;

/* Checks for PR `number`'s head commit into out[], at most `max`, count in
 * *count, sorted by name (see above: ours, not gh's). Returns 0 on success (including no
 * checks at all), -1 with `err` set otherwise. Caller supplies the array. */
int git_pr_checks_via_api_slug(const char *principal, const char *slug, int number, int max,
                               git_pr_check_t *out, int *count, char *err, size_t errlen);

/* Why one check failed, in the terms someone fixing it needs.
 *
 * `status`/`url` from the checks listing say a job is red; they do not say what
 * broke, and the URL is unreachable from an agent. These fields are: which job,
 * which step inside it, and the tail of that job's log, which is where the actual
 * error text lives (the forge's own annotation for a shell failure is
 * "Process completed with exit code 1", which explains nothing). */
typedef struct
{
   char name[256];        /* check / job name */
   char conclusion[24];   /* failure / timed_out / cancelled / action_required */
   char failed_step[256]; /* first failed step; for an unnamed step the forge
                           * reports the command, which is what to run locally */
   int failed_step_number;
   char url[512];
   char *log_tail; /* malloc'd, NUL-terminated, may be NULL; caller frees */
} git_pr_failure_t;

/* The failed checks for PR `number`'s head commit, at most `max`, count in
 * *count. Each row gets its failed step; the first `logs_for` rows also get up to
 * `tail_bytes` of their job log (0 for neither). Returns 0 on success — including
 * "nothing failed", which is *count == 0 — or -1 with `err` set.
 *
 * Caller supplies the array and must call git_pr_failures_free() to release the
 * log tails. */
int git_pr_failures_via_api_slug(const char *principal, const char *slug, int number, int max,
                                 int logs_for, long tail_bytes, git_pr_failure_t *out, int *count,
                                 char *err, size_t errlen);
void git_pr_failures_free(git_pr_failure_t *rows, int count);

/* One row of an open-PR listing. */
typedef struct
{
   int number;
   char state[16];  /* OPEN / CLOSED / MERGED, gh's spelling */
   char head[128];  /* head.ref */
   char title[512]; /* PR title */
} git_pr_list_item_t;

/* The `limit` most recently updated OPEN PRs into out[], writing how many landed
 * to *count. Returns 0 on success (including zero PRs), -1 with `err` set
 * otherwise. Caller supplies the array; nothing is allocated. */
int git_pr_list_open_via_api_slug(const char *principal, const char *slug, int limit,
                                  git_pr_list_item_t *out, int *count, char *err, size_t errlen);

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
   /* Reviewer-facing fields, so a caller can render a PR without a second trip
    * through `gh pr view`. Best-effort: a response missing any of them still
    * yields the refs above rather than failing, so existing callers that only
    * read open/merged/mergeable are unaffected. */
   char title[512];    /* PR title */
   char html_url[512]; /* browser URL */
   char merged_at[32]; /* ISO8601 merge timestamp; empty when not merged */
   /* REST `mergeable_state`, upper-cased to match the value gh reported as
    * mergeStateStatus: CLEAN / DIRTY / BLOCKED / BEHIND / UNSTABLE / DRAFT /
    * UNKNOWN. Empty when GitHub did not supply one; render that as UNKNOWN. */
   char merge_state[24];
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

/* The conflict-vs-lost-race classification itself now happens in the git module
 * (server-go/modules/git, isMergeConflict): the merge runs there, so the message
 * is read where it arrives rather than re-derived from a rendered error string
 * here. The stage reports the two apart as `conflict` and `retryable`, and the
 * 2-vs-3 mapping above is a straight translation of those. The phrasings that
 * must and must not terminate are pinned by
 * TestMergeConflictClassificationFailsSafeTowardRetry. */
int git_pr_merge_via_api(const char *principal, const char *repo_dir, int number, char *err,
                         size_t errlen);
int git_pr_merge_via_api_slug(const char *principal, const char *slug, int number, char *err,
                              size_t errlen);

/* The general merge: choose the method, optionally pin the head, and get the merge
 * commit back. Same 0/1/2/3/-1 contract as above.
 *
 * merge_method is merge (the default when NULL/empty), squash or rebase; an
 * unrecognised value is REFUSED, never coerced -- silently squashing would rewrite
 * history the caller did not ask to rewrite. commit_message is emptied only for a
 * squash, which is the only method that synthesizes a body.
 *
 * expected_head_sha, when given, becomes REST's `sha`: GitHub refuses the merge if
 * the head moved since the caller looked. out_sha receives the merge commit from
 * the 200 body, so no follow-up read is needed to record it.
 *
 * git_pr_merge_via_api{,_slug} keep their squash-with-empty-body behaviour and
 * delegate here -- the workflow forge and webchat depend on that exact form. */
int git_pr_merge_via_api_slug_ex(const char *principal, const char *slug, int number,
                                 const char *merge_method, const char *expected_head_sha,
                                 char *out_sha, size_t out_sha_cap, char *err, size_t errlen);

/* Merge the base branch INTO the PR's head -- REST's "Update branch" button.
 *
 * A base protected with "require branches to be up to date" reports its required
 * checks as "expected" while the head is BEHIND, so the PR will not merge however
 * green those checks already are. Updating the head is the only way to clear it.
 *
 * expected_head_sha, when given, refuses the update if the head moved since the
 * caller looked -- the same drift guard the merge op uses.
 *
 * Returns 0 when GitHub ACCEPTED the update (202 queued -- the new head is not
 * built yet, so poll merge_status before merging), 1 when the head already
 * contains the base (422, nothing to do), -1 on error with err set. */
int git_pr_update_branch_via_api_slug(const char *principal, const char *slug, int number,
                                      const char *expected_head_sha, char *err, size_t errlen);

#endif /* GIT_PR_API_H */
