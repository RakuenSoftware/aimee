/* db1/coord_jobs.c: durable per-machine coordinated job queue. */

#include "coord_jobs.h"
#include "db1_internal.h"

#include "cJSON.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static int files_overlap(const char *a_json, const char *b_json)
{
   if (!a_json || !b_json || !a_json[0] || !b_json[0])
      return 0;

   cJSON *a = cJSON_Parse(a_json);
   cJSON *b = cJSON_Parse(b_json);
   if (!a || !b)
   {
      cJSON_Delete(a);
      cJSON_Delete(b);
      return 0;
   }

   int overlap = 0;
   cJSON *ai;
   cJSON_ArrayForEach(ai, a)
   {
      if (!cJSON_IsString(ai))
         continue;
      cJSON *bi;
      cJSON_ArrayForEach(bi, b)
      {
         if (cJSON_IsString(bi) && strcmp(ai->valuestring, bi->valuestring) == 0)
         {
            overlap = 1;
            goto done;
         }
      }
   }

done:
   cJSON_Delete(a);
   cJSON_Delete(b);
   return overlap;
}

static void fill_task(db1_coord_task_t *out, sqlite3_stmt *stmt)
{
   memset(out, 0, sizeof(*out));
   out->id = sqlite3_column_int(stmt, 0);
   out->job_id = sqlite3_column_int(stmt, 1);
   out->step_id = sqlite3_column_int(stmt, 2);
   db1_copy_col_text(out->status, sizeof(out->status), stmt, 3);
   db1_copy_col_text(out->claimed_by, sizeof(out->claimed_by), stmt, 4);
   db1_copy_col_text(out->claimed_at, sizeof(out->claimed_at), stmt, 5);
   db1_copy_col_text(out->files, sizeof(out->files), stmt, 6);
   db1_copy_col_text(out->result, sizeof(out->result), stmt, 7);
   db1_copy_col_text(out->error, sizeof(out->error), stmt, 8);
   out->preempt_requeues = sqlite3_column_int(stmt, 9);
   db1_copy_col_text(out->created_at, sizeof(out->created_at), stmt, 10);
}

int db1_coord_job_create(int plan_id, int max_concurrent)
{
   if (plan_id <= 0)
      return -1;
   if (max_concurrent <= 0)
      max_concurrent = DB1_COORD_DEFAULT_PAR;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "INSERT INTO coord_jobs (plan_id, status, max_concurrent)"
                            " VALUES (?, 'pending', ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, plan_id);
   sqlite3_bind_int(stmt, 2, max_concurrent);
   int rc = sqlite3_step(stmt);
   int job_id = (rc == SQLITE_DONE) ? (int)sqlite3_last_insert_rowid(db) : -1;
   sqlite3_finalize(stmt);
   return job_id;
}

int db1_coord_job_add_task(int job_id, int step_id, const char *files_json, const char *role,
                           const char *prompt, const char *cwd)
{
   if (job_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO coord_job_tasks (job_id, step_id, files, role, prompt, cwd)"
       " VALUES (?, ?, ?, ?, ?, ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, job_id);
   if (step_id > 0)
      sqlite3_bind_int(stmt, 2, step_id);
   else
      sqlite3_bind_null(stmt, 2);
   sqlite3_bind_text(stmt, 3, files_json ? files_json : "[]", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, (role && role[0]) ? role : "execute", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, prompt ? prompt : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, cwd ? cwd : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   int task_id = (rc == SQLITE_DONE) ? (int)sqlite3_last_insert_rowid(db) : -1;
   sqlite3_finalize(stmt);
   return task_id;
}

int db1_coord_job_claim_next(int job_id, const char *delegate_name, db1_coord_task_t *out)
{
   if (job_id <= 0 || !delegate_name || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   if (db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *running_sql = "SELECT files FROM coord_job_tasks"
                                    " WHERE job_id = ? AND status IN ('claimed', 'running')";
   if (sqlite3_prepare_v2(db, running_sql, -1, &stmt, NULL) != SQLITE_OK)
   {
      (void)db1_txn_end(db, "ROLLBACK");
      return -1;
   }
   sqlite3_bind_int(stmt, 1, job_id);

   char running_files[DB1_COORD_MAX_TASKS][DB1_COORD_FILES_LEN];
   int running_count = 0;
   while (running_count < DB1_COORD_MAX_TASKS && sqlite3_step(stmt) == SQLITE_ROW)
      db1_copy_col_text(running_files[running_count++], sizeof(running_files[0]), stmt, 0);
   sqlite3_finalize(stmt);

   static const char *pending_sql = "SELECT id, step_id, files FROM coord_job_tasks"
                                    " WHERE job_id = ? AND status = 'pending'"
                                    " ORDER BY id ASC";
   if (sqlite3_prepare_v2(db, pending_sql, -1, &stmt, NULL) != SQLITE_OK)
   {
      (void)db1_txn_end(db, "ROLLBACK");
      return -1;
   }
   sqlite3_bind_int(stmt, 1, job_id);

   int found_id = -1;
   int found_step_id = 0;
   char found_files[DB1_COORD_FILES_LEN] = {0};
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      int tid = sqlite3_column_int(stmt, 0);
      int sid = sqlite3_column_int(stmt, 1);
      const unsigned char *files = sqlite3_column_text(stmt, 2);
      int conflict = 0;
      for (int i = 0; i < running_count; i++)
      {
         if (files_overlap((const char *)files, running_files[i]))
         {
            conflict = 1;
            break;
         }
      }
      if (!conflict)
      {
         found_id = tid;
         found_step_id = sid;
         snprintf(found_files, sizeof(found_files), "%s", files ? (const char *)files : "");
         break;
      }
   }
   sqlite3_finalize(stmt);

   if (found_id < 0)
   {
      (void)db1_txn_end(db, "ROLLBACK");
      return -1;
   }

   static const char *claim_sql = "UPDATE coord_job_tasks SET status = 'claimed',"
                                  " claimed_by = ?, claimed_at = datetime('now')"
                                  " WHERE id = ? AND status = 'pending'";
   if (sqlite3_prepare_v2(db, claim_sql, -1, &stmt, NULL) != SQLITE_OK)
   {
      (void)db1_txn_end(db, "ROLLBACK");
      return -1;
   }
   sqlite3_bind_text(stmt, 1, delegate_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, found_id);
   if (sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(db) == 0)
   {
      sqlite3_finalize(stmt);
      (void)db1_txn_end(db, "ROLLBACK");
      return -1;
   }
   sqlite3_finalize(stmt);

   static const char *job_run_sql =
       "UPDATE coord_jobs SET status = 'running', updated_at = datetime('now')"
       " WHERE id = ? AND status = 'pending'";
   if (sqlite3_prepare_v2(db, job_run_sql, -1, &stmt, NULL) == SQLITE_OK)
   {
      sqlite3_bind_int(stmt, 1, job_id);
      (void)sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }

   if (db1_txn_end(db, "COMMIT") != 0)
   {
      /* gate already released; a failed COMMIT auto-rolls-back in sqlite */
      return -1;
   }

   out->id = found_id;
   out->job_id = job_id;
   out->step_id = found_step_id;
   snprintf(out->status, sizeof(out->status), "%s", "claimed");
   snprintf(out->claimed_by, sizeof(out->claimed_by), "%s", delegate_name);
   snprintf(out->files, sizeof(out->files), "%s", found_files);
   return found_id;
}

static int task_update_and_refresh(int task_id, const char *sql, const char *value)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, value ? value : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, task_id);
   if (sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(db) == 0)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   sqlite3_finalize(stmt);

   static const char *jid_sql = "SELECT job_id FROM coord_job_tasks WHERE id = ?";
   if (sqlite3_prepare_v2(db, jid_sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, task_id);
   if (sqlite3_step(stmt) == SQLITE_ROW)
      (void)db1_coord_job_refresh_status(sqlite3_column_int(stmt, 0));
   sqlite3_finalize(stmt);
   return 0;
}

int db1_coord_job_complete_task(int task_id, const char *result)
{
   static const char *sql = "UPDATE coord_job_tasks SET status = 'done',"
                            " result = ? WHERE id = ? AND status IN ('claimed', 'running')";
   return task_update_and_refresh(task_id, sql, result);
}

int db1_coord_job_fail_task(int task_id, const char *error)
{
   static const char *sql = "UPDATE coord_job_tasks SET status = 'failed',"
                            " error = ? WHERE id = ? AND status IN ('claimed', 'running')";
   return task_update_and_refresh(task_id, sql, error);
}

int db1_coord_job_release_task(int task_id)
{
   if (task_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE coord_job_tasks SET status = 'pending',"
                            " claimed_by = '', claimed_at = ''"
                            " WHERE id = ? AND status IN ('claimed', 'running')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, task_id);
   int rc = sqlite3_step(stmt);
   int ok = (rc == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
   sqlite3_finalize(stmt);
   return ok;
}

int db1_coord_job_release_task_bounded(int task_id, int max_requeues)
{
   if (task_id <= 0 || max_requeues <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE coord_job_tasks SET status = 'pending',"
                            " claimed_by = '', claimed_at = '', result = '', error = '',"
                            " preempt_requeues = preempt_requeues + 1"
                            " WHERE id = ? AND status IN ('claimed', 'running')"
                            " AND preempt_requeues < ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, task_id);
   sqlite3_bind_int(stmt, 2, max_requeues);
   int rc = sqlite3_step(stmt);
   int ok = (rc == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
   sqlite3_finalize(stmt);
   return ok;
}

int db1_coord_job_get(int job_id, db1_coord_job_t *out)
{
   if (job_id <= 0 || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT id, plan_id, status, max_concurrent, created_at, updated_at"
                            " FROM coord_jobs WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, job_id);
   if (sqlite3_step(stmt) != SQLITE_ROW)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   out->id = sqlite3_column_int(stmt, 0);
   out->plan_id = sqlite3_column_int(stmt, 1);
   db1_copy_col_text(out->status, sizeof(out->status), stmt, 2);
   out->max_concurrent = sqlite3_column_int(stmt, 3);
   db1_copy_col_text(out->created_at, sizeof(out->created_at), stmt, 4);
   db1_copy_col_text(out->updated_at, sizeof(out->updated_at), stmt, 5);
   sqlite3_finalize(stmt);

   static const char *count_sql = "SELECT status, COUNT(*) FROM coord_job_tasks"
                                  " WHERE job_id = ? GROUP BY status";
   if (sqlite3_prepare_v2(db, count_sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, job_id);
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *status = sqlite3_column_text(stmt, 0);
      int cnt = sqlite3_column_int(stmt, 1);
      out->total_tasks += cnt;
      if (!status)
         continue;
      if (strcmp((const char *)status, "done") == 0)
         out->done_tasks = cnt;
      else if (strcmp((const char *)status, "failed") == 0)
         out->failed_tasks = cnt;
      else if (strcmp((const char *)status, "claimed") == 0 ||
               strcmp((const char *)status, "running") == 0)
         out->running_tasks += cnt;
   }
   sqlite3_finalize(stmt);
   return 0;
}

int db1_coord_job_list_tasks(int job_id, db1_coord_task_t *out, int max)
{
   if (job_id <= 0 || !out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT id, job_id, step_id, status, claimed_by, claimed_at,"
                            " files, result, error, preempt_requeues, created_at"
                            " FROM coord_job_tasks WHERE job_id = ? ORDER BY id ASC";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, job_id);
   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
      fill_task(&out[count++], stmt);
   sqlite3_finalize(stmt);
   return count;
}

int db1_coord_job_cancel(int job_id)
{
   if (job_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *task_sql = "UPDATE coord_job_tasks SET status = 'failed',"
                                 " error = 'cancelled'"
                                 " WHERE job_id = ? AND status IN ('pending', 'claimed')";
   if (sqlite3_prepare_v2(db, task_sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, job_id);
   (void)sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   static const char *job_sql =
       "UPDATE coord_jobs SET status = 'cancelled', updated_at = datetime('now') WHERE id = ?";
   if (sqlite3_prepare_v2(db, job_sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, job_id);
   int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
   sqlite3_finalize(stmt);
   return rc;
}

int db1_coord_job_refresh_status(int job_id)
{
   if (job_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT"
                            " SUM(CASE WHEN status = 'pending' THEN 1 ELSE 0 END),"
                            " SUM(CASE WHEN status IN ('claimed', 'running') THEN 1 ELSE 0 END),"
                            " SUM(CASE WHEN status = 'done' THEN 1 ELSE 0 END),"
                            " SUM(CASE WHEN status = 'failed' THEN 1 ELSE 0 END),"
                            " COUNT(*)"
                            " FROM coord_job_tasks WHERE job_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, job_id);
   if (sqlite3_step(stmt) != SQLITE_ROW)
   {
      sqlite3_finalize(stmt);
      return -1;
   }

   int pending = sqlite3_column_int(stmt, 0);
   int running = sqlite3_column_int(stmt, 1);
   int done = sqlite3_column_int(stmt, 2);
   int failed = sqlite3_column_int(stmt, 3);
   int total = sqlite3_column_int(stmt, 4);
   sqlite3_finalize(stmt);

   const char *new_status = NULL;
   if (total == 0)
      new_status = "pending";
   else if (pending == 0 && running == 0 && failed == 0)
      new_status = "complete";
   else if (pending == 0 && running == 0 && done == 0)
      new_status = "failed";
   else if (pending == 0 && running == 0)
      new_status = "complete";
   else
      new_status = "running";

   static const char *update_sql = "UPDATE coord_jobs SET status = ?, updated_at = datetime('now')"
                                   " WHERE id = ? AND status NOT IN ('cancelled')";
   if (sqlite3_prepare_v2(db, update_sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, new_status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, job_id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_coord_job_has_file_conflict(int job_id, const char *files_json)
{
   if (job_id <= 0 || !files_json)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT files FROM coord_job_tasks"
                            " WHERE job_id = ? AND status IN ('claimed', 'running')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, job_id);
   int conflict = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *files = sqlite3_column_text(stmt, 0);
      if (files_overlap((const char *)files, files_json))
      {
         conflict = 1;
         break;
      }
   }
   sqlite3_finalize(stmt);
   return conflict;
}

int db1_coord_job_list_recent(db1_coord_job_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT id, plan_id, status, max_concurrent, created_at, updated_at"
                            " FROM coord_jobs ORDER BY id DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_coord_job_t *row = &out[count++];
      memset(row, 0, sizeof(*row));
      row->id = sqlite3_column_int(stmt, 0);
      row->plan_id = sqlite3_column_int(stmt, 1);
      db1_copy_col_text(row->status, sizeof(row->status), stmt, 2);
      row->max_concurrent = sqlite3_column_int(stmt, 3);
      db1_copy_col_text(row->created_at, sizeof(row->created_at), stmt, 4);
      db1_copy_col_text(row->updated_at, sizeof(row->updated_at), stmt, 5);
   }
   sqlite3_finalize(stmt);
   return count;
}

int db1_coord_job_list_active_ids(int *out_ids, int max)
{
   if (!out_ids || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   /* A job needs dispatch work when it has pending tasks AND is not cancelled/complete/failed. */
   static const char *sql = "SELECT DISTINCT job_id FROM coord_job_tasks"
                            " WHERE status = 'pending'"
                            " AND job_id IN (SELECT id FROM coord_jobs WHERE status NOT IN "
                            "('cancelled', 'complete', 'failed'))"
                            " LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
      out_ids[count++] = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

int db1_coord_task_get_dispatch(int task_id, char *role_out, size_t role_cap, char *prompt_out,
                                size_t prompt_cap, char *files_out, size_t files_cap, char *cwd_out,
                                size_t cwd_cap)
{
   if (task_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT role, prompt, files, cwd FROM coord_job_tasks WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, task_id);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      if (role_out && role_cap > 0)
         db1_copy_col_text(role_out, role_cap, stmt, 0);
      if (prompt_out && prompt_cap > 0)
         db1_copy_col_text(prompt_out, prompt_cap, stmt, 1);
      if (files_out && files_cap > 0)
         db1_copy_col_text(files_out, files_cap, stmt, 2);
      if (cwd_out && cwd_cap > 0)
         db1_copy_col_text(cwd_out, cwd_cap, stmt, 3);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}
