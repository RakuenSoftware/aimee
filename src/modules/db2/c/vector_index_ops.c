/* vector_index_ops.c: DB2-side bookkeeping for pgvector write status.
 *
 * Postgres via libpq. The pgvector transport lives in src/modules/db2/c/pgvec_*;
 * this file only persists shared retry/status metadata for those writes. */

#include "vector_index_ops.h"

#include "aimee.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "kb_service_backend.h" /* db2_kb_service_get_active_embedder_version */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

int db2_vector_index_sync_suppressed(void)
{
   return db2_is_ephemeral();
}

void db2_vector_index_op_record(int64_t point_id, const char *collection, int64_t memory_id, int ok,
                                const char *error_msg)
{
   void *conn = db2_conn();
   if (!conn)
      return;

   /* embedding_version records which embedder produced this vector, read from
    * memory_active_embedder rather than passed in: the vector was produced by
    * whichever embedder was active when it was produced, and no caller of this
    * function knows that better than the table does.
    *
    * It is stamped only on the success path. A failed attempt produced no
    * vector, so claiming one exists at the current version would be the same
    * lie this column was added to stop -- see the note on the column in
    * schema.sql. On the conflict path a successful re-index restamps, because a
    * point re-embedded at a new version is at the new version. */
   static const char *sql =
       "INSERT INTO vector_index_ops"
       "  (point_id, collection, memory_id, status, attempts, last_error, indexed_at,"
       "   embedding_version, updated_at)"
       " VALUES (?1, ?2, ?3, ?4, 1, ?5, ?6, ?7, pg_now_text())"
       " ON CONFLICT(point_id) DO UPDATE SET"
       "  collection = excluded.collection,"
       "  status     = excluded.status,"
       "  attempts   = vector_index_ops.attempts + 1,"
       "  last_error = excluded.last_error,"
       "  indexed_at = excluded.indexed_at,"
       "  embedding_version = CASE WHEN excluded.status = 'ok'"
       "                           THEN excluded.embedding_version"
       "                           ELSE vector_index_ops.embedding_version END,"
       "  updated_at = pg_now_text()";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;

   char ts[32];
   now_utc(ts, sizeof(ts));
   aimee_pg_bind_int64(st, "?1", point_id);
   aimee_pg_bind_text(st, "?2", collection ? collection : "");
   if (memory_id > 0)
      aimee_pg_bind_int64(st, "?3", memory_id);
   else
      aimee_pg_bind_null(st, "?3");
   aimee_pg_bind_text(st, "?4", ok ? "ok" : "failed");
   aimee_pg_bind_text(st, "?5", (error_msg && !ok) ? error_msg : "");
   char active_version[128] = "";
   if (ok)
   {
      aimee_pg_bind_text(st, "?6", ts);
      (void)db2_kb_service_get_active_embedder_version(active_version, sizeof(active_version));
   }
   else
      aimee_pg_bind_null(st, "?6");
   aimee_pg_bind_text(st, "?7", active_version);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_vector_index_op_remove(int64_t point_id)
{
   void *conn = db2_conn();
   if (!conn)
      return;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "DELETE FROM vector_index_ops WHERE point_id = ?1", err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", point_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_vector_index_ops_reset_stuck(int max_attempts)
{
   if (max_attempts <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE vector_index_ops SET attempts = 0"
                                          " WHERE status = 'failed' AND attempts >= ?1",
                                          err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max_attempts);
   (void)aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return changes;
}

int db2_vector_index_ops_summary(int max_attempts, db2_vector_index_ops_summary_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));

   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT"
                            " SUM(CASE WHEN status='pending' THEN 1 ELSE 0 END),"
                            " SUM(CASE WHEN status='failed'  THEN 1 ELSE 0 END),"
                            " SUM(CASE WHEN status='ok'      THEN 1 ELSE 0 END),"
                            " SUM(CASE WHEN status='failed' AND attempts >= ?1 THEN 1 ELSE 0 END)"
                            " FROM vector_index_ops";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", max_attempts > 0 ? max_attempts : 1);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->pending_ops = aimee_pg_column_int64(st, 0);
      out->failed_ops = aimee_pg_column_int64(st, 1);
      out->ok_ops = aimee_pg_column_int64(st, 2);
      out->stuck_ops = aimee_pg_column_int64(st, 3);
   }
   aimee_pg_finalize(st);
   return 0;
}

int db2_vector_index_ops_list_failed(db2_vector_index_op_failed_t *rows, int max_rows)
{
   if (!rows || max_rows <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "SELECT point_id, collection, memory_id, attempts, last_error, updated_at"
       " FROM vector_index_ops WHERE status = 'failed'"
       " ORDER BY attempts DESC, updated_at DESC LIMIT ?1";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", max_rows);

   int count = 0;
   while (count < max_rows && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_vector_index_op_failed_t *row = &rows[count++];
      memset(row, 0, sizeof(*row));
      row->point_id = aimee_pg_column_int64(st, 0);
      const char *collection = aimee_pg_column_text(st, 1);
      if (collection)
         snprintf(row->collection, sizeof(row->collection), "%s", collection);
      row->memory_id = aimee_pg_column_int64(st, 2);
      row->attempts = aimee_pg_column_int(st, 3);
      const char *last_error = aimee_pg_column_text(st, 4);
      if (last_error)
         snprintf(row->last_error, sizeof(row->last_error), "%s", last_error);
      const char *updated_at = aimee_pg_column_text(st, 5);
      if (updated_at)
         snprintf(row->updated_at, sizeof(row->updated_at), "%s", updated_at);
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_vector_index_ops_list_retryable_memory_ids(int max_attempts, int limit, int64_t *out,
                                                   int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char sql[256];
   snprintf(sql, sizeof(sql),
            "SELECT DISTINCT memory_id FROM vector_index_ops"
            " WHERE status = 'failed' AND memory_id IS NOT NULL"
            "   AND attempts < ?1"
            " ORDER BY updated_at ASC%s",
            (limit > 0) ? " LIMIT ?2" : "");

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max_attempts);
   if (limit > 0)
      aimee_pg_bind_int(st, "?2", limit);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      out[n++] = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}
