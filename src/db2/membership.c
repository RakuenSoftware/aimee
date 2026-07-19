/* db2/membership.c: P1 tenancy team membership (kb_team_membership) — Postgres
 * via libpq. See membership.h. Mirrors the db2/enrollments.c access pattern.
 * Tenant-scoped: every entry requires the RLS-enforcing Postgres backend. */

#include "membership.h"
#include "db2_tenant.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

static void row_from_stmt(aimee_pg_stmt_t *st, db2_membership_row_t *row)
{
   memset(row, 0, sizeof(*row));
   row->id = aimee_pg_column_int64(st, 0);
   const char *c;
   c = aimee_pg_column_text(st, 1);
   snprintf(row->identity_key, sizeof(row->identity_key), "%s", c ? c : "");
   row->team = aimee_pg_column_int64(st, 2);
   row->is_default = (int)aimee_pg_column_int64(st, 3);
   c = aimee_pg_column_text(st, 4);
   snprintf(row->created_at, sizeof(row->created_at), "%s", c ? c : "");
}

#define MEMBERSHIP_COLS "id, identity_key, team, is_default, created_at"

int db2_membership_add(const char *identity_key, int64_t team, int is_default, int64_t *out_id)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!identity_key || !identity_key[0] || team <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   const char *sql = "INSERT INTO kb_team_membership (identity_key, team, is_default) "
                     "VALUES (?1, ?2, ?3) "
                     "ON CONFLICT (identity_key, team) DO NOTHING RETURNING id";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", identity_key);
   aimee_pg_bind_int64(st, "?2", team);
   aimee_pg_bind_int64(st, "?3", is_default ? 1 : 0);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ROW && out_id)
      *out_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   /* DONE (no RETURNING row) means the membership already existed — idempotent OK. */
   if (step != AIMEE_PG_ROW && step != AIMEE_PG_DONE)
      return -1;
   return 0;
}

int db2_membership_remove(const char *identity_key, int64_t team)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!identity_key || !identity_key[0] || team <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "DELETE FROM kb_team_membership WHERE identity_key = ?1 AND team = ?2", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", identity_key);
   aimee_pg_bind_int64(st, "?2", team);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_membership_list_for_identity(const char *identity_key, db2_membership_row_t *out, int max)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!identity_key || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT " MEMBERSHIP_COLS " FROM kb_team_membership WHERE identity_key = ?1 ORDER BY id",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", identity_key);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      row_from_stmt(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_membership_teams(const char *identity_key, int64_t *out_teams, int max)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!identity_key || !out_teams || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT team FROM kb_team_membership WHERE identity_key = ?1 ORDER BY id", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", identity_key);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      out_teams[n++] = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_membership_default_team(const char *identity_key, int64_t *out_team)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!identity_key || !out_team)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT team FROM kb_team_membership WHERE identity_key = ?1 AND is_default = 1 LIMIT 1", err,
       sizeof(err));
   if (!st)
      return -1;
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      *out_team = aimee_pg_column_int64(st, 0);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}
