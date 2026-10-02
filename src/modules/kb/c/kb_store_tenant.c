/* kb_store/kb_store_tenant.c: per-request tenant context (I4). See kb_store_tenant.h. */

#include "kb_store_tenant.h"
#include "kb_store_internal.h"
#include "db_postgres.h"
#include "kb_store.h" /* kb_store_lease_begin/_end */
#include "kb_store_bounded_text.h"
#include "management_intent_fields.h"
#include "../support/kb_store_log.h"

#include <string.h>

typedef struct
{
   kb_store_maintenance_worker_t worker;
   char project[256];
} maintenance_job_t;

static _Thread_local maintenance_job_t g_maintenance_job;
static kb_store_identity_key_fn g_identity_key_provider;

_Static_assert((int)KB_PRIN_NONE == (int)KB_STORE_HOST_PRINCIPAL_NONE, "principal NONE ABI drift");
_Static_assert((int)KB_PRIN_OIDC == (int)KB_STORE_HOST_PRINCIPAL_OIDC, "principal OIDC ABI drift");
_Static_assert((int)KB_PRIN_CERT == (int)KB_STORE_HOST_PRINCIPAL_CERT, "principal CERT ABI drift");
_Static_assert((int)KB_PRIN_OWNER == (int)KB_STORE_HOST_PRINCIPAL_OWNER,
               "principal OWNER ABI drift");
_Static_assert((int)KB_PRIN_HOST == (int)KB_STORE_HOST_PRINCIPAL_HOST, "principal HOST ABI drift");

void aimee_kb_store_register_identity_key_provider(kb_store_identity_key_fn provider)
{
   g_identity_key_provider = provider;
}

int kb_store_tenant_identity_key(const kb_principal_t *principal, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!principal || !out || cap < 2 || cap > KB_STORE_INTENT_ACTOR_MAX ||
       principal->authenticated != 1 ||
       kb_store_bounded_len(principal->issuer, sizeof(principal->issuer)) ==
           sizeof(principal->issuer) ||
       kb_store_bounded_len(principal->subject, sizeof(principal->subject)) ==
           sizeof(principal->subject) ||
       !g_identity_key_provider)
      return -1;

   switch (principal->kind)
   {
   case KB_PRIN_OIDC:
   case KB_PRIN_CERT:
   case KB_PRIN_OWNER:
   case KB_PRIN_HOST:
      break;
   default:
      return -1;
   }

   if (g_identity_key_provider((int)principal->kind, principal->issuer, principal->subject,
                               principal->authenticated, out, cap) != 0)
      goto invalid;

   size_t n = kb_store_bounded_len(out, cap);
   if (!n || n == cap)
      goto invalid;
   char canonical[KB_STORE_INTENT_ACTOR_MAX + 1] = {0};
   memcpy(canonical, out, n + 1);
   if (!kb_store_intent_canonical_actor(canonical, sizeof(canonical)))
      goto invalid;
   return 0;

invalid:
   memset(out, 0, cap);
   return -1;
}

int kb_store_tenant_require_pg(void)
{
   /* RLS is a Postgres control; the SQLite test shim cannot enforce it, so a
    * tenant op on the shim is a hard, typed failure — never a silent read. */
   if (aimee_pg_is_shim())
      return KB_STORE_ERR_TENANT_REQUIRES_PG;
   if (!kb_store_conn())
      return KB_STORE_ERR_TENANT_REQUIRES_PG;
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

static const char *maintenance_worker_name(kb_store_maintenance_worker_t worker)
{
   switch (worker)
   {
   case KB_STORE_MAINTENANCE_INGEST:
      return "ingest";
   case KB_STORE_MAINTENANCE_REEMBED:
      return "reembed";
   case KB_STORE_MAINTENANCE_CURATOR:
      return "curator";
   case KB_STORE_MAINTENANCE_CODE_INDEXER:
      return "code-indexer";
   }
   return NULL;
}

int kb_store_maintenance_job_enter(kb_store_maintenance_worker_t worker, const char *project)
{
   if (g_maintenance_job.worker != 0 || !maintenance_worker_name(worker) || !project ||
       !project[0] || strlen(project) >= sizeof(g_maintenance_job.project))
      return KB_STORE_ERR_MAINTENANCE_INVALID;
   g_maintenance_job.worker = worker;
   memcpy(g_maintenance_job.project, project, strlen(project) + 1);
   return 0;
}

void kb_store_maintenance_job_leave(void)
{
   memset(&g_maintenance_job, 0, sizeof(g_maintenance_job));
}

int kb_store_maintenance_job_active(void)
{
   return g_maintenance_job.worker != 0;
}

static int content_scope_enforced(void)
{
   kb_store_lease_begin();
   void *conn = kb_store_conn();
   if (!conn || aimee_pg_is_shim())
   {
      kb_store_lease_end();
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
      kb_store_lease_end();
      return -1;
   }
   int enabled = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      int count = aimee_pg_column_int(st, 0);
      enabled = count == 0 ? 0 : (count == 2 ? 1 : -1);
   }
   aimee_pg_finalize(st);
   kb_store_lease_end();
   return enabled;
}

static int apply_maintenance_context(kb_store_maintenance_worker_t worker, const char *project)
{
   const char *name = maintenance_worker_name(worker);
   if (!name || !project || !project[0])
      return KB_STORE_ERR_MAINTENANCE_INVALID;
   void *conn = kb_store_conn();
   if (!conn)
      return KB_STORE_ERR_TENANT_NO_CONN;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT set_maintenance_context(?1, ?2)", err, sizeof(err));
   if (!st)
   {
      LOG_WARN("kb_store.tenant", "maintenance context prepare failed for worker '%s': %s", name,
               err[0] ? err : "unknown database error");
      return KB_STORE_ERR_TENANT_BEGIN;
   }
   aimee_pg_bind_text(st, "?1", name);
   aimee_pg_bind_text(st, "?2", project);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
   {
      LOG_WARN("kb_store.tenant", "maintenance context rejected for worker '%s' project '%s': %s",
               name, project, err[0] ? err : "unexpected database result");
      return KB_STORE_ERR_MAINTENANCE_INVALID;
   }
   return 0;
}

int kb_store_tenant_scope_begin(const kb_principal_t *p, int64_t team)
{
   int g = kb_store_tenant_require_pg();
   if (g)
      return g;
   if (!p || !p->authenticated)
      return KB_STORE_ERR_TENANT_UNAUTHENTICATED;

   char key[576];
   if (kb_store_tenant_identity_key(p, key, sizeof(key)) != 0)
      return KB_STORE_ERR_TENANT_UNAUTHENTICATED;

   kb_store_lease_begin(); /* eager lease so the whole unit rides one connection */
   void *conn = kb_store_conn();
   if (!conn)
   {
      kb_store_lease_end();
      return KB_STORE_ERR_TENANT_NO_CONN;
   }

   char err[256] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
   {
      /* No GUCs were set, but reset defensively in case a prior unit left session
       * state on this pooled connection, then release the lease fail-closed. */
      tenant_reset_gucs(conn);
      kb_store_lease_end();
      return KB_STORE_ERR_TENANT_BEGIN;
   }

   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT set_tenant_context(?1, ?2)", err, sizeof(err));
   if (!st)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      tenant_reset_gucs(conn);
      kb_store_lease_end();
      return KB_STORE_ERR_TENANT_BEGIN;
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
      kb_store_lease_end();
      return KB_STORE_ERR_TENANT_DENIED;
   }
   return 0; /* transaction open, context set, connection held by the lease */
}

int kb_store_maintenance_scope_begin(kb_store_maintenance_worker_t worker, const char *project)
{
   int g = kb_store_tenant_require_pg();
   if (g)
      return g;
   if (!maintenance_worker_name(worker) || !project || !project[0])
      return KB_STORE_ERR_MAINTENANCE_INVALID;

   kb_store_lease_begin();
   void *conn = kb_store_conn();
   if (!conn)
   {
      kb_store_lease_end();
      return KB_STORE_ERR_TENANT_NO_CONN;
   }

   char err[256] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
   {
      tenant_reset_gucs(conn);
      kb_store_lease_end();
      return KB_STORE_ERR_TENANT_BEGIN;
   }

   int applied = apply_maintenance_context(worker, project);
   if (applied != 0)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      tenant_reset_gucs(conn);
      kb_store_lease_end();
      return applied;
   }
   return 0;
}

int kb_store_maintenance_scope_begin_current(void)
{
   if (!kb_store_maintenance_job_active())
      return 0;
   int enabled = content_scope_enforced();
   if (enabled <= 0)
      return enabled;
   int rc = kb_store_maintenance_scope_begin(g_maintenance_job.worker, g_maintenance_job.project);
   return rc == 0 ? 1 : rc;
}

int kb_store_maintenance_context_apply_current(void)
{
   if (!kb_store_maintenance_job_active())
      return 0;
   int enabled = content_scope_enforced();
   if (enabled <= 0)
      return enabled;
   int rc = apply_maintenance_context(g_maintenance_job.worker, g_maintenance_job.project);
   return rc == 0 ? 1 : rc;
}

int kb_store_tenant_scope_commit(void)
{
   void *conn = kb_store_conn();
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
   kb_store_lease_end();
   return rc == 0 ? 0 : -1;
}

void kb_store_tenant_scope_rollback(void)
{
   void *conn = kb_store_conn();
   if (conn)
   {
      char err[256] = "";
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      tenant_reset_gucs(conn);
   }
   kb_store_lease_end();
}

int kb_store_maintenance_scope_commit(void)
{
   return kb_store_tenant_scope_commit();
}

void kb_store_maintenance_scope_rollback(void)
{
   kb_store_tenant_scope_rollback();
}
