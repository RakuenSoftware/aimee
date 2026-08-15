/* db1/runtime_state.c: per-machine runtime state. */

#include "runtime_state.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

int db1_runtime_state_set(const char *key, const char *value)
{
   if (!key)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT OR REPLACE INTO memory_runtime_state (state_key, state_value) VALUES (?, ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, value ? value : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_runtime_state_get(const char *key, char *out, size_t out_len)
{
   if (!key || !out || out_len == 0)
      return -1;
   out[0] = '\0';
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT state_value FROM memory_runtime_state WHERE state_key = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *v = sqlite3_column_text(stmt, 0);
      snprintf(out, out_len, "%s", v ? (const char *)v : "");
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_runtime_state_add_int(const char *key, int delta, int *new_value_out)
{
   if (!key)
      return -1;

   char buf[64];
   int value = 0;
   if (db1_runtime_state_get(key, buf, sizeof(buf)) == 0 && buf[0])
      value = atoi(buf);
   value += delta;

   snprintf(buf, sizeof(buf), "%d", value);
   if (db1_runtime_state_set(key, buf) != 0)
      return -1;
   if (new_value_out)
      *new_value_out = value;
   return 0;
}
