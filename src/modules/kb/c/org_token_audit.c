#include "org_token_audit.h"
#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"
#include <string.h>
int db2_org_token_audit_start(const char *rid, const char *cn, const char *iss, const char *sub,
                              int64_t team, int has_proj, int64_t proj, const char *model,
                              int64_t pv, const char *session, const char *deleg, int64_t *out)
{
   if (db2_tenant_require_pg() != 0 || !rid || !cn || !model)
      return -1;
   void *c = db2_conn();
   if (!c)
      return -1;
   char e[256];
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       c, "SELECT org_token_audit_start(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)", e, sizeof(e));
   if (!s)
      return -1;
   aimee_pg_bind_text(s, "?1", rid);
   aimee_pg_bind_text(s, "?2", cn);
   aimee_pg_bind_text(s, "?3", iss ? iss : "");
   aimee_pg_bind_text(s, "?4", sub ? sub : "");
   aimee_pg_bind_int64(s, "?5", team);
   if (has_proj)
      aimee_pg_bind_int64(s, "?6", proj);
   else
      aimee_pg_bind_null(s, "?6");
   aimee_pg_bind_text(s, "?7", model);
   aimee_pg_bind_int64(s, "?8", pv);
   aimee_pg_bind_text(s, "?9", session ? session : "");
   aimee_pg_bind_text(s, "?10", deleg ? deleg : "");
   aimee_pg_step_t st = aimee_pg_step(s, e, sizeof(e));
   if (st == AIMEE_PG_ROW && out)
      *out = aimee_pg_column_int64(s, 0);
   aimee_pg_finalize(s);
   return st == AIMEE_PG_ROW ? 0 : -1;
}
int db2_org_token_audit_settle(const char *rid, const char *cn, const char *model,
                               const char *served, int64_t pt, int64_t ct, int64_t cr, int64_t cw,
                               const char *cost, const char *state)
{
   if (db2_tenant_require_pg() != 0 || !rid || !cn || !model || !cost || !state)
      return -1;
   void *c = db2_conn();
   if (!c)
      return -1;
   char e[256];
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       c, "SELECT org_token_audit_settle(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)", e, sizeof(e));
   if (!s)
      return -1;
   aimee_pg_bind_text(s, "?1", rid);
   aimee_pg_bind_text(s, "?2", cn);
   aimee_pg_bind_text(s, "?3", model);
   aimee_pg_bind_text(s, "?4", served ? served : "");
   aimee_pg_bind_int64(s, "?5", pt);
   aimee_pg_bind_int64(s, "?6", ct);
   aimee_pg_bind_int64(s, "?7", cr);
   aimee_pg_bind_int64(s, "?8", cw);
   aimee_pg_bind_text(s, "?9", cost);
   aimee_pg_bind_text(s, "?10", state);
   aimee_pg_step_t st = aimee_pg_step(s, e, sizeof(e));
   aimee_pg_finalize(s);
   return st == AIMEE_PG_ROW ? 0 : -1;
}
