#include "kb_egress_admission.h"
#include "../db2/org_budget.h"
#include "../db2/org_rate.h"
#include "kb_vault_policy.h"
#include "../db2/org_token_audit.h"
#include <stdio.h>
#include <string.h>
typedef struct { const kb_bedrock_target_t *target; const aimee_request_t *ir; int stream;
                 const char *ak,*sk,*tok,*amz,*date,*ca,*cc; char *out; size_t cap; int *status; } bedrock_ctx;
static int bedrock_cb(void *v,char *out,size_t cap,char *err,size_t errlen)
{ bedrock_ctx *c=v; int rc=kb_bedrock_dispatch_https(c->target,c->ir,c->stream,c->ak,c->sk,c->tok,c->amz,c->date,c->ca,c->cc,out,cap,c->status); if(rc!=0&&err&&errlen)snprintf(err,errlen,"bedrock dispatch failed"); return rc; }
int kb_egress_bedrock_dispatch(const kb_egress_admission_t *a,const kb_bedrock_target_t*t,const aimee_request_t*ir,int stream,const char*ak,const char*sk,const char*tok,const char*amz,const char*date,const char*ca,const char*cc,char*out,size_t cap,int*status)
{ if(!t||!ir||!ak||!sk||!amz||!date||!ca||!out||!cap)return -1; bedrock_ctx c={t,ir,stream,ak,sk,tok,amz,date,ca,cc,out,cap,status}; return kb_egress_admit_dispatch(a,bedrock_cb,&c,out,cap); }
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
   int64_t audit_id = 0;
   if (db2_org_token_audit_start(a->request_id, a->origin_cn, a->actor_issuer,
                                 a->actor_subject, a->team, a->has_project, a->project,
                                 a->model, a->pricing_version, a->session_id,
                                 a->delegation_id, &audit_id) != 0)
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
   if (db2_org_token_audit_settle(a->request_id, a->origin_cn, a->model, a->model,
                                  0, 0, 0, 0, realized, rc == 0 ? "settled" : "failed") != 0)
   {
      snprintf(out, cap, "audit settlement failed");
      return -6;
   }
   return rc;
}
