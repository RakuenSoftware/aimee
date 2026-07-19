/* db2/db2_tenant.c: per-request tenant context (I4). See db2_tenant.h. */

#include "db2_tenant.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "db2.h" /* db2_lease_begin/_end */
#include "log.h"

#include <string.h>

int db2_tenant_require_pg(void)
{
   /* RLS is a Postgres control; the SQLite test shim cannot enforce it, so a
    * tenant op on the shim is a hard, typed failure — never a silent read. */
   if (aimee_pg_is_shim())
      return DB2_ERR_TENANT_REQUIRES_PG;
   if (!db2_conn())
      return DB2_ERR_TENANT_REQUIRES_PG;
   return 0;
}

/* Belt-and-suspenders: transaction-local GUCs are cleared by COMMIT/ROLLBACK, but
 * RESET any session-level leftover so a pooled connection never carries context. */
static void tenant_reset_gucs(void *conn)
{
   if (!conn)
      return;
   char err[256] = "";
   (void)aimee_pg_exec(conn, "RESET aimee.team", err, sizeof(err));
   (void)aimee_pg_exec(conn, "RESET aimee.principal", err, sizeof(err));
}

int db2_tenant_scope_begin(const kb_principal_t *p, int64_t team)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!p || !p->authenticated)
      return DB2_ERR_TENANT_UNAUTHENTICATED;

   char key[576];
   if (kb_identity_key(p, key, sizeof(key)) != 0)
      return DB2_ERR_TENANT_UNAUTHENTICATED;

   db2_lease_begin(); /* eager lease so the whole unit rides one connection */
   void *conn = db2_conn();
   if (!conn)
   {
      db2_lease_end();
      return DB2_ERR_TENANT_NO_CONN;
   }

   char err[256] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
   {
      db2_lease_end();
      return DB2_ERR_TENANT_BEGIN;
   }

   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, "SELECT set_tenant_context(?1, ?2)", err, sizeof(err));
   if (!st)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      tenant_reset_gucs(conn);
      db2_lease_end();
      return DB2_ERR_TENANT_BEGIN;
   }
   aimee_pg_bind_text(st, "?1", key);
   aimee_pg_bind_int64(st, "?2", team > 0 ? team : 0);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (step == AIMEE_PG_ERR)
   {
      /* set_tenant_context raised (team not in principal's memberships) — the txn
       * is now aborted; roll back and leave no context behind (fail-closed). */
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      tenant_reset_gucs(conn);
      db2_lease_end();
      return DB2_ERR_TENANT_DENIED;
   }
   return 0; /* transaction open, context set, connection held by the lease */
}

int db2_tenant_scope_commit(void)
{
   void *conn = db2_conn();
   int rc = 0;
   if (conn)
   {
      char err[256] = "";
      rc = aimee_pg_exec(conn, "COMMIT", err, sizeof(err));
      tenant_reset_gucs(conn);
   }
   db2_lease_end();
   return rc == 0 ? 0 : -1;
}

void db2_tenant_scope_rollback(void)
{
   void *conn = db2_conn();
   if (conn)
   {
      char err[256] = "";
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      tenant_reset_gucs(conn);
   }
   db2_lease_end();
}
