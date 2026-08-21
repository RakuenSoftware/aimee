/* db1/checkpoints.c: session checkpoint storage — SQLite-backed. */

#include "checkpoints.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static void row_to_checkpoint(sqlite3_stmt *stmt, db1_checkpoint_t *cp)
{
   memset(cp, 0, sizeof(*cp));
   cp->id = sqlite3_column_int64(stmt, 0);
   cp->task_id = sqlite3_column_int64(stmt, 1);
   db1_copy_col_text(cp->session_id, sizeof(cp->session_id), stmt, 2);
   db1_copy_col_text(cp->label, sizeof(cp->label), stmt, 3);
   db1_copy_col_text(cp->snapshot, sizeof(cp->snapshot), stmt, 4);
   db1_copy_col_text(cp->created_at, sizeof(cp->created_at), stmt, 5);
}

int db1_checkpoint_insert(const char *label, const char *session_id, int64_t task_id,
                          const char *snapshot_json, db1_checkpoint_t *out)
{
   sqlite3 *db = db1_conn();
   if (!db || !label || !snapshot_json)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO checkpoints (task_id, session_id, label, snapshot, created_at)"
       " VALUES (?, ?, ?, ?, datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int64(stmt, 1, task_id);
   sqlite3_bind_text(stmt, 2, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, label, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, snapshot_json, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE)
      return -1;

   if (out)
      return db1_checkpoint_get(sqlite3_last_insert_rowid(db), out);
   return 0;
}

int db1_checkpoint_get(int64_t id, db1_checkpoint_t *out)
{
   sqlite3 *db = db1_conn();
   if (!db || !out)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT id, task_id, session_id, label, snapshot, created_at FROM checkpoints WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int64(stmt, 1, id);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      row_to_checkpoint(stmt, out);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_checkpoint_list(int limit, db1_checkpoint_t *out, int max)
{
   sqlite3 *db = db1_conn();
   if (!db || !out || max <= 0)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT id, task_id, session_id, label, snapshot, created_at"
                            " FROM checkpoints ORDER BY created_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   sqlite3_bind_int(stmt, 1, limit > 0 ? limit : max);
   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
      row_to_checkpoint(stmt, &out[count++]);
   sqlite3_finalize(stmt);
   return count;
}

int db1_checkpoint_delete(int64_t id)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, "DELETE FROM checkpoints WHERE id = ?", -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int64(stmt, 1, id);
   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(db);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE)
      return -1;
   return changes > 0 ? 0 : -1;
}
