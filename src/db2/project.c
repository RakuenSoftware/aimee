/* db2/project.c: P1 tenancy projects (kb_project) — Postgres via libpq.
 * See project.h. Mirrors the db2/enrollments.c access pattern. Tenant-scoped:
 * every entry requires the RLS-enforcing Postgres backend. */

#include "project.h"
#include "db2_tenant.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

static void row_from_stmt(aimee_pg_stmt_t *st, db2_project_row_t *row)
{
   memset(row, 0, sizeof(*row));
   row->id = aimee_pg_column_int64(st, 0);
   row->parent = aimee_pg_column_int64(st, 1);
   const char *c;
   c = aimee_pg_column_text(st, 2);
   snprintf(row->name, sizeof(row->name), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 3);
   snprintf(row->access_mode, sizeof(row->access_mode), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 4);
   snprintf(row->created_at, sizeof(row->created_at), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 5);
   snprintf(row->operator_id, sizeof(row->operator_id), "%s", c ? c : "");
}

#define PROJECT_COLS "id, parent, name, access_mode, created_at, operator_id"

int db2_project_create(int64_t parent, const char *name, const char *access_mode,
                       const char *operator_id, int64_t *out_id)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (parent <= 0 || !name || !name[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   const char *sql = "INSERT INTO kb_project (parent, name, access_mode, operator_id) "
                     "VALUES (?1, ?2, ?3, ?4) RETURNING id";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", parent);
   aimee_pg_bind_text(st, "?2", name);
   aimee_pg_bind_text(st, "?3", (access_mode && access_mode[0]) ? access_mode : "team-open");
   aimee_pg_bind_text(st, "?4", operator_id ? operator_id : "");
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t id = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return -1;
   if (out_id)
      *out_id = id;
   return 0;
}

int db2_project_list(int64_t parent, db2_project_row_t *out, int max)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int scoped = parent > 0;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       scoped ? "SELECT " PROJECT_COLS " FROM kb_project WHERE parent = ?1 ORDER BY id"
              : "SELECT " PROJECT_COLS " FROM kb_project ORDER BY id",
       err, sizeof(err));
   if (!st)
      return -1;
   if (scoped)
      aimee_pg_bind_int64(st, "?1", parent);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      row_from_stmt(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_project_get(int64_t id, db2_project_row_t *out)
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
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT " PROJECT_COLS " FROM kb_project WHERE id = ?1", err, sizeof(err));
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
