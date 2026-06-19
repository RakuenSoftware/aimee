/* db1/webchat_claude_sessions.c: per-(principal, tab) Claude --resume binding. */

#include "webchat_claude_sessions.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int db1_webchat_claude_session_get(const char *principal, const char *aimee_session_id, char *out,
                                   size_t out_n)
{
   if (out && out_n)
      out[0] = '\0';
   if (!aimee_session_id || !aimee_session_id[0] || !out || out_n == 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT claude_session_id FROM webchat_claude_sessions"
                            " WHERE principal = ? AND aimee_session_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, principal ? principal : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, aimee_session_id, -1, SQLITE_TRANSIENT);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *v = sqlite3_column_text(stmt, 0);
      if (v && v[0])
      {
         snprintf(out, out_n, "%s", (const char *)v);
         rc = 0;
      }
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_webchat_claude_session_owned_by_other(const char *principal, const char *aimee_session_id,
                                              const char *claude_session_id)
{
   if (!claude_session_id || !claude_session_id[0])
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT 1 FROM webchat_claude_sessions"
                            " WHERE claude_session_id = ?"
                            "   AND NOT (principal = ? AND aimee_session_id = ?) LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(stmt, 1, claude_session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, principal ? principal : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, aimee_session_id ? aimee_session_id : "", -1, SQLITE_TRANSIENT);

   int owned = (sqlite3_step(stmt) == SQLITE_ROW);
   sqlite3_finalize(stmt);
   return owned;
}

int db1_webchat_claude_session_bind(const char *principal, const char *aimee_session_id,
                                    const char *claude_session_id)
{
   if (!aimee_session_id || !aimee_session_id[0] || !claude_session_id || !claude_session_id[0])
      return -1;
   /* Refuse to bind an id another tab already owns — a binding is never hijacked. */
   if (db1_webchat_claude_session_owned_by_other(principal, aimee_session_id, claude_session_id))
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO webchat_claude_sessions (principal, aimee_session_id, claude_session_id,"
       " updated_at) VALUES (?, ?, ?, datetime('now'))"
       " ON CONFLICT(principal, aimee_session_id) DO UPDATE SET"
       "  claude_session_id = excluded.claude_session_id, updated_at = datetime('now')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, principal ? principal : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, aimee_session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, claude_session_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}
