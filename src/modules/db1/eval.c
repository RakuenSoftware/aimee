/* db1/eval.c: per-machine evaluation run results. */

#include "eval.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int db1_eval_result_insert(const db1_eval_result_row_t *row)
{
   if (!row || !row->suite || !row->task_name)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO eval_results (suite, task_name, agent_name, ablation, success, turns,"
       " tool_calls, tool_call_failures, rescue_recoveries, prompt_tokens, completion_tokens,"
       " latency_ms, response, error, dataset_hash, target_hash, harness_version,"
       " hardware_profile, seed)"
       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, row->suite, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, row->task_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, row->agent_name ? row->agent_name : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, row->ablation ? row->ablation : "full", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 5, row->success);
   sqlite3_bind_int(stmt, 6, row->turns);
   sqlite3_bind_int(stmt, 7, row->tool_calls);
   sqlite3_bind_int(stmt, 8, row->tool_call_failures);
   sqlite3_bind_int(stmt, 9, row->rescue_recoveries);
   sqlite3_bind_int(stmt, 10, row->prompt_tokens);
   sqlite3_bind_int(stmt, 11, row->completion_tokens);
   sqlite3_bind_int(stmt, 12, row->latency_ms);
   sqlite3_bind_text(stmt, 13, row->response ? row->response : "", -1, SQLITE_TRANSIENT);
   if (row->error)
      sqlite3_bind_text(stmt, 14, row->error, -1, SQLITE_TRANSIENT);
   else
      sqlite3_bind_null(stmt, 14);
   sqlite3_bind_text(stmt, 15, row->dataset_hash ? row->dataset_hash : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 16, row->target_hash ? row->target_hash : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 17, row->harness_version ? row->harness_version : "1", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 18, row->hardware_profile ? row->hardware_profile : "", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 19, row->seed);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_eval_failed_tasks_recent(db1_eval_failed_task_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT DISTINCT task_name, error FROM eval_results"
                            " WHERE success = 0"
                            "   AND created_at > datetime('now', '-7 days')"
                            " ORDER BY created_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *t = sqlite3_column_text(stmt, 0);
      const unsigned char *e = sqlite3_column_text(stmt, 1);
      snprintf(out[n].task_name, sizeof(out[n].task_name), "%s", t ? (const char *)t : "");
      snprintf(out[n].error, sizeof(out[n].error), "%s", e ? (const char *)e : "");
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_eval_passed_tasks_recent(db1_eval_passed_task_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT DISTINCT task_name FROM eval_results"
                            " WHERE success = 1"
                            "   AND created_at > datetime('now', '-7 days')"
                            " ORDER BY created_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *t = sqlite3_column_text(stmt, 0);
      snprintf(out[n].task_name, sizeof(out[n].task_name), "%s", t ? (const char *)t : "");
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_eval_results_list(const char *suite_or_null, db1_eval_display_row_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   const char *sql = suite_or_null
                         ? "SELECT suite, task_name, agent_name, ablation, success, turns,"
                           " tool_calls, tool_call_failures, rescue_recoveries, latency_ms,"
                           " created_at"
                           " FROM eval_results WHERE suite = ? ORDER BY id DESC LIMIT ?"
                         : "SELECT suite, task_name, agent_name, ablation, success, turns,"
                           " tool_calls, tool_call_failures, rescue_recoveries, latency_ms,"
                           " created_at"
                           " FROM eval_results ORDER BY id DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   if (suite_or_null)
   {
      sqlite3_bind_text(stmt, 1, suite_or_null, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 2, max);
   }
   else
   {
      sqlite3_bind_int(stmt, 1, max);
   }

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *s = sqlite3_column_text(stmt, 0);
      const unsigned char *t = sqlite3_column_text(stmt, 1);
      const unsigned char *a = sqlite3_column_text(stmt, 2);
      const unsigned char *ab = sqlite3_column_text(stmt, 3);
      const unsigned char *c = sqlite3_column_text(stmt, 10);
      snprintf(out[n].suite, sizeof(out[n].suite), "%s", s ? (const char *)s : "");
      snprintf(out[n].task_name, sizeof(out[n].task_name), "%s", t ? (const char *)t : "");
      snprintf(out[n].agent_name, sizeof(out[n].agent_name), "%s", a ? (const char *)a : "");
      snprintf(out[n].ablation, sizeof(out[n].ablation), "%s", ab ? (const char *)ab : "");
      out[n].success = sqlite3_column_int(stmt, 4);
      out[n].turns = sqlite3_column_int(stmt, 5);
      out[n].tool_calls = sqlite3_column_int(stmt, 6);
      out[n].tool_call_failures = sqlite3_column_int(stmt, 7);
      out[n].rescue_recoveries = sqlite3_column_int(stmt, 8);
      out[n].latency_ms = sqlite3_column_int(stmt, 9);
      snprintf(out[n].created_at, sizeof(out[n].created_at), "%s", c ? (const char *)c : "");
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}
