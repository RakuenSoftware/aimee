/* db2/db2_tenant.c: per-request tenant context (I4). See db2_tenant.h. */

#include "db2_tenant.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "db2.h" /* db2_lease_begin/_end */
#include "log.h"

#include <string.h>

typedef struct
{
   db2_maintenance_worker_t worker;
   char project[256];
} maintenance_job_t;

static _Thread_local maintenance_job_t g_maintenance_job;

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
   (void)aimee_pg_exec(conn, "RESET aimee.maintenance_worker", err, sizeof(err));
   (void)aimee_pg_exec(conn, "RESET aimee.maintenance_project", err, sizeof(err));
   (void)aimee_pg_exec(conn, "RESET aimee.maintenance_kb_project", err, sizeof(err));
}

static const char *maintenance_worker_name(db2_maintenance_worker_t worker)
{
   switch (worker)
   {
   case DB2_MAINTENANCE_INGEST:
      return "ingest";
   case DB2_MAINTENANCE_REEMBED:
      return "reembed";
   case DB2_MAINTENANCE_CURATOR:
      return "curator";
   case DB2_MAINTENANCE_CODE_INDEXER:
      return "code-indexer";
   }
   return NULL;
}

int db2_maintenance_job_enter(db2_maintenance_worker_t worker, const char *project)
{
   if (g_maintenance_job.worker != 0 || !maintenance_worker_name(worker) || !project ||
       !project[0] || strlen(project) >= sizeof(g_maintenance_job.project))
      return DB2_ERR_MAINTENANCE_INVALID;
   g_maintenance_job.worker = worker;
   memcpy(g_maintenance_job.project, project, strlen(project) + 1);
   return 0;
}

void db2_maintenance_job_leave(void)
{
   memset(&g_maintenance_job, 0, sizeof(g_maintenance_job));
}

int db2_maintenance_job_active(void)
{
   return g_maintenance_job.worker != 0;
}

static int content_scope_enforced(void)
{
   db2_lease_begin();
   void *conn = db2_conn();
   if (!conn || aimee_pg_is_shim())
   {
      db2_lease_end();
      return 0;
   }
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT count(*) FROM pg_class"
                        " WHERE oid IN (to_regclass('kb_documents'),to_regclass('kb_file_index'))"
                        " AND relrowsecurity AND relforcerowsecurity",
                        err, sizeof(err));
   if (!st)
   {
      db2_lease_end();
      return -1;
   }
   int enabled = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      int count = aimee_pg_column_int(st, 0);
      enabled = count == 0 ? 0 : (count == 2 ? 1 : -1);
   }
   aimee_pg_finalize(st);
   db2_lease_end();
   return enabled;
}

static int apply_maintenance_context(db2_maintenance_worker_t worker, const char *project)
{
   const char *name = maintenance_worker_name(worker);
   if (!name || !project || !project[0])
      return DB2_ERR_MAINTENANCE_INVALID;
   void *conn = db2_conn();
   if (!conn)
      return DB2_ERR_TENANT_NO_CONN;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT set_maintenance_context(?1, ?2)", err, sizeof(err));
   if (!st)
      return DB2_ERR_TENANT_BEGIN;
   aimee_pg_bind_text(st, "?1", name);
   aimee_pg_bind_text(st, "?2", project);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ERR ? DB2_ERR_MAINTENANCE_INVALID : 0;
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
      /* No GUCs were set, but reset defensively in case a prior unit left session
       * state on this pooled connection, then release the lease fail-closed. */
      tenant_reset_gucs(conn);
      db2_lease_end();
      return DB2_ERR_TENANT_BEGIN;
   }

   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT set_tenant_context(?1, ?2)", err, sizeof(err));
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

int db2_maintenance_scope_begin(db2_maintenance_worker_t worker, const char *project)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!maintenance_worker_name(worker) || !project || !project[0])
      return DB2_ERR_MAINTENANCE_INVALID;

   db2_lease_begin();
   void *conn = db2_conn();
   if (!conn)
   {
      db2_lease_end();
      return DB2_ERR_TENANT_NO_CONN;
   }

   char err[256] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
   {
      tenant_reset_gucs(conn);
      db2_lease_end();
      return DB2_ERR_TENANT_BEGIN;
   }

   int applied = apply_maintenance_context(worker, project);
   if (applied != 0)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      tenant_reset_gucs(conn);
      db2_lease_end();
      return applied;
   }
   return 0;
}

int db2_maintenance_scope_begin_current(void)
{
   if (!db2_maintenance_job_active())
      return 0;
   int enabled = content_scope_enforced();
   if (enabled <= 0)
      return enabled;
   int rc = db2_maintenance_scope_begin(g_maintenance_job.worker, g_maintenance_job.project);
   return rc == 0 ? 1 : rc;
}

int db2_maintenance_context_apply_current(void)
{
   if (!db2_maintenance_job_active())
      return 0;
   int enabled = content_scope_enforced();
   if (enabled <= 0)
      return enabled;
   int rc = apply_maintenance_context(g_maintenance_job.worker, g_maintenance_job.project);
   return rc == 0 ? 1 : rc;
}

int db2_tenant_scope_commit(void)
{
   void *conn = db2_conn();
   int rc = 0;
   if (conn)
   {
      char err[256] = "";
      rc = aimee_pg_exec(conn, "COMMIT", err, sizeof(err));
      if (rc != 0)
         /* COMMIT failed (e.g. serialization/deadlock): force the transaction
          * out so the pooled connection never returns mid-transaction. */
         (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
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

int db2_maintenance_scope_commit(void)
{
   return db2_tenant_scope_commit();
}

void db2_maintenance_scope_rollback(void)
{
   db2_tenant_scope_rollback();
}
