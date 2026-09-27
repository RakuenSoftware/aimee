/* learning_synth_ops.c: the candidate-generation work queue (DB2). */

#include "learning_synth_ops.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

#define LSO_ERRBUF 256

int db2_synth_enqueue(const char *artifact_id)
{
   if (!artifact_id || !artifact_id[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "INSERT INTO learning_synth_ops (artifact_id) VALUES (?1) ON CONFLICT DO NOTHING";
   char err[LSO_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", artifact_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_synth_list_pending(char out[][37], int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT artifact_id FROM learning_synth_ops"
                            " WHERE status = 'pending' ORDER BY artifact_id LIMIT ?1";
   char err[LSO_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", max);

   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *aid = aimee_pg_column_text(st, 0);
      snprintf(out[count], 37, "%s", aid ? aid : "");
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_synth_mark_done(const char *artifact_id)
{
   if (!artifact_id || !artifact_id[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "UPDATE learning_synth_ops SET status = 'ok',"
                            " updated_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')"
                            " WHERE artifact_id = ?1";
   char err[LSO_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", artifact_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_synth_mark_failed(const char *artifact_id, const char *error)
{
   if (!artifact_id || !artifact_id[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "UPDATE learning_synth_ops SET status = 'failed',"
                            " attempts = attempts + 1, last_error = ?2,"
                            " updated_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')"
                            " WHERE artifact_id = ?1";
   char err[LSO_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", artifact_id);
   aimee_pg_bind_text(st, "?2", error ? error : "");
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_synth_reenqueue_all(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql =
       "UPDATE learning_synth_ops SET status = 'pending', attempts = 0, last_error = ''";
   char err[LSO_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   (void)aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return changes;
}

int db2_synth_ops_count(const char *status)
{
   if (status)
      return db2_scalar_int_text("SELECT COUNT(*) FROM learning_synth_ops WHERE status = ?1",
                                 status, -1);
   return db2_scalar_int("SELECT COUNT(*) FROM learning_synth_ops", -1);
}
