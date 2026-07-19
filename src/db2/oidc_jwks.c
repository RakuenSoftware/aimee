/* db2/oidc_jwks.c: P1 tenancy fleet-wide trusted JWKS (kb_oidc_jwks) — Postgres
 * via libpq. See oidc_jwks.h. Mirrors the db2/enrollments.c access pattern.
 * Requires the RLS-enforcing Postgres backend. */

#include "oidc_jwks.h"
#include "db2_tenant.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

static void row_from_stmt(aimee_pg_stmt_t *st, db2_jwks_row_t *row)
{
   memset(row, 0, sizeof(*row));
   row->id = aimee_pg_column_int64(st, 0);
   const char *c;
   c = aimee_pg_column_text(st, 1);
   snprintf(row->issuer, sizeof(row->issuer), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 2);
   snprintf(row->kid, sizeof(row->kid), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 3);
   snprintf(row->jwk_json, sizeof(row->jwk_json), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 4);
   snprintf(row->added_at, sizeof(row->added_at), "%s", c ? c : "");
   c = aimee_pg_column_text(st, 5);
   snprintf(row->retired_at, sizeof(row->retired_at), "%s", c ? c : "");
}

#define JWKS_COLS "id, issuer, kid, jwk_json, added_at, retired_at"

int db2_jwks_add(const char *issuer, const char *kid, const char *jwk_json, int64_t *out_id)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!issuer || !issuer[0] || !kid || !kid[0] || !jwk_json)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   const char *sql = "INSERT INTO kb_oidc_jwks (issuer, kid, jwk_json) VALUES (?1, ?2, ?3) "
                     "ON CONFLICT (issuer, kid) DO UPDATE SET jwk_json=EXCLUDED.jwk_json, "
                     "retired_at='' RETURNING id";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", issuer);
   aimee_pg_bind_text(st, "?2", kid);
   aimee_pg_bind_text(st, "?3", jwk_json);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t id = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return -1;
   if (out_id)
      *out_id = id;
   return 0;
}

int db2_jwks_retire(const char *issuer, const char *kid)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!issuer || !issuer[0] || !kid || !kid[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE kb_oidc_jwks SET retired_at=pg_now_text() WHERE issuer=?1 AND kid=?2", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", issuer);
   aimee_pg_bind_text(st, "?2", kid);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_jwks_list_active(const char *issuer, db2_jwks_row_t *out, int max)
{
   int __g = db2_tenant_require_pg();
   if (__g)
      return __g;
   if (!issuer || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT " JWKS_COLS " FROM kb_oidc_jwks WHERE retired_at='' AND issuer=?1 ORDER BY id",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", issuer);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      row_from_stmt(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}
