/* db2/org_rate.c: P4b keyed fixed-window RPM rate limiter — Postgres via libpq. See
 * org_rate.h. Mirrors db2/org_budget.c: one prepared call into a SECURITY DEFINER
 * function, the definer's RAISE mapped to a sentinel by message text (libpq surfaces the
 * RAISE message, not the SQLSTATE). org_rate_check returns a STRUCTURED row (admitted,
 * binding_dim, reset_epoch) — the stable P2b admission contract, never parsed error text.
 * Every mutation goes through the audited/atomic definer rather than a raw INSERT.
 * Tenant-scoped: requires the RLS-enforcing Postgres backend. */

#include "org_rate.h"

#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

/* Map an admin-gated / actor-bound definer RAISE to a sentinel. The definer RAISEs
 * "<fn>: admin only" / "not authorized" (42501); libpq surfaces the message text. */
static int rate_step_err(const char *err)
{
   if (!err)
      return -1;
   if (strstr(err, "admin only") || strstr(err, "not authorized"))
      return DB2_RATE_ERR_DENIED;
   return -1;
}

int db2_org_rate_policy_set(const char *dim, const char *scope_key, int64_t window_seconds,
                            int64_t max_count, int64_t *out_id)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!dim || !dim[0] || !scope_key || !scope_key[0] || window_seconds <= 0 || max_count < 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT org_rate_policy_set(?1, ?2, ?3, ?4)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", dim);
   aimee_pg_bind_text(st, "?2", scope_key);
   aimee_pg_bind_int64(st, "?3", window_seconds);
   aimee_pg_bind_int64(st, "?4", max_count);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t id = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return rate_step_err(err);
   if (out_id)
      *out_id = id;
   return 0;
}

int db2_org_rate_policy_show(const char *dim, const char *scope_key, db2_org_rate_policy_t *out,
                             int max)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!dim || !dim[0] || !scope_key || !scope_key[0] || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT id, dim, scope_key, window_seconds, max_count FROM org_rate_policy_show(?1, ?2)",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", dim);
   aimee_pg_bind_text(st, "?2", scope_key);

   int n = 0;
   aimee_pg_step_t step = AIMEE_PG_DONE;
   while ((step = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      if (n >= max)
         break;
      db2_org_rate_policy_t *r = &out[n++];
      memset(r, 0, sizeof(*r));
      r->id = aimee_pg_column_int64(st, 0);
      const char *c;
      c = aimee_pg_column_text(st, 1);
      snprintf(r->dim, sizeof(r->dim), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 2);
      snprintf(r->scope_key, sizeof(r->scope_key), "%s", c ? c : "");
      r->window_seconds = aimee_pg_column_int64(st, 3);
      r->max_count = aimee_pg_column_int64(st, 4);
   }
   int failed = (step == AIMEE_PG_ERR);
   aimee_pg_finalize(st);
   if (failed)
      return rate_step_err(err);
   return n;
}

int db2_org_rate_check(int64_t team, int has_project, int64_t project, const char *model,
                       const char *cred_slot, db2_org_rate_result_t *out)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT admitted, binding_dim, reset_epoch FROM org_rate_check(?1, ?2, ?3, ?4)", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", team);
   if (has_project)
      aimee_pg_bind_int64(st, "?2", project);
   else
      aimee_pg_bind_null(st, "?2");
   if (model && model[0])
      aimee_pg_bind_text(st, "?3", model);
   else
      aimee_pg_bind_null(st, "?3");
   if (cred_slot && cred_slot[0])
      aimee_pg_bind_text(st, "?4", cred_slot);
   else
      aimee_pg_bind_null(st, "?4");

   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   memset(out, 0, sizeof(*out));
   if (step == AIMEE_PG_ROW)
   {
      const char *c = aimee_pg_column_text(st, 0);
      out->admitted = (c && (c[0] == 't' || c[0] == 'T' || c[0] == '1')) ? 1 : 0;
      c = aimee_pg_column_is_null(st, 1) ? "" : aimee_pg_column_text(st, 1);
      snprintf(out->binding_dim, sizeof(out->binding_dim), "%s", c ? c : "");
      out->reset_epoch = aimee_pg_column_int64(st, 2);
   }
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return rate_step_err(err);
   return 0;
}
