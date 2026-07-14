/* git_pr_ci_grade.c: pure aggregation of GitHub check-runs / combined-status
 * payloads into one CI verdict (split from git_pr_api.c so the grader links —
 * and unit-tests — without the HTTP/credential stack). */
#include "git_pr_api.h"

#include "cJSON.h"

#include <string.h>

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

git_pr_ci_t git_pr_ci_grade_json(const char *check_runs_json, const char *combined_status_json)
{
   int pending = 0, failed = 0, seen = 0;

   cJSON *cr = check_runs_json ? cJSON_Parse(check_runs_json) : NULL;
   const cJSON *runs = cr ? cJSON_GetObjectItem(cr, "check_runs") : NULL;
   if (cJSON_IsArray(runs))
   {
      const cJSON *r = NULL;
      cJSON_ArrayForEach(r, runs)
      {
         const cJSON *st = cJSON_GetObjectItem(r, "status");
         const cJSON *co = cJSON_GetObjectItem(r, "conclusion");
         ci_fold(cJSON_IsString(st) ? st->valuestring : NULL,
                 cJSON_IsString(co) ? co->valuestring : NULL, &pending, &failed, &seen);
      }
   }
   cJSON_Delete(cr);

   if (seen == 0) /* no check runs: the legacy combined status decides */
   {
      cJSON *cs = combined_status_json ? cJSON_Parse(combined_status_json) : NULL;
      const cJSON *statuses = cs ? cJSON_GetObjectItem(cs, "statuses") : NULL;
      if (cJSON_IsArray(statuses) && cJSON_GetArraySize(statuses) > 0)
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
      return GIT_PR_CI_NONE;
   }
   if (failed > 0)
      return GIT_PR_CI_FAILURE;
   if (pending > 0)
      return GIT_PR_CI_PENDING;
   return GIT_PR_CI_SUCCESS;
}
