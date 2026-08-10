/* git_pr_ci_grade.c: the CI verdict the merge gate trusts.
 *
 * Split from git_pr_api.c so it links — and unit-tests — without the
 * HTTP/credential stack. The aggregation itself has since moved to the git
 * module (server-go/modules/git/ci_grade.go); what stays here is the request,
 * the ruling, and the pure predicate below, which is a handful of comparisons
 * on the merge path and would gain nothing from a round trip.
 *
 * The conflict-vs-lost-race predicate that used to sit here went with the merge
 * into the module: the message is now read where it arrives instead of being
 * re-derived from a rendered error string. */
#include "git_pr_api.h"

#include "cJSON.h"
#include "headers/module_json_call.h"

#include <aimee/git/module_api.h>
#include <string.h>

/* A forge's check-runs payload for a busy repository is the largest thing this
 * carries; the module body cap bounds it. */
#define GIT_PR_CI_GRADE_MAX_BODY (1024u * 1024u)
/* The caller already spent two HTTP round trips reaching the forge, so a short
 * bus deadline here only converts a slow module into a refused merge. */
#define GIT_PR_CI_GRADE_TIMEOUT_MS 10000

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

/* The grading itself now lives in the git MODULE (server-go/modules/git,
 * ci_grade.go) and is reached as bus stage AIMEE_GIT_STAGE_CI_GRADE. Parsing an
 * untrusted forge payload is feature work; the C core carries the message and
 * applies the ruling.
 *
 * The verdict crosses the wire as a NAME, not as the git_pr_ci_t integer: the
 * enum could be renumbered on one side of the boundary and silently change
 * meaning on the other.
 *
 * Fail-closed twice over. An unreachable module, a malformed reply or an
 * unrecognised verdict all yield ERROR, never NONE -- NONE means "the forge
 * reported no CI", which git_pr_ci_permits_merge() lets through, so it must not
 * be reachable from "we could not get an answer". git is a REQUIRED module, so
 * an absent one is a broken deployment rather than a supported configuration. */
git_pr_ci_t git_pr_ci_grade_json(const char *check_runs_json, const char *combined_status_json)
{
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return GIT_PR_CI_ERROR;
   cJSON_AddStringToObject(request, "check_runs", check_runs_json ? check_runs_json : "");
   cJSON_AddStringToObject(request, "combined_status",
                           combined_status_json ? combined_status_json : "");

   cJSON *reply =
       aimee_module_json_call(AIMEE_GIT_EVENT_CI_GRADE, AIMEE_GIT_STAGE_CI_GRADE, request,
                              GIT_PR_CI_GRADE_MAX_BODY, GIT_PR_CI_GRADE_TIMEOUT_MS, NULL);
   if (!reply)
      return GIT_PR_CI_ERROR;

   const cJSON *verdict = cJSON_GetObjectItemCaseSensitive(reply, "verdict");
   const char *name = cJSON_IsString(verdict) ? verdict->valuestring : "";
   git_pr_ci_t graded = strcmp(name, "success") == 0   ? GIT_PR_CI_SUCCESS
                        : strcmp(name, "pending") == 0 ? GIT_PR_CI_PENDING
                        : strcmp(name, "failure") == 0 ? GIT_PR_CI_FAILURE
                        : strcmp(name, "none") == 0    ? GIT_PR_CI_NONE
                                                       : GIT_PR_CI_ERROR;
   cJSON_Delete(reply);
   return graded;
}
