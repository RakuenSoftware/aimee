/* test_git_pr_ci_grade.c -- the pure CI aggregation the live forge's gate.ci
 * trusts: check runs first (any failed/cancelled -> FAILURE beats pending; any
 * queued/in-progress -> PENDING; all green-ish -> SUCCESS), legacy combined
 * status only when no check runs exist, and NONE when neither reports. */
#include <assert.h>
#include <stdio.h>

#include "git_pr_api.h"

#define RUNS(x)     "{\"total_count\":9,\"check_runs\":[" x "]}"
#define RUN(st, co) "{\"status\":\"" st "\",\"conclusion\":" co "}"

int main(void)
{
   printf("git-pr-ci-grade: ");

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

   printf("ok\n");
   return 0;
}
