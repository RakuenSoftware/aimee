/* test_git_pr_ci_grade.c -- the C half of the CI gate.
 *
 * The aggregation itself moved to the git module
 * (server-go/modules/git/ci_grade.go) and its rules are pinned there, by cases
 * ported one-for-one from the suite this file used to carry. What is left in C,
 * and therefore what this file covers, is:
 *
 *   - the ruling: which verdicts permit a merge;
 *   - the wire: that a verdict NAME from the module maps onto the right
 *     git_pr_ci_t, and that anything unrecognised or unreachable becomes ERROR
 *     rather than NONE -- NONE permits merge, so a garbled or missing answer
 *     must never be able to reach it;
 *   - the conflict predicate, which never left C.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "modules/git/git_pr_api.h"

#include <aimee/audit/obs_bus.h>

/* Bus stub: this test is about the C side's handling of a reply, not about
 * transport, and the grading rules are proven in Go. */
static int g_available = 1;
static const char *g_reply = "{\"verdict\":\"success\"}";
static aimee_module_call_result_t g_result = AIMEE_MODULE_CALL_OK;

int obs_bus_module_available(uint32_t event_kind)
{
   (void)event_kind;
   return g_available;
}

aimee_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)event_kind, (void)stage_id, (void)trace_id, (void)deadline_ns, (void)request_body,
       (void)request_len, (void)cancelled, (void)cancel_context;
   if (g_result != AIMEE_MODULE_CALL_OK)
      return g_result;
   size_t n = strlen(g_reply);
   assert(n <= response_capacity);
   memcpy(response_body, g_reply, n);
   *response_len = (uint32_t)n;
   return AIMEE_MODULE_CALL_OK;
}

static git_pr_ci_t grade_with(const char *reply)
{
   g_available = 1;
   g_result = AIMEE_MODULE_CALL_OK;
   g_reply = reply;
   return git_pr_ci_grade_json("{\"check_runs\":[]}", NULL);
}

int main(void)
{
   printf("git-pr-ci-grade: ");

   /* Squash merges must suppress GitHub's synthesized commit body. Otherwise
    * child commit trailers can be copied into the feature commit and fail the
    * protected no-coauthor-trailers check on the final PR. */
   assert(strstr(GIT_PR_SQUASH_MERGE_JSON, "\"merge_method\":\"squash\"") != NULL);
   assert(strstr(GIT_PR_SQUASH_MERGE_JSON, "\"commit_message\":\"\"") != NULL);

   /* --- the verdict name maps onto the enum --- */
   assert(grade_with("{\"verdict\":\"success\"}") == GIT_PR_CI_SUCCESS);
   assert(grade_with("{\"verdict\":\"pending\"}") == GIT_PR_CI_PENDING);
   assert(grade_with("{\"verdict\":\"failure\"}") == GIT_PR_CI_FAILURE);
   assert(grade_with("{\"verdict\":\"none\"}") == GIT_PR_CI_NONE);
   assert(grade_with("{\"verdict\":\"error\"}") == GIT_PR_CI_ERROR);

   /* --- anything we cannot read is ERROR, never NONE ---
    * NONE permits merge, so it must not be reachable from a reply we failed to
    * understand, a module that answered with the wrong shape, or one that did
    * not answer at all. */
   assert(grade_with("{\"verdict\":\"something-new\"}") == GIT_PR_CI_ERROR);
   assert(grade_with("{\"verdict\":42}") == GIT_PR_CI_ERROR);
   assert(grade_with("{}") == GIT_PR_CI_ERROR);
   assert(grade_with("not json") == GIT_PR_CI_ERROR);

   g_available = 0; /* module not attached */
   assert(git_pr_ci_grade_json("{\"check_runs\":[]}", NULL) == GIT_PR_CI_ERROR);
   g_available = 1;
   g_result = AIMEE_MODULE_CALL_DEADLINE_EXCEEDED; /* module too slow */
   assert(git_pr_ci_grade_json("{\"check_runs\":[]}", NULL) == GIT_PR_CI_ERROR);
   g_result = AIMEE_MODULE_CALL_OK;

   /* --- the merge ruling: only green, or genuinely no CI at all --- */
   assert(git_pr_ci_permits_merge(GIT_PR_CI_SUCCESS));
   assert(git_pr_ci_permits_merge(GIT_PR_CI_NONE)); /* no CI -> nothing to fail */
   assert(!git_pr_ci_permits_merge(GIT_PR_CI_PENDING));
   assert(!git_pr_ci_permits_merge(GIT_PR_CI_FAILURE));
   assert(!git_pr_ci_permits_merge(GIT_PR_CI_ERROR)); /* unknown is never pass */
   /* end to end: an unreadable answer must not reach a merge */
   assert(!git_pr_ci_permits_merge(grade_with("not json")));

   /* The terminal-conflict vs retryable-lost-race classification moved into the
    * git module with the merge itself. Its cases — including the ones that must
    * NOT terminate, which is the direction that wedged a run for 15 attempts
    * over 3 hours — are pinned in
    * server-go/modules/git: TestMergeConflictClassificationFailsSafeTowardRetry. */

   printf("ok\n");
   return 0;
}
