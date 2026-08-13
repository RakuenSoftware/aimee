#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "db1.h"
#include "platform_test_util.h"
#include <sqlite3.h>

extern sqlite3 *db1_conn(void);

static int column_exists(sqlite3 *db, const char *table, const char *column)
{
   sqlite3_stmt *stmt = NULL;
   char sql[128];
   int found = 0;

   snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
   assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *name = sqlite3_column_text(stmt, 1);
      if (name && strcmp((const char *)name, column) == 0)
      {
         found = 1;
         break;
      }
   }
   sqlite3_finalize(stmt);
   return found;
}

int main(void)
{
   printf("execution_trace: ");
   char path[256];
   snprintf(path, sizeof path, "%s/test_execution_trace_XXXXXX.db", platform_tmpdir());
   int fd = mkstemps(path, 3);
   assert(fd >= 0);
   close(fd);
   platform_test_remove_sqlite(path);

   assert(db1_init(path) == 0);
   assert(column_exists(db1_conn(), "execution_trace", "context_hash"));

   db1_execution_trace_insert_row_t first = {
       .plan_id = 7,
       .turn = 3,
       .direction = "call",
       .content = "content",
       .tool_name = "bash",
       .tool_args = "{\"cmd\":\"true\"}",
       .tool_result = "ok",
       .context_hash = "abc123",
   };
   db1_execution_trace_insert_row_t second = {
       .plan_id = 0,
       .turn = 4,
       .direction = "response",
       .content = "done",
       .tool_name = NULL,
       .tool_args = NULL,
       .tool_result = NULL,
       .context_hash = NULL,
   };

   assert(db1_execution_trace_insert(&first) == 0);
   assert(db1_execution_trace_insert(&second) == 0);

   db1_execution_trace_recent_row_t recent[4];
   int count = db1_execution_trace_list_recent(recent, 4);
   assert(count == 2);
   assert(recent[0].turn == 4);
   assert(strcmp(recent[1].tool_name, "bash") == 0);

   db1_execution_trace_detail_t detail;
   assert(db1_execution_trace_get(recent[1].id, &detail) == 0);
   assert(detail.plan_id == 7);
   assert(strcmp(detail.direction, "call") == 0);
   assert(strcmp(detail.content, "content") == 0);
   assert(strcmp(detail.tool_args, "{\"cmd\":\"true\"}") == 0);
   assert(strcmp(detail.tool_result, "ok") == 0);
   assert(strcmp(detail.context_hash, "abc123") == 0);

   db1_execution_trace_tool_call_t tool_rows[4];
   count = db1_execution_trace_list_tool_calls(tool_rows, 4);
   assert(count == 2);
   assert(strcmp(tool_rows[1].tool_name, "bash") == 0);
   assert(strcmp(tool_rows[1].tool_args, "{\"cmd\":\"true\"}") == 0);

   db1_shutdown();
   platform_test_remove_sqlite(path);
   printf("PASS\n");
   return 0;
}
