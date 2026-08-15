/* db1/git_ownership.c: user-local branch ownership domain API. */

#include "git_ownership.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

int db1_git_ownership_upsert(const char *repo_path, const char *branch_name, const char *session_id)
{
   if (!repo_path || !branch_name || !session_id)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO branch_ownership (repo_path, branch_name, session_id)"
       " VALUES (?, ?, ?)"
       " ON CONFLICT(repo_path, branch_name) DO UPDATE SET session_id = excluded.session_id";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, repo_path, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, branch_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, session_id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_git_ownership_delete(const char *repo_path, const char *branch_name)
{
   if (!repo_path || !branch_name)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM branch_ownership WHERE repo_path = ? AND branch_name = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, repo_path, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, branch_name, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

int db1_git_ownership_get_owner(const char *repo_path, const char *branch_name, char *owner_out,
                                size_t owner_len)
{
   if (!repo_path || !branch_name || !owner_out || owner_len == 0)
      return -1;
   owner_out[0] = '\0';
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT session_id FROM branch_ownership WHERE repo_path = ? AND branch_name = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, repo_path, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, branch_name, -1, SQLITE_TRANSIENT);

   int found = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *owner = sqlite3_column_text(stmt, 0);
      snprintf(owner_out, owner_len, "%s", owner ? (const char *)owner : "");
      found = 1;
   }
   sqlite3_finalize(stmt);
   return found;
}

int db1_git_ownership_get_branch_for_session(const char *repo_path, const char *session_id,
                                             char *branch_out, size_t branch_len)
{
   if (!repo_path || !session_id || !branch_out || branch_len == 0)
      return -1;
   branch_out[0] = '\0';
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT branch_name FROM branch_ownership WHERE repo_path = ? AND session_id = ? LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, repo_path, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_TRANSIENT);

   int found = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *name = sqlite3_column_text(stmt, 0);
      snprintf(branch_out, branch_len, "%s", name ? (const char *)name : "");
      found = 1;
   }
   sqlite3_finalize(stmt);
   return found;
}

int db1_git_ownership_find_session_by_prefix(const char *session_prefix, char *session_out,
                                             size_t session_len)
{
   if (!session_prefix || !session_out || session_len == 0)
      return -1;
   session_out[0] = '\0';
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   char like_pat[128];
   snprintf(like_pat, sizeof(like_pat), "%s%%", session_prefix);

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT session_id FROM branch_ownership WHERE session_id LIKE ? LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, like_pat, -1, SQLITE_TRANSIENT);

   int found = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *sid = sqlite3_column_text(stmt, 0);
      snprintf(session_out, session_len, "%s", sid ? (const char *)sid : "");
      found = 1;
   }
   sqlite3_finalize(stmt);
   return found;
}
