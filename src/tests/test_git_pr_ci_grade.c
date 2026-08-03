/* test_git_pr_ci_grade.c -- the pure CI aggregation the live forge's gate.ci
 * trusts: check runs first (any failed/cancelled -> FAILURE beats pending; any
 * queued/in-progress -> PENDING; all green-ish -> SUCCESS), legacy combined
 * status only when no check runs exist, and NONE when neither reports. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "modules/git/git_pr_api.h"

#define RUNS(x)     "{\"total_count\":9,\"check_runs\":[" x "]}"
#define RUN(st, co) "{\"status\":\"" st "\",\"conclusion\":" co "}"

int main(void)
{
   printf("git-pr-ci-grade: ");

   /* Squash merges must suppress GitHub's synthesized commit body. Otherwise
    * child commit trailers can be copied into the feature commit and fail the
    * protected no-coauthor-trailers check on the final PR. */
   assert(strstr(GIT_PR_SQUASH_MERGE_JSON, "\"merge_method\":\"squash\"") != NULL);
   assert(strstr(GIT_PR_SQUASH_MERGE_JSON, "\"commit_message\":\"\"") != NULL);

   /* all completed successfully (neutral/skipped count as green) */
   assert(git_pr_ci_grade_json(RUNS(RUN("completed", "\"success\"") "," RUN(
                                   "completed", "\"neutral\"") "," RUN("completed", "\"skipped\"")),
                               NULL) == GIT_PR_CI_SUCCESS);
   /* one still running -> pending */
   assert(git_pr_ci_grade_json(RUNS(RUN("completed", "\"success\"") "," RUN("in_progress", "null")),
                               NULL) == GIT_PR_CI_PENDING);
   /* a failure wins even while others still run */
   assert(git_pr_ci_grade_json(RUNS(RUN("in_progress", "null") "," RUN("completed", "\"failure\"")),
                               NULL) == GIT_PR_CI_FAILURE);
   /* cancelled / timed_out block too */
   assert(git_pr_ci_grade_json(RUNS(RUN("completed", "\"cancelled\"")), NULL) == GIT_PR_CI_FAILURE);
   /* completed with null conclusion is not a pass */
   assert(git_pr_ci_grade_json(RUNS(RUN("completed", "null")), NULL) == GIT_PR_CI_PENDING);

   /* no check runs: the combined status decides */
   const char *empty = "{\"total_count\":0,\"check_runs\":[]}";
   assert(git_pr_ci_grade_json(empty, "{\"state\":\"success\",\"statuses\":[{}]}") ==
          GIT_PR_CI_SUCCESS);
   assert(git_pr_ci_grade_json(empty, "{\"state\":\"pending\",\"statuses\":[{}]}") ==
          GIT_PR_CI_PENDING);
   assert(git_pr_ci_grade_json(empty, "{\"state\":\"failure\",\"statuses\":[{}]}") ==
          GIT_PR_CI_FAILURE);
   /* combined status with ZERO statuses is "pending" by GitHub convention but
    * means "no CI at all" -> NONE (the gate parks rather than waiting forever) */
   assert(git_pr_ci_grade_json(empty, "{\"state\":\"pending\",\"statuses\":[]}") == GIT_PR_CI_NONE);
   /* nothing anywhere -> NONE */
   assert(git_pr_ci_grade_json(empty, NULL) == GIT_PR_CI_NONE);
   assert(git_pr_ci_grade_json(NULL, NULL) == GIT_PR_CI_NONE);

   /* A payload we cannot read is ERROR, never NONE. NONE means "the forge reported
    * no CI", which git_pr_ci_permits_merge() lets through -- so it must not be
    * reachable from "we failed to parse the answer", or a garbled body would merge. */
   assert(git_pr_ci_grade_json("not json", "also not json") == GIT_PR_CI_ERROR);
   assert(git_pr_ci_grade_json(empty, "not json") == GIT_PR_CI_ERROR);
   /* parsed, but not the payload we asked for (e.g. an API error object) */
   assert(git_pr_ci_grade_json("{\"message\":\"Not Found\"}", NULL) == GIT_PR_CI_ERROR);
   assert(git_pr_ci_grade_json(empty, "{\"message\":\"Not Found\"}") == GIT_PR_CI_ERROR);

   /* --- the merge ruling: only green, or genuinely no CI at all --- */
   assert(git_pr_ci_permits_merge(GIT_PR_CI_SUCCESS));
   assert(git_pr_ci_permits_merge(GIT_PR_CI_NONE)); /* no CI -> nothing to fail */
   assert(!git_pr_ci_permits_merge(GIT_PR_CI_PENDING));
   assert(!git_pr_ci_permits_merge(GIT_PR_CI_FAILURE));
   assert(!git_pr_ci_permits_merge(GIT_PR_CI_ERROR)); /* unknown is never pass */
   /* end to end: a malformed payload must not reach a merge */
   assert(!git_pr_ci_permits_merge(git_pr_ci_grade_json("not json", "also not json")));

   /* --- terminal conflict vs retryable lost race (both arrive as 405/409) ---
    * A content conflict is identical on every retry, so it must be terminal; a
    * moved head/base is a lost race a retry wins. Getting this wrong in the
    * retryable direction wedges a run forever (observed: 15 attempts over 3
    * hours); getting it wrong in the terminal direction kills a run that would
    * have merged. Hence the predicate fails SAFE toward retry. */
   assert(git_pr_merge_err_is_conflict("github API (pr merge, HTTP 405): Pull Request has merge "
                                       "conflicts"));
   assert(git_pr_merge_err_is_conflict("merge conflict"));           /* minimal phrasing */
   assert(git_pr_merge_err_is_conflict("MERGE CONFLICTS DETECTED")); /* case-insensitive */

   /* Lost races must stay retryable — these are the messages a retry wins. */
   assert(!git_pr_merge_err_is_conflict(
       "github API (pr merge, HTTP 409): Head branch was modified. Review and try the merge "
       "again."));
   assert(!git_pr_merge_err_is_conflict("github API (pr merge, HTTP 405): Base branch was "
                                        "modified. Review and try the merge again."));
   /* The bare word must NOT terminate: HTTP 409 is named "Conflict", so a
    * lost-race message can carry it with no content conflict involved. */
   assert(!git_pr_merge_err_is_conflict("github API (pr merge, HTTP 409): Conflict"));
   /* Unrecognised / empty degrade to retry, never to a terminal kill. */
   assert(!git_pr_merge_err_is_conflict("github API (pr merge, HTTP 405): failed"));
   assert(!git_pr_merge_err_is_conflict(""));
   assert(!git_pr_merge_err_is_conflict(NULL));

   printf("ok\n");
   return 0;
}
