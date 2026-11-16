/* kb_store/org_budget.c: P4a budget reservation core — Postgres via libpq. See org_budget.h.
 * Mirrors the kb_store/org_model_catalog.c / kb_store/org_spend.c access pattern: one prepared
 * call into a SECURITY DEFINER function, the definer's RAISE mapped to a sentinel by
 * message text (libpq surfaces the RAISE message, not the SQLSTATE). All money is bound
 * and read as NUMERIC TEXT (never a double), so a hard cap is exact. Every mutation goes
 * through the audited/atomic definer rather than a raw INSERT. Tenant-scoped: requires
 * the RLS-enforcing Postgres backend. */

#include "org_budget.h"

#include "kb_store_internal.h"
#include "kb_store_tenant.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

/* Map an admin-gated / actor-bound definer RAISE to a sentinel. The definer RAISEs
 * "<fn>: admin only" / "not authorized" (42501), "mismatched attributes" (23505), and
 * "retroactive reduction" (23514); libpq surfaces the message text (not the SQLSTATE). */
static int budget_step_err(const char *err)
{
   if (!err)
      return -1;
   if (strstr(err, "admin only") || strstr(err, "not authorized"))
      return KB_STORE_BUDGET_ERR_DENIED;
   if (strstr(err, "mismatched attributes"))
      return KB_STORE_BUDGET_ERR_CONFLICT;
   if (strstr(err, "retroactive reduction"))
      return KB_STORE_BUDGET_ERR_RETRO;
   return -1;
}

int kb_store_org_budget_set(int64_t team, int has_project, int64_t project, const char *period,
                            const char *limit_usd, const char *soft_limit_usd, int64_t *out_id)
{
   int g = kb_store_tenant_require_pg();
   if (g)
      return g;
   if (!period || !period[0] || !limit_usd || !limit_usd[0])
      return -1;
   if (strcmp(period, "day") != 0 && strcmp(period, "month") != 0)
      return -1;
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT org_budget_set(?1, ?2, ?3, ?4, ?5)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", team);
   if (has_project)
      aimee_pg_bind_int64(st, "?2", project);
   else
      aimee_pg_bind_null(st, "?2");
   aimee_pg_bind_text(st, "?3", period);
   /* NUMERIC args: bind decimal text so libpq's unknown-type param coerces to numeric. */
   aimee_pg_bind_text(st, "?4", limit_usd);
   if (soft_limit_usd && soft_limit_usd[0])
      aimee_pg_bind_text(st, "?5", soft_limit_usd);
   else
      aimee_pg_bind_null(st, "?5");
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t id = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return budget_step_err(err);
   if (out_id)
      *out_id = id;
   return 0;
}

int kb_store_org_budget_show(int64_t team, int has_project, int64_t project,
                             kb_store_org_budget_row_t *out, int max)
{
   int g = kb_store_tenant_require_pg();
   if (g)
      return g;
   if (!out || max <= 0)
      return -1;
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT team_id, project_id, period, limit_usd, soft_limit_usd, period_id,"
                        " spend_usd, reserved_usd, remaining_usd FROM org_budget_show(?1, ?2)",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", team);
   if (has_project)
      aimee_pg_bind_int64(st, "?2", project);
   else
      aimee_pg_bind_null(st, "?2");

   int n = 0;
   aimee_pg_step_t step = AIMEE_PG_DONE;
   while ((step = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      if (n >= max)
         break;
      kb_store_org_budget_row_t *r = &out[n++];
      memset(r, 0, sizeof(*r));
      r->team_id = aimee_pg_column_int64(st, 0);
      if (aimee_pg_column_is_null(st, 1))
      {
         r->has_project = 0;
         r->project_id = 0;
      }
      else
      {
         r->has_project = 1;
         r->project_id = aimee_pg_column_int64(st, 1);
      }
      const char *c;
      c = aimee_pg_column_text(st, 2);
      snprintf(r->period, sizeof(r->period), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 3);
      snprintf(r->limit_usd, sizeof(r->limit_usd), "%s", (c && c[0]) ? c : "0");
      c = aimee_pg_column_is_null(st, 4) ? "" : aimee_pg_column_text(st, 4);
      snprintf(r->soft_limit_usd, sizeof(r->soft_limit_usd), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 5);
      snprintf(r->period_id, sizeof(r->period_id), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 6);
      snprintf(r->spend_usd, sizeof(r->spend_usd), "%s", (c && c[0]) ? c : "0");
      c = aimee_pg_column_text(st, 7);
      snprintf(r->reserved_usd, sizeof(r->reserved_usd), "%s", (c && c[0]) ? c : "0");
      c = aimee_pg_column_text(st, 8);
      snprintf(r->remaining_usd, sizeof(r->remaining_usd), "%s", (c && c[0]) ? c : "0");
   }
   int failed = (step == AIMEE_PG_ERR);
   aimee_pg_finalize(st);
   if (failed)
      return budget_step_err(err);
   return n;
}
