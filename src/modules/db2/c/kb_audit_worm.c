/* kb_audit_worm.c: aimee-kb's durable WORM producer seam.
 *
 * The KB transaction owns only an immutable PostgreSQL outbox intent. The
 * separately credentialed aimee-kb-worm process claims committed intents and
 * appends them through modules/audit/audit_worm.c, the same SQLite chain/store
 * implementation used by aimee-server. No PostgreSQL chain builder lives here. */
#include <stdio.h>
#include <string.h>

#include <aimee/audit/audit_worm_chain.h>
#include "../support/db2_runtime_config.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "../support/db2_time.h" /* now_utc: portable enqueued_at for the shim path */
#include "kb_audit_worm.h"

/* Retained as a no-op ABI during the module transition. Canonical hashing is
 * now owned directly by the shared SQLite WORM worker, not injected into DB2. */
void aimee_db2_register_audit_hash_provider(db2_audit_hash_fn provider)
{
   (void)provider;
}

/* Capture gate (S6). Resolved once from config.audit_worm_enabled (default-off)
 * and cached, so the hot kb-audit seam costs one branch after the first call. */
static int g_kb_worm_enabled = -1;
void db2_kb_audit_worm_set_enabled(int enabled)
{
   g_kb_worm_enabled = enabled ? 1 : 0;
}
int db2_kb_audit_worm_enabled(void)
{
   if (g_kb_worm_enabled < 0)
      g_kb_worm_enabled = config_audit_worm_enabled() ? 1 : 0;
   return g_kb_worm_enabled;
}

int db2_kb_audit_append_in_txn(void *conn, const char *actor_role, const char *actor_principal,
                               const char *action, const char *subject, const char *verdict,
                               const char *detail)
{
   if (!conn || !aimee_pg_in_transaction(conn) || !action || !action[0])
      return -1;
   actor_role = actor_role ? actor_role : "";
   actor_principal = actor_principal ? actor_principal : "";
   subject = subject ? subject : "";
   verdict = verdict ? verdict : "";
   detail = detail ? detail : "";

   char detail_capped[AUDIT_WORM_DETAIL_MAX + 96];
   size_t dlen = strlen(detail);
   if (dlen > AUDIT_WORM_DETAIL_MAX)
   {
      snprintf(detail_capped, sizeof detail_capped, "%.*s\"[worm-truncated %zu bytes]\"",
               AUDIT_WORM_DETAIL_MAX, detail, dlen - (size_t)AUDIT_WORM_DETAIL_MAX);
      detail = detail_capped;
   }

   char err[256] = "";
#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
   aimee_pg_stmt_t *submit =
       aimee_pg_prepare(conn, "SELECT kb_audit_worm_submit(?1,?2,?3,?4,?5,?6)", err, sizeof(err));
#else
   /* The DB2 SQLite shim cannot execute PL/pgSQL SECURITY DEFINER functions;
    * mirror the producer contract by inserting the intent directly.
    *
    * The timestamp is computed in C and bound rather than written as a SQL
    * function call. This branch is compiled for every unit-test binary, and the
    * real-Postgres shard runs those same binaries against a live server, so any
    * SQLite-only spelling here (datetime('now') is not a Postgres function)
    * fails every governed mutation on that shard. now_utc() emits the same
    * YYYY-MM-DDTHH:MM:SSZ shape as the schema's pg_now_text() default. */
   char enqueued_at[32];
   now_utc(enqueued_at, sizeof(enqueued_at));
   aimee_pg_stmt_t *submit = aimee_pg_prepare(
       conn,
       "INSERT INTO kb_audit_outbox(enqueued_at,actor_role,actor_principal,action,subject,"
       "verdict,detail) VALUES(?7,?1,?2,?3,?4,?5,?6) RETURNING outbox_id",
       err, sizeof(err));
#endif
   if (!submit)
      return -1;
   aimee_pg_bind_text(submit, "?1", actor_role);
   aimee_pg_bind_text(submit, "?2", actor_principal);
   aimee_pg_bind_text(submit, "?3", action);
   aimee_pg_bind_text(submit, "?4", subject);
   aimee_pg_bind_text(submit, "?5", verdict);
   aimee_pg_bind_text(submit, "?6", detail);
#ifndef AIMEE_DISABLE_DB2_SQLITE_SHIM
   aimee_pg_bind_text(submit, "?7", enqueued_at);
#endif
   aimee_pg_step_t submitted = aimee_pg_step(submit, err, sizeof(err));
   aimee_pg_finalize(submit);
   return submitted == AIMEE_PG_ROW ? 0 : -1;
}

int db2_kb_audit_append(const char *actor_role, const char *actor_principal, const char *action,
                        const char *subject, const char *verdict, const char *detail)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256];
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;
   if (db2_kb_audit_append_in_txn(conn, actor_role, actor_principal, action, subject, verdict,
                                  detail) != 0 ||
       aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   return 0;
}

int db2_kb_audit_pending(long long *count, long long *oldest_age_seconds)
{
   if (count)
      *count = 0;
   if (oldest_age_seconds)
      *oldest_age_seconds = 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[128];
#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
   const char *sql = "SELECT pending_count,oldest_age_seconds FROM kb_audit_worm_pending()";
#else
   const char *sql = "SELECT COUNT(*),0 FROM kb_audit_outbox o LEFT JOIN kb_audit_delivery d"
                     " ON d.outbox_id=o.outbox_id WHERE d.outbox_id IS NULL";
#endif
   aimee_pg_stmt_t *q = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!q)
      return -1;
   int rc = -1;
   if (aimee_pg_step(q, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      if (count)
         *count = aimee_pg_column_int64(q, 0);
      if (oldest_age_seconds)
         *oldest_age_seconds = aimee_pg_column_int64(q, 1);
      rc = 0;
   }
   aimee_pg_finalize(q);
   return rc;
}
