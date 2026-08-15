/* db1/decisions.c: per-window decisions audit — SQLite-backed. */

#include "decisions.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int db1_decision_record(int64_t window_id, const char *description, const char *created_at)
{
   if (!description)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   /* Always write a concrete timestamp so the caller and the default
    * agree on the same second when it matters. If the caller passed NULL,
    * fall back to the server's datetime('now'). */
   const char *sql = created_at ? "INSERT INTO decisions (window_id, description, created_at)"
                                  " VALUES (?, ?, ?)"
                                : "INSERT INTO decisions (window_id, description, created_at)"
                                  " VALUES (?, ?, datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_int64(stmt, 1, window_id);
   sqlite3_bind_text(stmt, 2, description, -1, SQLITE_TRANSIENT);
   if (created_at)
      sqlite3_bind_text(stmt, 3, created_at, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}
