/* wfe_iface.c -- the narrow executor vtable + step-result constructors. */
#include "wfe_iface.h"

#include <math.h> /* isfinite */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp / strncasecmp */
#include <sys/stat.h> /* stat — wfe_repo_local dir check */

/* ---- per-work-item repo resolution (see wfe_iface.h) ---- */

const char *wfe_repo_local(const char *wi_repo)
{
   /* The work item's own repo binds the run to its repository (a trigger rule's
    * pipeline.workspace, or an explicit dev-submit repo) — honor it whenever it
    * names a local directory. Anything else (an identifier/URL, or "") falls back
    * to the process-wide $AIMEE_WORKFLOW_REPO, then cwd, preserving
    * single-repo deployments unchanged. */
   if (wi_repo && wi_repo[0] == '/')
   {
      struct stat st;
      if (stat(wi_repo, &st) == 0 && S_ISDIR(st.st_mode))
         return wi_repo;
   }
   const char *d = getenv("AIMEE_WORKFLOW_REPO");
   return (d && d[0]) ? d : ".";
}

/* ---- autonomous merge-target rail (WP-5 safety; see wfe_iface.h) ---- */

const char *wfe_autonomous_base(void)
{
   const char *b = getenv("AIMEE_AUTONOMY_BASE");
   return (b && b[0]) ? b : "testing";
}

int wfe_base_is_protected(const char *branch)
{
   if (!branch || !branch[0])
      return 1; /* empty -> treat as protected (fail closed) */
   /* Case-insensitive: 'Main'/'MASTER' must not slip past. */
   if (strcasecmp(branch, "main") == 0 || strcasecmp(branch, "master") == 0)
      return 1;
   /* Protect the release-train namespace ("release/..." or "release-<ver>") without
    * snagging an unrelated branch that merely starts with the word "release"
    * (e.g. "release-notes-edit"): require a separator + a version-ish char. */
   if (strncasecmp(branch, "release", 7) == 0 && (branch[7] == '/' || branch[7] == '-') &&
       (branch[8] == 'v' || (branch[8] >= '0' && branch[8] <= '9')))
      return 1;
   return 0;
}

int wfe_autonomous_target_ok(void)
{
   return !wfe_base_is_protected(wfe_autonomous_base());
}

/* Server-side authoritative cost estimate (WP-5): a delegate turn's USD cost as
 * wall-clock seconds * a configured rate. Provider-agnostic (never trusts a
 * provider-reported figure) so the per-run USD budget cap actually bites. Rate is
 * AIMEE_AUTONOMY_USD_PER_SEC (default 0.0005 ~= $1.80/hr of delegate wall-clock); a
 * malformed/negative override falls back to the default. Negative elapsed -> 0. */
double wfe_autonomy_cost_estimate(double elapsed_secs)
{
   double rate = 0.0005;
   const char *v = getenv("AIMEE_AUTONOMY_USD_PER_SEC");
   if (v && v[0])
   {
      char *end = NULL;
      double r = strtod(v, &end);
      /* require finite AND strictly positive: inf/NaN or rate==0 would silently
       * neutralize the budget cap (every turn would cost 0 or inf). Fall back. */
      if (end && *end == '\0' && isfinite(r) && r > 0)
         rate = r;
   }
   if (!(elapsed_secs > 0)) /* also rejects NaN */
      return 0;
   return elapsed_secs * rate;
}

static wfe_block_exec_fn g_execs[WFE_BLK__COUNT];

void wfe_register_block_executor(wfe_block_type_t type, wfe_block_exec_fn fn)
{
   if (type > WFE_BLK_UNKNOWN && type < WFE_BLK__COUNT)
      g_execs[type] = fn;
}

wfe_block_exec_fn wfe_lookup_block_executor(wfe_block_type_t type)
{
   if (type > WFE_BLK_UNKNOWN && type < WFE_BLK__COUNT)
      return g_execs[type];
   return NULL;
}

void wfe_reset_block_executors(void)
{
   memset(g_execs, 0, sizeof g_execs);
}

wfe_step_result_t wfe_step_advanced(const char *artifact_handle, const char *content_hash,
                                    double cost_usd)
{
   wfe_step_result_t r;
   memset(&r, 0, sizeof r);
   r.status = WFE_STEP_ADVANCED;
   if (artifact_handle)
      snprintf(r.artifact_handle, sizeof r.artifact_handle, "%s", artifact_handle);
   if (content_hash)
      snprintf(r.content_hash, sizeof r.content_hash, "%s", content_hash);
   r.cost_usd = cost_usd;
   return r;
}

wfe_step_result_t wfe_step_pending(wfe_pause_reason_t reason)
{
   wfe_step_result_t r;
   memset(&r, 0, sizeof r);
   r.status = WFE_STEP_PENDING;
   r.pause_reason = reason;
   return r;
}

wfe_step_result_t wfe_step_failed(void)
{
   wfe_step_result_t r;
   memset(&r, 0, sizeof r);
   r.status = WFE_STEP_FAILED;
   /* Back-compat: an unclassified failed() is a conservative terminal stop. */
   r.failure_class = WFE_FAIL_PERMANENT;
   return r;
}

wfe_step_result_t wfe_step_failed_class(wfe_failure_class_t cls, int has_new_input)
{
   wfe_step_result_t r;
   memset(&r, 0, sizeof r);
   r.status = WFE_STEP_FAILED;
   r.failure_class = (cls == WFE_FAIL_NONE) ? WFE_FAIL_PERMANENT : cls;
   r.failure_has_new_input = has_new_input ? 1 : 0;
   return r;
}

wfe_failure_disposition_t wfe_failure_disposition(wfe_failure_class_t cls, int has_new_input)
{
   switch (cls)
   {
   case WFE_FAIL_TRANSIENT:
      /* The core invariant: retry ONLY with genuinely new input, else park stuck so
       * the scheduler cannot spin on an identical re-dispatch. */
      return has_new_input ? WFE_FDISP_RETRY : WFE_FDISP_PARK_STUCK;
   case WFE_FAIL_DEGRADED:
   case WFE_FAIL_BUDGET:
   case WFE_FAIL_FORGE:
      return WFE_FDISP_PARK_HUMAN;
   case WFE_FAIL_REFUSAL:
   case WFE_FAIL_PERMANENT:
   case WFE_FAIL_CORRUPTION:
   case WFE_FAIL_NONE:
   default:
      return WFE_FDISP_TERMINAL;
   }
}

wfe_step_result_t wfe_step_looped(void)
{
   wfe_step_result_t r;
   memset(&r, 0, sizeof r);
   r.status = WFE_STEP_LOOPED;
   return r;
}
