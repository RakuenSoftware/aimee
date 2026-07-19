/* db2/admin_grant.c: P1 tenancy org-admin grants (kb_admin_grant) — Postgres via
 * libpq. See admin_grant.h. Mirrors the db2/enrollments.c access pattern.
 * Tenant-scoped: every entry requires the RLS-enforcing Postgres backend. */

#include "admin_grant.h"
#include "db2_tenant.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

static void row_from_stmt(aimee_pg_stmt_t *st, db2_admin_grant_row_t *row)
{
   memset(row, 0, sizeof(*row));
   row->id = aimee_pg_column_int64(st, 0);
   const char *c;
   c = aimee_pg_column_text(st, 1);
   snprintf(row->identity_key, sizeof(row->identity_key), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 2);
   snprintf(row->source, sizeof(row->source), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 3);
   snprintf(row->granted_at, sizeof(row->granted_at), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 4);
   snprintf(row->granted_by, sizeof(row->granted_by), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 5);
   snprintf(row->revoked_at, sizeof(row->revoked_at), "%s", c ? c : "");
}

#define ADMIN_GRANT_COLS "id, identity_key, source, granted_at, granted_by, revoked_at"

int db2_admin_grant_add(const char *identity_key, const char *source, const char *granted_by,
                        int64_t *out_id)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!identity_key || !identity_key[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   const char *sql = "INSERT INTO kb_admin_grant (identity_key, source, granted_by) "
                     "VALUES (?1, ?2, ?3) "
                     "ON CONFLICT (identity_key) DO UPDATE SET source=EXCLUDED.source, "
                     "granted_by=EXCLUDED.granted_by, granted_at=pg_now_text(), revoked_at='' "
                     "RETURNING id";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", identity_key);
   aimee_pg_bind_text(st, "?2", source ? source : "");
   aimee_pg_bind_text(st, "?3", granted_by ? granted_by : "");
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t id = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return -1;
   if (out_id)
      *out_id = id;
   return 0;
}

int db2_admin_grant_revoke(const char *identity_key)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!identity_key || !identity_key[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE kb_admin_grant SET revoked_at=pg_now_text() "
             "WHERE identity_key=?1 AND revoked_at=''",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", identity_key);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_admin_grant_is_active(const char *identity_key)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!identity_key || !identity_key[0])
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT " ADMIN_GRANT_COLS " FROM kb_admin_grant WHERE identity_key=?1 AND revoked_at='' "
       "LIMIT 1",
       err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", identity_key);
   int active = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_admin_grant_row_t row;
      row_from_stmt(st, &row);
      active = (row.revoked_at[0] == '\0') ? 1 : 0; /* WHERE already filters, belt-and-suspenders */
   }
   aimee_pg_finalize(st);
   return active;
}
