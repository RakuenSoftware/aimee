/* db2/team.c: P1 tenancy teams (kb_team) — Postgres via libpq.
 * See team.h. Mirrors the db2/enrollments.c access pattern. Tenant-scoped:
 * every entry requires the RLS-enforcing Postgres backend. */

#include "team.h"
#include "db2_tenant.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

static void row_from_stmt(aimee_pg_stmt_t *st, db2_team_row_t *row)
{
   memset(row, 0, sizeof(*row));
   row->id = aimee_pg_column_int64(st, 0);
   const char *c;
   c = aimee_pg_column_text(st, 1);
   snprintf(row->name, sizeof(row->name), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 2);
   snprintf(row->created_at, sizeof(row->created_at), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 3);
   snprintf(row->operator_id, sizeof(row->operator_id), "%s", c ? c : "");
}

#define TEAM_COLS "id, name, created_at, operator_id"

int db2_team_create(const char *name, const char *operator_id, int64_t *out_id)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!name || !name[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   const char *sql = "INSERT INTO kb_team (name, operator_id) VALUES (?1, ?2) RETURNING id";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", name);
   aimee_pg_bind_text(st, "?2", operator_id ? operator_id : "");
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t id = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return -1;
   if (out_id)
      *out_id = id;
   return 0;
}

int db2_team_list(db2_team_row_t *out, int max)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT " TEAM_COLS " FROM kb_team ORDER BY id", err, sizeof(err));
   if (!st)
      return -1;
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      row_from_stmt(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_team_get(int64_t id, db2_team_row_t *out)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT " TEAM_COLS " FROM kb_team WHERE id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      row_from_stmt(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_team_get_by_name(const char *name, db2_team_row_t *out)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!out || !name)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT " TEAM_COLS " FROM kb_team WHERE name = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", name);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      row_from_stmt(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}
