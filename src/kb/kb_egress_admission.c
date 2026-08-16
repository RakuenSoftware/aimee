#include "kb_egress_admission.h"
#include "../modules/db2/c/org_budget.h"
#include "../modules/db2/c/org_rate.h"
#include "kb_vault_policy.h"
#include "../modules/db2/c/org_token_audit.h"
#include <stdio.h>
#include <string.h>
int kb_egress_admit_dispatch(const kb_egress_admission_t *a, kb_egress_dispatch_fn fn, void *ctx,
                             char *out, size_t cap)
{
   /* The credential slot is authoritative catalog state.  Requiring it at
    * admission prevents a caller from reaching a dispatch seam with an
    * unbound/implicit credential (which would otherwise be easy to turn into
    * a credential-redirection or SSRF bug in a future driver). */
   if (!a || !fn || !a->origin_cn || !a->request_id || !a->model || !a->cred_slot ||
       !a->cred_slot[0] || !a->reserve_max || !a->reserve_max[0] || !out || !cap ||
       !kb_vault_live_keys_allowed())
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
   int64_t audit_id = 0;
   if (db2_org_token_audit_start(a->request_id, a->origin_cn, a->actor_issuer, a->actor_subject,
                                 a->team, a->has_project, a->project, a->model, a->pricing_version,
                                 a->session_id, a->delegation_id, &audit_id) != 0)
   {
      int ignored = 0;
      (void)db2_org_budget_settle(a->origin_cn, a->request_id, "0", &ignored);
      snprintf(out, cap, "audit unavailable");
      return -4;
   }
   (void)audit_id;
   char err[256];
   int rc = fn(ctx, out, cap, err, sizeof(err));
   char realized[64] = "0";
   if (rc == 0)
      strcpy(realized, a->reserve_max);
   int settled = 0;
   if (db2_org_budget_settle(a->origin_cn, a->request_id, realized, &settled) != 0)
   {
      snprintf(out, cap, "settlement failed");
      return -5;
   }
   if (db2_org_token_audit_settle(a->request_id, a->origin_cn, a->model, a->model, 0, 0, 0, 0,
                                  realized, rc == 0 ? "settled" : "failed") != 0)
   {
      snprintf(out, cap, "audit settlement failed");
      return -6;
   }
   return rc;
}
