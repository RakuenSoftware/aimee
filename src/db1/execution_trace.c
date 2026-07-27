/* db1/execution_trace.c: per-machine execution trace rows for turns and tools. */

#include "execution_trace.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int db1_execution_trace_insert(const db1_execution_trace_insert_row_t *row)
{
   if (!row || !row->direction)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   const char *sql_with_context =
       "INSERT INTO execution_trace (plan_id, session_id, turn, direction, content,"
       " tool_name, tool_args, tool_result, context_hash)"
       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql_with_context, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, row->plan_id > 0 ? row->plan_id : 0);
   sqlite3_bind_text(stmt, 2, row->session_id ? row->session_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, row->turn);
   sqlite3_bind_text(stmt, 4, row->direction, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, row->content ? row->content : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, row->tool_name ? row->tool_name : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 7, row->tool_args ? row->tool_args : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 8, row->tool_result ? row->tool_result : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 9, row->context_hash ? row->context_hash : "", -1, SQLITE_TRANSIENT);

   const int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

/* How many trace rows belong to `session_id`. Exists so attribution is checkable:
 * concurrent delegates share the table and are otherwise indistinguishable. */
int db1_execution_trace_count_for_session(const char *session_id)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id)
      return -1;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM execution_trace WHERE session_id = ?", -1,
                          &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   int count = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

int db1_execution_trace_list_recent(db1_execution_trace_recent_row_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT id, turn, direction, COALESCE(tool_name, ''), created_at"
                            " FROM execution_trace ORDER BY id DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_execution_trace_recent_row_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->id = sqlite3_column_int(stmt, 0);
      row->turn = sqlite3_column_int(stmt, 1);
      db1_copy_col_text(row->direction, sizeof(row->direction), stmt, 2);
      db1_copy_col_text(row->tool_name, sizeof(row->tool_name), stmt, 3);
      db1_copy_col_text(row->created_at, sizeof(row->created_at), stmt, 4);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_execution_trace_get(int trace_id, db1_execution_trace_detail_t *out)
{
   if (!out || trace_id <= 0)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   const char *sql = "SELECT id, COALESCE(plan_id, 0), turn, direction, COALESCE(content, ''),"
                     " COALESCE(tool_name, ''), COALESCE(tool_args, ''),"
                     " COALESCE(tool_result, ''), COALESCE(context_hash, ''), created_at"
                     " FROM execution_trace WHERE id = ?";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, trace_id);
   memset(out, 0, sizeof(*out));
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      out->id = sqlite3_column_int(stmt, 0);
      out->plan_id = sqlite3_column_int(stmt, 1);
      out->turn = sqlite3_column_int(stmt, 2);
      db1_copy_col_text(out->direction, sizeof(out->direction), stmt, 3);
      db1_copy_col_text(out->content, sizeof(out->content), stmt, 4);
      db1_copy_col_text(out->tool_name, sizeof(out->tool_name), stmt, 5);
      db1_copy_col_text(out->tool_args, sizeof(out->tool_args), stmt, 6);
      db1_copy_col_text(out->tool_result, sizeof(out->tool_result), stmt, 7);
      db1_copy_col_text(out->context_hash, sizeof(out->context_hash), stmt, 8);
      db1_copy_col_text(out->created_at, sizeof(out->created_at), stmt, 9);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_execution_trace_list_tool_calls(db1_execution_trace_tool_call_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT turn, direction, COALESCE(tool_name, ''), COALESCE(tool_args, ''),"
       " COALESCE(tool_result, '')"
       " FROM execution_trace ORDER BY id DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_execution_trace_tool_call_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->turn = sqlite3_column_int(stmt, 0);
      db1_copy_col_text(row->direction, sizeof(row->direction), stmt, 1);
      db1_copy_col_text(row->tool_name, sizeof(row->tool_name), stmt, 2);
      db1_copy_col_text(row->tool_args, sizeof(row->tool_args), stmt, 3);
      db1_copy_col_text(row->tool_result, sizeof(row->tool_result), stmt, 4);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_execution_trace_list_after_id(int64_t after_id, db1_execution_trace_mining_row_t *out,
                                      int max)
{
   if (!out || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT id, COALESCE(plan_id, 0), turn, direction, COALESCE(tool_name, ''),"
       " COALESCE(tool_args, ''), COALESCE(tool_result, '')"
       " FROM execution_trace"
       " WHERE id > ? AND COALESCE(tool_name, '') != ''"
       " ORDER BY plan_id, turn, id";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int64(stmt, 1, after_id);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_execution_trace_mining_row_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->id = sqlite3_column_int64(stmt, 0);
      row->plan_id = sqlite3_column_int(stmt, 1);
      row->turn = sqlite3_column_int(stmt, 2);
      db1_copy_col_text(row->direction, sizeof(row->direction), stmt, 3);
      db1_copy_col_text(row->tool_name, sizeof(row->tool_name), stmt, 4);
      db1_copy_col_text(row->tool_args, sizeof(row->tool_args), stmt, 5);
      db1_copy_col_text(row->tool_result, sizeof(row->tool_result), stmt, 6);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}
