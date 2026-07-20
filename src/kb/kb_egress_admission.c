#include "kb_egress_admission.h"
#include "../db2/org_budget.h"
#include "../db2/org_rate.h"
#include "kb_vault_policy.h"
#include <stdio.h>
#include <string.h>
int kb_egress_admit_dispatch(const kb_egress_admission_t *a, kb_egress_dispatch_fn fn, void *ctx,
                             char *out, size_t cap)
{
   if (!a || !fn || !a->origin_cn || !a->request_id || !a->model || !a->reserve_max || !out ||
       !cap || !kb_vault_live_keys_allowed())
   {
      if (out && cap)
         snprintf(out, cap, "egress unavailable");
      return -1;
   }
   db2_org_rate_result_t rr;
   if (db2_org_rate_check(a->team, a->has_project, a->project, a->model, a->cred_slot, &rr) != 0 ||
       !rr.admitted)
   {
      snprintf(out, cap, "rate limited");
      return -2;
   }
   int bo;
   if (db2_org_budget_reserve(a->origin_cn, a->request_id, a->team, a->has_project, a->project,
                              a->pricing_version, a->reserve_max, 900, &bo) != 0 ||
       bo != DB2_BUDGET_GRANTED)
   {
      snprintf(out, cap, "budget refused");
      return -3;
   }
   char err[256];
   int rc = fn(ctx, out, cap, err, sizeof(err));
   char realized[64] = "0";
   if (rc == 0)
      strcpy(realized, a->reserve_max);
   int settled = 0;
   if (db2_org_budget_settle(a->origin_cn, a->request_id, realized, &settled) != 0)
   {
      snprintf(out, cap, "settlement failed");
      return -4;
   }
   return rc;
}
