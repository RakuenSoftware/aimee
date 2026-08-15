/* db1/tool_local_availability.c: local machine tool usability cache. */

#include "tool_local_availability.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int db1_tool_local_availability_set(const char *tool_uuid, int usable, const char *binary_path)
{
   if (!tool_uuid || !tool_uuid[0])
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO tool_local_availability (tool_uuid, usable, binary_path, checked_at)"
       " VALUES (?, ?, ?, datetime('now'))"
       " ON CONFLICT(tool_uuid) DO UPDATE SET"
       " usable = excluded.usable,"
       " binary_path = excluded.binary_path,"
       " checked_at = datetime('now')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, tool_uuid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, usable ? 1 : 0);
   sqlite3_bind_text(stmt, 3, binary_path ? binary_path : "", -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_tool_local_availability_get(const char *tool_uuid, db1_tool_local_availability_t *out)
{
   if (!tool_uuid || !tool_uuid[0] || !out)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT tool_uuid, usable, binary_path, checked_at"
                            " FROM tool_local_availability WHERE tool_uuid = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, tool_uuid, -1, SQLITE_TRANSIENT);
   memset(out, 0, sizeof(*out));
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out->tool_uuid, sizeof(out->tool_uuid), stmt, 0);
      out->usable = sqlite3_column_int(stmt, 1);
      db1_copy_col_text(out->binary_path, sizeof(out->binary_path), stmt, 2);
      db1_copy_col_text(out->checked_at, sizeof(out->checked_at), stmt, 3);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_tool_local_availability_delete(const char *tool_uuid)
{
   if (!tool_uuid || !tool_uuid[0])
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM tool_local_availability WHERE tool_uuid = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, tool_uuid, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed > 0 ? 0 : -1;
}

int db1_tool_local_availability_list(db1_tool_local_availability_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT tool_uuid, usable, binary_path, checked_at"
       " FROM tool_local_availability ORDER BY checked_at DESC, tool_uuid ASC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_tool_local_availability_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      db1_copy_col_text(row->tool_uuid, sizeof(row->tool_uuid), stmt, 0);
      row->usable = sqlite3_column_int(stmt, 1);
      db1_copy_col_text(row->binary_path, sizeof(row->binary_path), stmt, 2);
      db1_copy_col_text(row->checked_at, sizeof(row->checked_at), stmt, 3);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}
