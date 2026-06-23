/* db1/webchat_live.c: the live (in-progress) webchat turn, mirrored to db1. */

#include "webchat_live.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

int db1_webchat_live_set(const char *session_id, const char *turn_id, const char *text,
                         const char *status)
{
   if (!session_id || !session_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   /* One row per session; rev increments on every write so a poller can tell the
    * row advanced without diffing the text. COALESCE keeps the bump correct on
    * the first insert (no prior row → 0 → 1). */
   const char *sql = "INSERT INTO webchat_live(session_id, turn_id, rev, text, status, updated_at) "
                     "VALUES(?1, ?2, 1, ?3, ?4, datetime('now')) "
                     "ON CONFLICT(session_id) DO UPDATE SET "
                     "turn_id=excluded.turn_id, rev=webchat_live.rev+1, text=excluded.text, "
                     "status=excluded.status, updated_at=datetime('now')";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, turn_id ? turn_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, text ? text : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, status ? status : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

static char *dup_col(sqlite3_stmt *stmt, int col)
{
   const unsigned char *v = sqlite3_column_text(stmt, col);
   int n = sqlite3_column_bytes(stmt, col);
   char *out = malloc((size_t)n + 1);
   if (!out)
      return NULL;
   if (n > 0 && v)
      memcpy(out, v, (size_t)n);
   out[n] = '\0';
   return out;
}

int db1_webchat_live_get(const char *session_id, long long since_rev, char **turn_id, char **text,
                         char **status, long long *rev)
{
   if (!session_id || !session_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   const char *sql =
       "SELECT turn_id, text, status, rev FROM webchat_live WHERE session_id=?1 AND rev>?2";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 2, since_rev);
   int found = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      if (turn_id)
         *turn_id = dup_col(stmt, 0);
      if (text)
         *text = dup_col(stmt, 1);
      if (status)
         *status = dup_col(stmt, 2);
      if (rev)
         *rev = sqlite3_column_int64(stmt, 3);
      found = 1;
   }
   sqlite3_finalize(stmt);
   return found;
}
