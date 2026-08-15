/* db1/pipelines.c: durable per-machine autopilot pipeline state. */

#include "pipelines.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int db1_pipeline_create(const char *task, const char *request_classification,
                        const char *plan_depth, int *out_id)
{
   if (!task || !task[0])
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO pipelines (task, status, current_phase, request_classification, plan_depth,"
       " phase_attempts, plan_id, job_id, clarify_session_id, created_at, updated_at)"
       " VALUES (?, 'active', 'classify', ?, ?, 0, 0, 0, 0, datetime('now'), datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, task, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, request_classification ? request_classification : "", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, plan_depth ? plan_depth : "", -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_DONE && out_id)
      *out_id = (int)sqlite3_last_insert_rowid(db);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_pipeline_get(int pipeline_id, db1_pipeline_t *out)
{
   if (!out || pipeline_id <= 0)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT id, task, status, current_phase, request_classification, plan_depth,"
       " phase_attempts, plan_id, job_id, clarify_session_id, created_at, updated_at"
       " FROM pipelines WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, pipeline_id);
   memset(out, 0, sizeof(*out));
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      out->id = sqlite3_column_int(stmt, 0);
      db1_copy_col_text(out->task, sizeof(out->task), stmt, 1);
      db1_copy_col_text(out->status, sizeof(out->status), stmt, 2);
      db1_copy_col_text(out->current_phase, sizeof(out->current_phase), stmt, 3);
      db1_copy_col_text(out->request_classification, sizeof(out->request_classification), stmt, 4);
      db1_copy_col_text(out->plan_depth, sizeof(out->plan_depth), stmt, 5);
      out->phase_attempts = sqlite3_column_int(stmt, 6);
      out->plan_id = sqlite3_column_int(stmt, 7);
      out->job_id = sqlite3_column_int(stmt, 8);
      out->clarify_session_id = sqlite3_column_int(stmt, 9);
      db1_copy_col_text(out->created_at, sizeof(out->created_at), stmt, 10);
      db1_copy_col_text(out->updated_at, sizeof(out->updated_at), stmt, 11);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_pipeline_update(int pipeline_id, const char *status, const char *current_phase,
                        int phase_attempts, int plan_id, int job_id,
                        const char *request_classification, const char *plan_depth,
                        int clarify_session_id)
{
   if (pipeline_id <= 0 || !status || !current_phase)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE pipelines SET status = ?, current_phase = ?, request_classification = ?,"
       " plan_depth = ?, phase_attempts = ?, plan_id = ?, job_id = ?, clarify_session_id = ?,"
       " updated_at = datetime('now') WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, current_phase, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, request_classification ? request_classification : "", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, plan_depth ? plan_depth : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 5, phase_attempts);
   sqlite3_bind_int(stmt, 6, plan_id);
   sqlite3_bind_int(stmt, 7, job_id);
   sqlite3_bind_int(stmt, 8, clarify_session_id);
   sqlite3_bind_int(stmt, 9, pipeline_id);

   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed > 0 ? 0 : -1;
}

int db1_pipeline_link_plan(int pipeline_id, int plan_id)
{
   if (pipeline_id <= 0 || plan_id <= 0)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE pipelines SET plan_id = ?, updated_at = datetime('now') WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, plan_id);
   sqlite3_bind_int(stmt, 2, pipeline_id);

   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed > 0 ? 0 : -1;
}

int db1_pipeline_link_job(int pipeline_id, int job_id)
{
   if (pipeline_id <= 0 || job_id <= 0)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE pipelines SET job_id = ?, updated_at = datetime('now') WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, job_id);
   sqlite3_bind_int(stmt, 2, pipeline_id);

   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed > 0 ? 0 : -1;
}

int db1_pipeline_cancel(int pipeline_id)
{
   if (pipeline_id <= 0)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE pipelines SET status = 'cancelled', updated_at = datetime('now') WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, pipeline_id);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed > 0 ? 0 : -1;
}

int db1_pipeline_list_active(db1_pipeline_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT id, task, status, current_phase, request_classification, plan_depth,"
       " phase_attempts, plan_id, job_id, clarify_session_id, created_at, updated_at"
       " FROM pipelines WHERE status IN ('active', 'paused')"
       " ORDER BY updated_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_pipeline_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->id = sqlite3_column_int(stmt, 0);
      db1_copy_col_text(row->task, sizeof(row->task), stmt, 1);
      db1_copy_col_text(row->status, sizeof(row->status), stmt, 2);
      db1_copy_col_text(row->current_phase, sizeof(row->current_phase), stmt, 3);
      db1_copy_col_text(row->request_classification, sizeof(row->request_classification), stmt, 4);
      db1_copy_col_text(row->plan_depth, sizeof(row->plan_depth), stmt, 5);
      row->phase_attempts = sqlite3_column_int(stmt, 6);
      row->plan_id = sqlite3_column_int(stmt, 7);
      row->job_id = sqlite3_column_int(stmt, 8);
      row->clarify_session_id = sqlite3_column_int(stmt, 9);
      db1_copy_col_text(row->created_at, sizeof(row->created_at), stmt, 10);
      db1_copy_col_text(row->updated_at, sizeof(row->updated_at), stmt, 11);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}
