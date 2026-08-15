/* db1/execution_plans.c: durable per-machine execution plans, steps, and evidence. */

#include "execution_plans.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int status_code_from_name(const char *status)
{
   if (!status || !status[0] || strcmp(status, "pending") == 0)
      return 0;
   if (strcmp(status, "0") == 0)
      return 0;
   if (strcmp(status, "running") == 0)
      return 1;
   if (strcmp(status, "1") == 0)
      return 1;
   if (strcmp(status, "done") == 0)
      return 2;
   if (strcmp(status, "2") == 0)
      return 2;
   if (strcmp(status, "failed") == 0)
      return 3;
   if (strcmp(status, "3") == 0)
      return 3;
   if (strcmp(status, "rolled_back") == 0)
      return 4;
   if (strcmp(status, "4") == 0)
      return 4;
   return 0;
}

static int bind_step_sqlite(sqlite3_stmt *stmt, int plan_id, int seq, const cJSON *step)
{
   cJSON *action = cJSON_GetObjectItem((cJSON *)step, "action");
   cJSON *pre = cJSON_GetObjectItem((cJSON *)step, "precondition");
   cJSON *suc = cJSON_GetObjectItem((cJSON *)step, "success_predicate");
   cJSON *rb = cJSON_GetObjectItem((cJSON *)step, "rollback");
   cJSON *after = cJSON_GetObjectItem((cJSON *)step, "after");
   char *deps_json = NULL;

   if (after && cJSON_IsArray(after))
      deps_json = cJSON_PrintUnformatted(after);

   sqlite3_bind_int(stmt, 1, plan_id);
   sqlite3_bind_int(stmt, 2, seq);
   sqlite3_bind_text(stmt, 3, (action && cJSON_IsString(action)) ? action->valuestring : "", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, (pre && cJSON_IsString(pre)) ? pre->valuestring : "", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, (suc && cJSON_IsString(suc)) ? suc->valuestring : "", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, (rb && cJSON_IsString(rb)) ? rb->valuestring : "", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 7, deps_json ? deps_json : "[]", -1, SQLITE_TRANSIENT);
   free(deps_json);
   return 0;
}

static void fill_plan_step(plan_step_t *step, sqlite3_stmt *stmt)
{
   memset(step, 0, sizeof(*step));
   step->id = sqlite3_column_int(stmt, 0);
   db1_copy_col_text(step->action, sizeof(step->action), stmt, 2);
   db1_copy_col_text(step->precondition, sizeof(step->precondition), stmt, 3);
   db1_copy_col_text(step->success_predicate, sizeof(step->success_predicate), stmt, 4);
   db1_copy_col_text(step->rollback, sizeof(step->rollback), stmt, 5);
   step->status = status_code_from_name((const char *)sqlite3_column_text(stmt, 6));
   db1_copy_col_text(step->output, sizeof(step->output), stmt, 7);

   const unsigned char *deps_text = sqlite3_column_text(stmt, 8);
   if (deps_text && deps_text[0] && strcmp((const char *)deps_text, "[]") != 0)
   {
      cJSON *deps_arr = cJSON_Parse((const char *)deps_text);
      if (deps_arr && cJSON_IsArray(deps_arr))
      {
         int ndeps = cJSON_GetArraySize(deps_arr);
         for (int d = 0; d < ndeps && step->dep_count < AGENT_MAX_PLAN_DEPS; d++)
         {
            cJSON *item = cJSON_GetArrayItem(deps_arr, d);
            if (item && cJSON_IsNumber(item))
               step->depends_on[step->dep_count++] = (int)item->valuedouble;
         }
      }
      cJSON_Delete(deps_arr);
   }
}

int db1_execution_plan_create(const char *agent_name, const char *task, const cJSON *steps_json)
{
   if (!agent_name || !task || !steps_json)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *plan_sql = "INSERT INTO execution_plans (agent_name, task, status)"
                                 " VALUES (?, ?, 'pending')";
   if (sqlite3_prepare_v2(db, plan_sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, task, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) != SQLITE_DONE)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   sqlite3_finalize(stmt);
   int plan_id = (int)sqlite3_last_insert_rowid(db);

   static const char *step_sql = "INSERT INTO plan_steps (plan_id, seq, action, precondition,"
                                 " success_predicate, rollback, deps, status)"
                                 " VALUES (?, ?, ?, ?, ?, ?, ?, 'pending')";
   if (sqlite3_prepare_v2(db, step_sql, -1, &stmt, NULL) != SQLITE_OK)
      return plan_id;

   int n = cJSON_GetArraySize((cJSON *)steps_json);
   for (int i = 0; i < n && i < AGENT_MAX_PLAN_STEPS; i++)
   {
      cJSON *step = cJSON_GetArrayItem((cJSON *)steps_json, i);
      if (!step)
         continue;
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);
      bind_step_sqlite(stmt, plan_id, i, step);
      (void)sqlite3_step(stmt);
   }
   sqlite3_finalize(stmt);

   return plan_id;
}

int db1_execution_plan_get(int plan_id, plan_t *out)
{
   if (!out || plan_id <= 0)
      return -1;
   memset(out, 0, sizeof(*out));
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *plan_sql =
       "SELECT id, agent_name, task, status FROM execution_plans WHERE id = ?";
   if (sqlite3_prepare_v2(db, plan_sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, plan_id);
   if (sqlite3_step(stmt) != SQLITE_ROW)
   {
      sqlite3_finalize(stmt);
      return -1;
   }
   out->id = sqlite3_column_int(stmt, 0);
   db1_copy_col_text(out->agent_name, sizeof(out->agent_name), stmt, 1);
   db1_copy_col_text(out->task, sizeof(out->task), stmt, 2);
   db1_copy_col_text(out->status, sizeof(out->status), stmt, 3);
   sqlite3_finalize(stmt);

   static const char *step_sql =
       "SELECT id, seq, action, precondition, success_predicate, rollback,"
       " status, output, deps FROM plan_steps WHERE plan_id = ? ORDER BY seq";
   if (sqlite3_prepare_v2(db, step_sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, plan_id);
   while (out->step_count < AGENT_MAX_PLAN_STEPS && sqlite3_step(stmt) == SQLITE_ROW)
      fill_plan_step(&out->steps[out->step_count++], stmt);
   sqlite3_finalize(stmt);
   return 0;
}

int db1_execution_plan_list(plan_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT id FROM execution_plans ORDER BY id DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      int id = sqlite3_column_int(stmt, 0);
      if (db1_execution_plan_get(id, &out[n]) == 0)
         n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_execution_plan_exists(int plan_id)
{
   if (plan_id <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT 1 FROM execution_plans WHERE id = ? LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, plan_id);
   int found = (sqlite3_step(stmt) == SQLITE_ROW);
   sqlite3_finalize(stmt);
   return found;
}

int db1_execution_plan_count_steps(int plan_id)
{
   if (plan_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT COUNT(*) FROM plan_steps WHERE plan_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, plan_id);
   int count = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

int db1_execution_plan_list_running_ids(int *out_ids, int max)
{
   if (!out_ids || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT id FROM execution_plans WHERE status = 'running' LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
      out_ids[n++] = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

int db1_execution_plan_list_recent_summaries(db1_execution_plan_summary_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT p.id, p.agent_name, p.task, p.status, p.created_at,"
       " (SELECT COUNT(*) FROM plan_steps WHERE plan_id = p.id),"
       " (SELECT COUNT(*) FROM plan_steps WHERE plan_id = p.id AND status = 'done')"
       " FROM execution_plans p ORDER BY p.id DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_execution_plan_summary_t *row = &out[n++];
      memset(row, 0, sizeof(*row));
      row->id = sqlite3_column_int(stmt, 0);
      db1_copy_col_text(row->agent_name, sizeof(row->agent_name), stmt, 1);
      db1_copy_col_text(row->task, sizeof(row->task), stmt, 2);
      db1_copy_col_text(row->status, sizeof(row->status), stmt, 3);
      db1_copy_col_text(row->created_at, sizeof(row->created_at), stmt, 4);
      row->total_steps = sqlite3_column_int(stmt, 5);
      row->done_steps = sqlite3_column_int(stmt, 6);
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_execution_plan_set_status(int plan_id, const char *status)
{
   if (plan_id <= 0 || !status || !status[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE execution_plans SET status = ? WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, plan_id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
}

int db1_execution_plan_cancel_by_id(int plan_id, const char *reason)
{
   if (plan_id <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE execution_plans SET status = 'cancelled', cancelled_at = datetime('now'),"
       " cancel_reason = ? WHERE id = ? AND status IN ('pending', 'running')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(stmt, 1, reason ? reason : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, plan_id);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed;
}

int db1_execution_plan_cancel_stale(int threshold_seconds, const char *reason)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   char sql[384];
   snprintf(sql, sizeof(sql),
            "UPDATE execution_plans SET status = 'cancelled', cancelled_at = datetime('now'),"
            " cancel_reason = ? WHERE status = 'running'"
            " AND created_at < datetime('now', '-%d seconds')",
            threshold_seconds);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(stmt, 1, reason ? reason : "orphan cleanup", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed;
}

int db1_plan_step_set_status(int step_id, const char *status)
{
   return db1_plan_step_set_status_output(step_id, status, NULL);
}

int db1_plan_step_set_status_output(int step_id, const char *status, const char *output)
{
   if (step_id <= 0 || !status || !status[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   const char *sql = output ? "UPDATE plan_steps SET status = ?, output = ? WHERE id = ?"
                            : "UPDATE plan_steps SET status = ? WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
   if (output)
   {
      sqlite3_bind_text(stmt, 2, output, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 3, step_id);
   }
   else
      sqlite3_bind_int(stmt, 2, step_id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? sqlite3_changes(db) : -1;
}

int db1_plan_step_cancel_active_for_plan(int plan_id)
{
   if (plan_id <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE plan_steps SET status = 'failed', finished_at = datetime('now')"
       " WHERE plan_id = ? AND (status IN ('pending', 'running') OR status IN (0, 1))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, plan_id);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed;
}

int db1_plan_step_cancel_orphans(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "UPDATE plan_steps SET status = 'failed', finished_at = datetime('now')"
                            " WHERE (status = 'running' OR status = 1) AND plan_id NOT IN"
                            " (SELECT id FROM execution_plans WHERE status = 'running')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed;
}

int db1_step_evidence_insert(int plan_id, int step_id, const char *kind, const char *content,
                             int passed, const char *strength)
{
   if (step_id <= 0 || !kind || !content || !strength)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO step_evidence (plan_id, step_id, kind, content, passed, strength)"
       " VALUES (?, ?, ?, ?, ?, ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, plan_id);
   sqlite3_bind_int(stmt, 2, step_id);
   sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, content, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 5, passed);
   sqlite3_bind_text(stmt, 6, strength, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_step_evidence_get_latest(int step_id, db1_step_evidence_latest_t *out)
{
   if (step_id <= 0 || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT strength, passed, kind, created_at FROM step_evidence"
                            " WHERE step_id = ? ORDER BY id DESC LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, step_id);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out->strength, sizeof(out->strength), stmt, 0);
      out->passed = sqlite3_column_int(stmt, 1);
      db1_copy_col_text(out->kind, sizeof(out->kind), stmt, 2);
      db1_copy_col_text(out->created_at, sizeof(out->created_at), stmt, 3);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}
