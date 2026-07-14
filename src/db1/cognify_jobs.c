/* db1/cognify_jobs.c: per-machine cognification job queue. */

#include "cognify_jobs.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int db1_cognify_job_enqueue(int64_t memory_id)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "INSERT OR IGNORE INTO memory_cognify_jobs"
                            " (kind, memory_id, status, updated_at)"
                            " VALUES ('cognify_unit', ?, 'pending', datetime('now'))";

   if (!db || memory_id <= 0)
      return -1;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(stmt, 1, (sqlite3_int64)memory_id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_cognify_job_status(db1_cognify_job_stats_t *out)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT status, COUNT(*) FROM memory_cognify_jobs GROUP BY status";

   if (!db || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *status = (const char *)sqlite3_column_text(stmt, 0);
      int count = sqlite3_column_int(stmt, 1);
      if (!status)
         continue;
      if (strcmp(status, "pending") == 0)
         out->pending = count;
      else if (strcmp(status, "running") == 0)
         out->running = count;
      else if (strcmp(status, "done") == 0)
         out->done = count;
      else if (strcmp(status, "failed") == 0)
         out->failed = count;
      out->total += count;
   }

   sqlite3_finalize(stmt);
   return 0;
}

int db1_cognify_job_claim_next(db1_cognify_job_t *out)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *sel = NULL;
   sqlite3_stmt *upd = NULL;
   static const char *sel_sql =
       "SELECT id, kind, memory_id, status, attempts, max_attempts, claimed_by, claimed_at, "
       "last_error"
       " FROM memory_cognify_jobs"
       " WHERE status = 'pending'"
       " ORDER BY id ASC LIMIT 1";
   static const char *upd_sql =
       "UPDATE memory_cognify_jobs"
       " SET status='running', attempts=attempts+1, claimed_at=datetime('now'),"
       "     updated_at=datetime('now')"
       " WHERE id=?";

   if (!db || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   if (db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return -1;
   if (sqlite3_prepare_v2(db, sel_sql, -1, &sel, NULL) != SQLITE_OK)
      goto fail;

   if (sqlite3_step(sel) != SQLITE_ROW)
   {
      sqlite3_finalize(sel);
      db1_txn_end(db, "COMMIT");
      return 0;
   }

   out->id = (int64_t)sqlite3_column_int64(sel, 0);
   snprintf(out->kind, sizeof(out->kind), "%s",
            sqlite3_column_text(sel, 1) ? (const char *)sqlite3_column_text(sel, 1) : "");
   out->memory_id = (int64_t)sqlite3_column_int64(sel, 2);
   snprintf(out->status, sizeof(out->status), "%s",
            sqlite3_column_text(sel, 3) ? (const char *)sqlite3_column_text(sel, 3) : "");
   out->attempts = sqlite3_column_int(sel, 4) + 1;
   out->max_attempts = sqlite3_column_int(sel, 5);
   snprintf(out->claimed_by, sizeof(out->claimed_by), "%s",
            sqlite3_column_text(sel, 6) ? (const char *)sqlite3_column_text(sel, 6) : "");
   snprintf(out->claimed_at, sizeof(out->claimed_at), "%s",
            sqlite3_column_text(sel, 7) ? (const char *)sqlite3_column_text(sel, 7) : "");
   snprintf(out->last_error, sizeof(out->last_error), "%s",
            sqlite3_column_text(sel, 8) ? (const char *)sqlite3_column_text(sel, 8) : "");
   sqlite3_finalize(sel);

   if (sqlite3_prepare_v2(db, upd_sql, -1, &upd, NULL) != SQLITE_OK)
      goto fail;
   sqlite3_bind_int64(upd, 1, (sqlite3_int64)out->id);
   if (sqlite3_step(upd) != SQLITE_DONE)
      goto fail;
   sqlite3_finalize(upd);

   if (db1_txn_end(db, "COMMIT") != 0)
      return -1;
   snprintf(out->status, sizeof(out->status), "%s", "running");
   return 1;

fail:
   if (sel)
      sqlite3_finalize(sel);
   if (upd)
      sqlite3_finalize(upd);
   db1_txn_end(db, "ROLLBACK");
   return -1;
}

int db1_cognify_job_mark(int64_t job_id, const char *status, const char *error)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE memory_cognify_jobs"
                            " SET status=?, last_error=?, updated_at=datetime('now')"
                            " WHERE id=?";

   if (!db || job_id <= 0 || !status)
      return -1;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, error ? error : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 3, (sqlite3_int64)job_id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}
