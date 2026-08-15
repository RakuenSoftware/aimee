/* db1/session_paths.c: see session_paths.h */

#include "session_paths.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int db1_session_write_path_record(const char *session_id, const char *path)
{
   if (!session_id || !session_id[0] || !path || !path[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   /* Auto-allocate seq via COALESCE(MAX(seq)+1, 0) so the caller does
    * not have to track per-session counters. The PRIMARY KEY is
    * (session_id, seq); duplicates are not possible by construction. */
   static const char *sql = "INSERT INTO session_state_write_paths (session_id, seq, path)"
                            " VALUES (?, COALESCE((SELECT MAX(seq) FROM session_state_write_paths"
                            "                       WHERE session_id = ?) + 1, 0), ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, path, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_session_stale_reads(const char *parent_session_id, const char *child_session_id,
                            char (*out_paths)[DB1_SESSION_PATH_LEN], int max)
{
   if (!out_paths || max <= 0)
      return 0;
   if (!parent_session_id || !parent_session_id[0] || !child_session_id || !child_session_id[0])
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   /* DISTINCT path because both source tables can repeat a path across
    * multiple seqs. INNER JOIN on the path itself; ORDER BY for stable
    * test output. */
   static const char *sql = "SELECT DISTINCT r.path"
                            " FROM session_state_read_paths r"
                            " JOIN session_state_write_paths w ON r.path = w.path"
                            " WHERE r.session_id = ? AND w.session_id = ?"
                            " ORDER BY r.path"
                            " LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, parent_session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, child_session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *p = sqlite3_column_text(stmt, 0);
      if (!p)
         continue;
      snprintf(out_paths[n], DB1_SESSION_PATH_LEN, "%s", (const char *)p);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}
