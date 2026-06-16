/* code_index_ops.c: DB2-side replay bookkeeping for code-chunk pgvector writes.
 * Mirrors vector_index_ops; Postgres via libpq (sqlite under the test shim). */

#include "code_index_ops.h"

#include "aimee.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

void db2_code_index_op_record(int64_t point_id, const char *project, const char *node_key,
                              const char *file_path, int ok, const char *error_msg)
{
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql =
       "INSERT INTO code_index_ops"
       "  (point_id, project, node_key, file_path, status, attempts, last_error, indexed_at,"
       "   updated_at)"
       " VALUES (?1, ?2, ?3, ?4, ?5, 1, ?6, ?7, pg_now_text())"
       " ON CONFLICT(point_id) DO UPDATE SET"
       "  project    = excluded.project,"
       "  node_key   = excluded.node_key,"
       "  file_path  = excluded.file_path,"
       "  status     = excluded.status,"
       "  attempts   = code_index_ops.attempts + 1,"
       "  last_error = excluded.last_error,"
       "  indexed_at = excluded.indexed_at,"
       "  updated_at = pg_now_text()";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;

   char ts[32];
   now_utc(ts, sizeof(ts));
   aimee_pg_bind_int64(st, "?1", point_id);
   aimee_pg_bind_text(st, "?2", project ? project : "");
   aimee_pg_bind_text(st, "?3", node_key ? node_key : "");
   aimee_pg_bind_text(st, "?4", file_path ? file_path : "");
   aimee_pg_bind_text(st, "?5", ok ? "ok" : "failed");
   aimee_pg_bind_text(st, "?6", (error_msg && !ok) ? error_msg : "");
   if (ok)
      aimee_pg_bind_text(st, "?7", ts);
   else
      aimee_pg_bind_null(st, "?7");
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_code_index_ops_reset_stuck(int max_attempts)
{
   if (max_attempts <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE code_index_ops SET attempts = 0"
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

int db2_code_index_ops_summary(int max_attempts, db2_code_index_ops_summary_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT"
                        "  SUM(CASE WHEN status = 'ok' THEN 1 ELSE 0 END),"
                        "  SUM(CASE WHEN status = 'pending' THEN 1 ELSE 0 END),"
                        "  SUM(CASE WHEN status = 'failed' THEN 1 ELSE 0 END),"
                        "  SUM(CASE WHEN status = 'failed' AND attempts >= ?1 THEN 1 ELSE 0 END)"
                        " FROM code_index_ops",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", max_attempts > 0 ? max_attempts : 8);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->ok_ops = aimee_pg_column_int64(st, 0);
      out->pending_ops = aimee_pg_column_int64(st, 1);
      out->failed_ops = aimee_pg_column_int64(st, 2);
      out->stuck_ops = aimee_pg_column_int64(st, 3);
   }
   aimee_pg_finalize(st);
   return 0;
}

int64_t db2_code_index_drift_candidates(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* auditable-correctness D7 (staleness heuristic): a code embedding is a
    * re-ingest candidate when its source file was re-scanned AFTER the embedding
    * was written — files.scanned_at > code_embeddings.updated_at — i.e. the file
    * changed but its vectors haven't been refreshed yet. Joined within DB2 on
    * project name + file path. Read-only; ranks/sizes re-ingest, not a verdict.
    *
    * Timestamp NORMALIZATION: files.scanned_at is now_utc() ("YYYY-MM-DDTHH:MM:SSZ")
    * while code_embeddings.updated_at is to_char(...,'YYYY-MM-DD HH24:MI:SS') — a
    * raw string compare would mis-order ('T' > ' '). Strip the 'T'/'Z' from
    * scanned_at so both are "YYYY-MM-DD HH:MM:SS" and compare lexically (valid for
    * zero-padded UTC). replace() is portable across Postgres and the sqlite shim. */
   static const char *sql =
       "SELECT COUNT(*) FROM code_embeddings ce"
       " JOIN projects p ON p.name = ce.project"
       " JOIN files f ON f.project_id = p.id AND f.path = ce.file_path"
       " WHERE ce.file_path <> ''"
       "   AND replace(replace(f.scanned_at, 'T', ' '), 'Z', '') > ce.updated_at";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   int64_t n = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}
