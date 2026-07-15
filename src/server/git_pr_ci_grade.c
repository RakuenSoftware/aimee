/* git_pr_ci_grade.c: pure aggregation of GitHub check-runs / combined-status
 * payloads into one CI verdict (split from git_pr_api.c so the grader links —
 * and unit-tests — without the HTTP/credential stack). */
#include "git_pr_api.h"

#include "cJSON.h"

#include <string.h>

int git_pr_ci_permits_merge(git_pr_ci_t ci)
{
   /* Enumerated, not a default: a new git_pr_ci_t value must be classified here
    * deliberately rather than inheriting "may merge" by falling through. Lives beside
    * the grader, not in git_pr_api.c, for the reason in this file's header: it is
    * pure policy and must link into a unit test without the HTTP/credential stack. */
   switch (ci)
   {
   case GIT_PR_CI_SUCCESS: /* every check green */
   case GIT_PR_CI_NONE:    /* forge reported no CI at all -> nothing to fail */
      return 1;
   case GIT_PR_CI_PENDING: /* still running -> not yet green */
   case GIT_PR_CI_FAILURE: /* red */
   case GIT_PR_CI_ERROR:   /* undetermined -> never merge on an unknown state */
      return 0;
   }
   return 0; /* unreachable for a valid enum; fail closed regardless */
}

/* One check run / one legacy status context graded into the aggregate. */
static void ci_fold(const char *status, const char *conclusion, int *pending, int *failed,
                    int *seen)
{
   (*seen)++;
   if (status && strcmp(status, "completed") != 0)
   {
      (*pending)++;
      return;
   }
   if (!conclusion || !conclusion[0])
   {
      (*pending)++;
      return;
   }
   if (strcmp(conclusion, "success") == 0 || strcmp(conclusion, "neutral") == 0 ||
       strcmp(conclusion, "skipped") == 0)
      return;
   (*failed)++; /* failure / cancelled / timed_out / action_required / stale */
}

/* A payload that was PROVIDED must parse into the shape we expect. NONE means "the
 * forge reported no CI", which git_pr_ci_permits_merge() treats as mergeable — so it
 * must never be reachable from "we could not read the answer". An unparseable or
 * unexpected body is ERROR (undetermined), which refuses. Both still park the gate.ci
 * node identically (live_ci_status maps NONE and ERROR alike to WFE_CI_NONE), so this
 * distinction costs that path nothing and closes a fail-open on the merge path. */
git_pr_ci_t git_pr_ci_grade_json(const char *check_runs_json, const char *combined_status_json)
{
   int pending = 0, failed = 0, seen = 0;

   if (check_runs_json && check_runs_json[0])
   {
      cJSON *cr = cJSON_Parse(check_runs_json);
      const cJSON *runs = cr ? cJSON_GetObjectItem(cr, "check_runs") : NULL;
      if (!cJSON_IsArray(runs)) /* unparseable, or not a check-runs payload */
      {
         cJSON_Delete(cr);
         return GIT_PR_CI_ERROR;
      }
      const cJSON *r = NULL;
      cJSON_ArrayForEach(r, runs)
      {
         const cJSON *st = cJSON_GetObjectItem(r, "status");
         const cJSON *co = cJSON_GetObjectItem(r, "conclusion");
         ci_fold(cJSON_IsString(st) ? st->valuestring : NULL,
                 cJSON_IsString(co) ? co->valuestring : NULL, &pending, &failed, &seen);
      }
      cJSON_Delete(cr);
   }

   if (seen == 0) /* no check runs: the legacy combined status decides */
   {
      if (!combined_status_json || !combined_status_json[0])
         return GIT_PR_CI_NONE; /* nothing reported anywhere */
      cJSON *cs = cJSON_Parse(combined_status_json);
      const cJSON *statuses = cs ? cJSON_GetObjectItem(cs, "statuses") : NULL;
      if (!cJSON_IsArray(statuses)) /* unparseable, or not a combined-status payload */
      {
         cJSON_Delete(cs);
         return GIT_PR_CI_ERROR;
      }
      if (cJSON_GetArraySize(statuses) > 0)
      {
         const cJSON *state = cJSON_GetObjectItem(cs, "state");
         const char *v = cJSON_IsString(state) ? state->valuestring : "";
         git_pr_ci_t g = strcmp(v, "success") == 0   ? GIT_PR_CI_SUCCESS
                         : strcmp(v, "pending") == 0 ? GIT_PR_CI_PENDING
                                                     : GIT_PR_CI_FAILURE;
         cJSON_Delete(cs);
         return g;
      }
      cJSON_Delete(cs);
      return GIT_PR_CI_NONE; /* genuinely zero checks */
   }
   if (failed > 0)
      return GIT_PR_CI_FAILURE;
   if (pending > 0)
      return GIT_PR_CI_PENDING;
   return GIT_PR_CI_SUCCESS;
}
