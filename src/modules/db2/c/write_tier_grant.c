/* db2/write_tier_grant.c: per-user /v1 write authorization (kb_write_tier_grant)
 * — Postgres via libpq. See write_tier_grant.h. Mirrors the db2/admin_grant.c
 * access pattern. Tenant-scoped: every entry requires the RLS-enforcing
 * Postgres backend. */

#include "write_tier_grant.h"
#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

/* Local tier<->wire mapping. Deliberately not shared with the token builder —
 * see the link-boundary note in write_tier_grant.h. The strings must stay
 * identical to the kb_write_tier_grant CHECK constraint and to the token
 * layer's `tier` claim; the round-trip test pins them against both. */
static const char *tier_text(kb_identity_tier_t tier)
{
   switch (tier)
   {
   case KB_IDENTITY_TIER_OFF:
      return "off";
   case KB_IDENTITY_TIER_DATA:
      return "data";
   case KB_IDENTITY_TIER_FULL:
      return "full";
   default:
      return NULL;
   }
}

/* 1 on a recognized tier string. An unrecognized value is a corrupt row, not a
 * tier: the caller must fail closed rather than pick a default. */
static int tier_from_text(const char *s, kb_identity_tier_t *out)
{
   if (!s)
      return 0;
   if (!strcmp(s, "off"))
      *out = KB_IDENTITY_TIER_OFF;
   else if (!strcmp(s, "data"))
      *out = KB_IDENTITY_TIER_DATA;
   else if (!strcmp(s, "full"))
      *out = KB_IDENTITY_TIER_FULL;
   else
      return 0;
   return 1;
}

static int grant_args_valid(const char *server_id, int64_t team_id, const char *subject)
{
   return server_id && server_id[0] && subject && subject[0] && team_id > 0;
}

int db2_write_tier_grant_lookup(const char *server_id, int64_t team_id, const char *subject,
                                kb_identity_tier_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   /* Pass the tenancy code through unchanged: the shim guard asserts every
    * tenant-scoped entrypoint reports the same typed refusal. */
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!out || !grant_args_valid(server_id, team_id, subject))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT tier FROM kb_write_tier_grant WHERE server_id=?1 AND team_id=?2 "
                        "AND subject=?3 AND revoked_at IS NULL LIMIT 1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", server_id);
   aimee_pg_bind_int64(st, "?2", team_id);
   aimee_pg_bind_text(st, "?3", subject);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int result = -1;
   if (step == AIMEE_PG_ROW)
   {
      /* A row whose tier the mapping does not recognize is a corrupt grant.
       * Report an error, not NONE: both deny, but only one is a policy
       * decision. */
      result = tier_from_text(aimee_pg_column_text(st, 0), out) ? DB2_WRITE_TIER_GRANT_FOUND : -1;
   }
   else if (step == AIMEE_PG_DONE)
      result = DB2_WRITE_TIER_GRANT_NONE;
   aimee_pg_finalize(st);
   if (result != DB2_WRITE_TIER_GRANT_FOUND)
      memset(out, 0, sizeof(*out));
   return result;
}

int db2_write_tier_grant_set(const char *server_id, int64_t team_id, const char *subject,
                             kb_identity_tier_t tier, const char *granted_by)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   const char *tier_str = tier_text(tier);
   if (!grant_args_valid(server_id, team_id, subject) || !tier_str || !granted_by || !granted_by[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   /* The definer function is the only write path: it re-checks admin/team-lead
    * authority (SECURITY DEFINER bypasses RLS) and emits the WORM audit row in
    * the same transaction, which runtime cannot do itself. */
   const char *sql = "SELECT kb_write_tier_grant_set(?1, ?2, ?3, ?4, ?5)";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", server_id);
   aimee_pg_bind_int64(st, "?2", team_id);
   aimee_pg_bind_text(st, "?3", subject);
   aimee_pg_bind_text(st, "?4", tier_str);
   aimee_pg_bind_text(st, "?5", granted_by);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_write_tier_grant_set_reporting(const char *server_id, int64_t team_id, const char *subject,
                                       kb_identity_tier_t tier, const char *granted_by,
                                       db2_write_tier_grant_report_t *out)
{
   if (out)
   {
      /* Zeroed, so had_previous = 0: "no previous tier" must not read as tier 0,
       * which is a real tier (off). */
      memset(out, 0, sizeof(*out));
   }
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   const char *tier_str = tier_text(tier);
   if (!out || !grant_args_valid(server_id, team_id, subject) || !tier_str || !granted_by ||
       !granted_by[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   /* The reporting definer function. It DELEGATES to kb_write_tier_grant_set for
    * authorization, validation, the upsert and the audit row, and adds only the
    * observation of the pre-image under the same lock — so this is the same write path as
    * db2_write_tier_grant_set, not a second one. */
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT changed, was_revoked, previous_tier, is_member FROM "
                        "kb_write_tier_grant_set_reporting(?1, ?2, ?3, ?4, ?5)",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", server_id);
   aimee_pg_bind_int64(st, "?2", team_id);
   aimee_pg_bind_text(st, "?3", subject);
   aimee_pg_bind_text(st, "?4", tier_str);
   aimee_pg_bind_text(st, "?5", granted_by);
   db2_write_tier_grant_report_t report;
   memset(&report, 0, sizeof(report));
   int ok = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *changed = aimee_pg_column_text(st, 0);
      const char *revoked = aimee_pg_column_text(st, 1);
      const char *previous = aimee_pg_column_text(st, 2);
      const char *member = aimee_pg_column_text(st, 3);
      report.changed = (changed && (changed[0] == 't' || changed[0] == '1'));
      report.was_revoked = (revoked && (revoked[0] == 't' || revoked[0] == '1'));
      report.is_member = (member && (member[0] == 't' || member[0] == '1'));
      /* NULL means the grant did not exist. A non-NULL value that does not parse is a
       * corrupt row, not an absent one, and must not be reported as either. */
      ok = 1;
      if (previous && previous[0])
      {
         report.had_previous = 1;
         if (!tier_from_text(previous, &report.previous_tier))
            ok = 0;
      }
   }
   aimee_pg_finalize(st);
   if (!ok)
      return -1;
   *out = report;
   return 0;
}

int db2_write_tier_grant_revoke(const char *server_id, int64_t team_id, const char *subject)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!grant_args_valid(server_id, team_id, subject))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   /* Definer write path — see db2_write_tier_grant_set. */
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT kb_write_tier_grant_revoke(?1, ?2, ?3)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", server_id);
   aimee_pg_bind_int64(st, "?2", team_id);
   aimee_pg_bind_text(st, "?3", subject);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_write_tier_grant_list(const char *server_id, int64_t team_id,
                              db2_write_tier_grant_row_t *out, size_t cap, size_t *count)
{
   return db2_write_tier_grant_list_ex(server_id, team_id, 0, NULL, out, cap, count);
}

int db2_write_tier_grant_list_ex(const char *server_id, int64_t team_id, int include_revoked,
                                 const char *subject, db2_write_tier_grant_row_t *out, size_t cap,
                                 size_t *count)
{
   if (count)
      *count = 0;
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!out || !cap || !count || !server_id || !server_id[0] || team_id <= 0)
      return -1;
   memset(out, 0, cap * sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   /* Two statements rather than one with a parameter, so the default path's SQL is
    * unchanged and a reader can see exactly which rows each returns. `include_revoked`
    * WIDENS: it does not select revoked rows only. */
   /* Four statements rather than one with optional predicates, so each returns exactly the
    * rows a reader can see it returns. The SUBJECT FILTER IS IN THE QUERY: applying it after
    * a capped fetch would hide a subject that sorts beyond the cap, and a caller asking
    * about one subject would be told it has no grant because others sort ahead of it. */
   const char *sql;
   if (subject && include_revoked)
      sql = "SELECT subject, tier, granted_by, created_at, updated_at, revoked_at "
            "FROM kb_write_tier_grant WHERE server_id=?1 AND team_id=?2 AND subject=?3 "
            "ORDER BY subject";
   else if (subject)
      sql = "SELECT subject, tier, granted_by, created_at, updated_at, NULL "
            "FROM kb_write_tier_grant WHERE server_id=?1 AND team_id=?2 AND subject=?3 "
            "AND revoked_at IS NULL ORDER BY subject";
   else if (include_revoked)
      sql = "SELECT subject, tier, granted_by, created_at, updated_at, revoked_at "
            "FROM kb_write_tier_grant WHERE server_id=?1 AND team_id=?2 ORDER BY subject";
   else
      sql = "SELECT subject, tier, granted_by, created_at, updated_at, NULL "
            "FROM kb_write_tier_grant "
            "WHERE server_id=?1 AND team_id=?2 AND revoked_at IS NULL ORDER BY subject";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", server_id);
   aimee_pg_bind_int64(st, "?2", team_id);
   if (subject)
      aimee_pg_bind_text(st, "?3", subject);
   size_t n = 0;
   int failed = 0;
   aimee_pg_step_t step;
   while ((step = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW && n < cap)
   {
      db2_write_tier_grant_row_t *row = &out[n];
      const char *c;
      c = aimee_pg_column_text(st, 0);
      snprintf(row->subject, sizeof(row->subject), "%s", c ? c : "");
      if (!tier_from_text(aimee_pg_column_text(st, 1), &row->tier))
      {
         failed = 1; /* a corrupt row must not be reported as a usable grant */
         break;
      }
      c = aimee_pg_column_text(st, 2);
      snprintf(row->granted_by, sizeof(row->granted_by), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 3);
      snprintf(row->created_at, sizeof(row->created_at), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 4);
      snprintf(row->updated_at, sizeof(row->updated_at), "%s", c ? c : "");
      /* Empty on the default path, where the column is a literal NULL. */
      c = aimee_pg_column_text(st, 5);
      snprintf(row->revoked_at, sizeof(row->revoked_at), "%s", c ? c : "");
      ++n;
   }
   if (step == AIMEE_PG_ERR)
      failed = 1;
   aimee_pg_finalize(st);
   if (failed)
   {
      memset(out, 0, cap * sizeof(*out));
      return -1;
   }
   *count = n;
   return 0;
}
