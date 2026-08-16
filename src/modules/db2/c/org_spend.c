/* db2/org_spend.c: P3b org spend reporting — Postgres via libpq. See org_spend.h.
 * Mirrors the db2/org_model_catalog.c access pattern: one prepared call into the
 * SECURITY DEFINER aggregation function, the definer's RAISE mapped to a sentinel by
 * message text (libpq surfaces the RAISE message, not the SQLSTATE, in the step error).
 * Reads cost_usd as TEXT (it is NUMERIC — never a double) and carries it through as a
 * string. Tenant-scoped: requires the RLS-enforcing Postgres backend. */

#include "org_spend.h"

#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

/* The definer RAISEs distinct message text per failure class (SQLSTATE isn't surfaced
 * through aimee_pg): 'not authorized' (42501) and 'bad date' (22007). Map to sentinels
 * so the HTTP route can return 403 vs 400 vs 500; everything else is a generic -1. */
static int spend_step_err(const char *err)
{
   if (err && strstr(err, "not authorized"))
      return DB2_SPEND_ERR_DENIED;
   if (err && strstr(err, "bad date"))
      return DB2_SPEND_ERR_BADDATE;
   return -1;
}

int db2_org_spend_query(int has_team, int64_t team, int has_project, int64_t project,
                        const char *since, const char *until, db2_org_spend_row_t *out, int max)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!out || max <= 0 || !since || !until)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   /* org_spend_query(p_team, p_project, p_since, p_until) — SECURITY DEFINER, actor-bound
    * (it reads aimee.principal for its admin/lead predicate). NULL team = org-wide
    * (admin-only) branch; NULL project = no project filter. LIMIT ?5 = max+1 is a cheap
    * overflow probe: reading a (max+1)th row means the report exceeds the caller's buffer,
    * so we return DB2_SPEND_ERR_TOOBIG rather than SILENTLY TRUNCATE — total/by_* always
    * reconcile because a too-large report is an explicit error, never partial data. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT team_id, project_id, billable_model, prompt_tokens, completion_tokens,"
       " cache_read_tokens, cache_write_tokens, cost_usd, calls"
       " FROM org_spend_query(?1, ?2, ?3, ?4) LIMIT ?5",
       err, sizeof(err));
   if (!st)
      return -1;
   if (has_team)
      aimee_pg_bind_int64(st, "?1", team);
   else
      aimee_pg_bind_null(st, "?1");
   if (has_project)
      aimee_pg_bind_int64(st, "?2", project);
   else
      aimee_pg_bind_null(st, "?2");
   aimee_pg_bind_text(st, "?3", since);
   aimee_pg_bind_text(st, "?4", until);
   aimee_pg_bind_int64(st, "?5", (int64_t)max + 1);

   int n = 0;
   int overflow = 0;
   aimee_pg_step_t step = AIMEE_PG_DONE;
   while ((step = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      if (n >= max)
      {
         overflow = 1; /* the (max+1)th row from the LIMIT probe — report too large */
         break;
      }
      db2_org_spend_row_t *r = &out[n++];
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
      const char *c = aimee_pg_column_text(st, 2);
      snprintf(r->billable_model, sizeof(r->billable_model), "%s", c ? c : "");
      r->prompt_tokens = aimee_pg_column_int64(st, 3);
      r->completion_tokens = aimee_pg_column_int64(st, 4);
      r->cache_read_tokens = aimee_pg_column_int64(st, 5);
      r->cache_write_tokens = aimee_pg_column_int64(st, 6);
      /* NUMERIC -> TEXT (never a double). libpq returns the canonical decimal text. */
      c = aimee_pg_column_text(st, 7);
      snprintf(r->cost_usd, sizeof(r->cost_usd), "%s", (c && c[0]) ? c : "0");
      r->calls = aimee_pg_column_int64(st, 8);
   }
   /* Distinguish a clean end-of-rows (DONE) from a RAISE mid-iteration (ERR): on the
    * first step the definer's authz/date RAISE surfaces as ERR with no rows emitted. */
   int failed = (step == AIMEE_PG_ERR);
   aimee_pg_finalize(st);
   if (failed)
      return spend_step_err(err);
   if (overflow)
      return DB2_SPEND_ERR_TOOBIG;
   return n;
}
