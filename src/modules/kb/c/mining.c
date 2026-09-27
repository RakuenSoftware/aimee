/* db2/mining.c: DB2 substrate for aimee-kb continuous mining. */

#include "mining.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

typedef struct
{
   const char *id;
   int interval_s;
} mining_job_default_t;

static const mining_job_default_t JOB_DEFAULTS[] = {
    {"pattern_cluster", 900},
    {"recurrence", 1800},
    {NULL, 0},
};

static int exec_job_default(const char *id, int interval_s)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "INSERT INTO mining_jobs (id, hwm, interval_s, enabled)"
                                          " VALUES (?1, 0, ?2, TRUE)"
                                          " ON CONFLICT (id) DO NOTHING",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", id);
   aimee_pg_bind_int(st, "?2", interval_s);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_mining_seed_job_defaults(void)
{
   for (int i = 0; JOB_DEFAULTS[i].id; i++)
   {
      if (exec_job_default(JOB_DEFAULTS[i].id, JOB_DEFAULTS[i].interval_s) != 0)
         return -1;
   }
   return 0;
}

int db2_mining_job_get(const char *id, db2_mining_job_row_t *out)
{
   if (!id || !out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, COALESCE(last_run_at, ''), hwm, interval_s,"
                        " CASE WHEN enabled THEN 1 ELSE 0 END, COALESCE(last_error, '')"
                        " FROM mining_jobs WHERE id = ?1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", id);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return -1;
   }

   memset(out, 0, sizeof(*out));
   const char *v = aimee_pg_column_text(st, 0);
   snprintf(out->id, sizeof(out->id), "%s", v ? v : "");
   v = aimee_pg_column_text(st, 1);
   snprintf(out->last_run_at, sizeof(out->last_run_at), "%s", v ? v : "");
   out->hwm = aimee_pg_column_int64(st, 2);
   out->interval_s = aimee_pg_column_int(st, 3);
   out->enabled = aimee_pg_column_int(st, 4) ? 1 : 0;
   v = aimee_pg_column_text(st, 5);
   snprintf(out->last_error, sizeof(out->last_error), "%s", v ? v : "");
   aimee_pg_finalize(st);
   return 0;
}

int db2_mining_job_complete(const char *id, int64_t hwm, const char *error)
{
   if (!id)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE mining_jobs"
                                          " SET hwm = CASE WHEN hwm > ?2 THEN hwm ELSE ?2 END,"
                                          "     last_run_at = pg_now_text(),"
                                          "     last_error = ?3"
                                          " WHERE id = ?1",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", id);
   aimee_pg_bind_int64(st, "?2", hwm);
   aimee_pg_bind_text(st, "?3", error ? error : "");
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}

int db2_mining_job_try_lock(const char *id)
{
   if (!id || !id[0])
      return 0;
   if (aimee_pg_is_shim())
      return 1;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[256] = "";
   /* libpq exposes booleans as "t"/"f", while aimee_pg_column_int() is a
    * numeric parser. Normalize in SQL so an acquired Postgres advisory lock is
    * not misread as false. The SQLite shim bypasses locks, so only a live
    * Postgres run exposes this failure mode. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT CASE WHEN pg_try_advisory_lock(hashtext(?1)) THEN 1 ELSE 0 END", err,
       sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", id);
   int locked = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      locked = aimee_pg_column_int(st, 0) ? 1 : 0;
   aimee_pg_finalize(st);
   return locked;
}

void db2_mining_job_unlock(const char *id)
{
   if (!id || !id[0] || aimee_pg_is_shim())
      return;
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT pg_advisory_unlock(hashtext(?1))", err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_mining_event_upsert(const db2_mining_event_t *event)
{
   if (!event || event->source_event_id <= 0 || !event->event_type[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO interaction_event_embeddings"
                        " (source_event_id, session_id, event_type, role, failure_mode,"
                        "  scope_kind,scope_id,task_family,action_sequence,error_signature,"
                        "  environment,preconditions,outcome,recovery_action,"
                        "  payload_json, embedding, cluster_key, created_at)"
                        " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,"
                        "         ?15,?16,?17,pg_now_text())"
                        " ON CONFLICT (source_event_id) DO UPDATE SET"
                        "   session_id = EXCLUDED.session_id,"
                        "   event_type = EXCLUDED.event_type,"
                        "   role = EXCLUDED.role,"
                        "   failure_mode = EXCLUDED.failure_mode,"
                        "   scope_kind = EXCLUDED.scope_kind,"
                        "   scope_id = EXCLUDED.scope_id,"
                        "   task_family = EXCLUDED.task_family,"
                        "   action_sequence = EXCLUDED.action_sequence,"
                        "   error_signature = EXCLUDED.error_signature,"
                        "   environment = EXCLUDED.environment,"
                        "   preconditions = EXCLUDED.preconditions,"
                        "   outcome = EXCLUDED.outcome,"
                        "   recovery_action = EXCLUDED.recovery_action,"
                        "   payload_json = EXCLUDED.payload_json,"
                        "   embedding = EXCLUDED.embedding,"
                        "   cluster_key = EXCLUDED.cluster_key",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", event->source_event_id);
   aimee_pg_bind_text(st, "?2", event->session_id);
   aimee_pg_bind_text(st, "?3", event->event_type);
   aimee_pg_bind_text(st, "?4", event->role);
   aimee_pg_bind_text(st, "?5", event->failure_mode);
   aimee_pg_bind_text(st, "?6", event->scope_kind[0] ? event->scope_kind : "workspace");
   aimee_pg_bind_text(st, "?7", event->scope_id);
   aimee_pg_bind_text(st, "?8", event->task_family);
   aimee_pg_bind_text(st, "?9", event->action_sequence);
   aimee_pg_bind_text(st, "?10", event->error_signature);
   aimee_pg_bind_text(st, "?11", event->environment);
   aimee_pg_bind_text(st, "?12", event->preconditions);
   aimee_pg_bind_text(st, "?13", event->outcome);
   aimee_pg_bind_text(st, "?14", event->recovery_action);
   aimee_pg_bind_text(st, "?15", event->payload_json[0] ? event->payload_json : "{}");
   aimee_pg_bind_text(st, "?16", event->embedding[0] ? event->embedding : "[]");
   aimee_pg_bind_text(st, "?17", event->cluster_key);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? 0 : -1;
}
