/* db1/server_sessions.c: per-machine aimee-server connection sessions. */

#include "server_sessions.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void fill_row(db1_server_session_t *out, sqlite3_stmt *stmt)
{
   db1_copy_col_text(out->id, sizeof(out->id), stmt, 0);
   db1_copy_col_text(out->client_type, sizeof(out->client_type), stmt, 1);
   db1_copy_col_text(out->principal, sizeof(out->principal), stmt, 2);
   db1_copy_col_text(out->title, sizeof(out->title), stmt, 3);
   db1_copy_col_text(out->created_at, sizeof(out->created_at), stmt, 4);
   db1_copy_col_text(out->last_activity_at, sizeof(out->last_activity_at), stmt, 5);
   db1_copy_col_text(out->claude_session_id, sizeof(out->claude_session_id), stmt, 6);
   db1_copy_col_text(out->outcome, sizeof(out->outcome), stmt, 7);
   db1_copy_col_text(out->source, sizeof(out->source), stmt, 8);
   db1_copy_col_text(out->chat_key, sizeof(out->chat_key), stmt, 9);
}

int db1_server_session_create(const char *id, const char *client_type, const char *principal)
{
   if (!id)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO server_sessions (id, client_type, principal, title, created_at,"
       " last_activity_at) VALUES (?, ?, ?, '', datetime('now'), datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, client_type ? client_type : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, principal ? principal : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_server_session_get(const char *id, db1_server_session_t *out)
{
   if (!id || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT id, client_type, principal, title, created_at, last_activity_at,"
       " COALESCE(claude_session_id, ''), COALESCE(outcome, ''),"
       " COALESCE(source, ''), COALESCE(chat_key, '')"
       " FROM server_sessions WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      fill_row(out, stmt);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

static int run_simple_update(const char *sql, const char *id, const char *arg)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, arg ? arg : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_server_session_set_outcome(const char *id, const char *outcome)
{
   if (!id)
      return -1;
   return run_simple_update("UPDATE server_sessions SET outcome = ? WHERE id = ?", id, outcome);
}

int db1_server_session_delete(const char *id)
{
   if (!id)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM server_sessions WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_server_session_list_recent(db1_server_session_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT id, client_type, principal, title, created_at, last_activity_at,"
       " COALESCE(claude_session_id, ''), COALESCE(outcome, '')"
       " FROM server_sessions ORDER BY last_activity_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
      fill_row(&out[n++], stmt);
   sqlite3_finalize(stmt);
   return n;
}

int db1_server_session_search_by_title(const char *pattern, db1_server_session_t *out, int max)
{
   if (!pattern || !out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT id, client_type, principal, title, created_at, last_activity_at,"
       " COALESCE(claude_session_id, ''), COALESCE(outcome, '')"
       " FROM server_sessions WHERE title LIKE ?"
       " ORDER BY last_activity_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
      fill_row(&out[n++], stmt);
   sqlite3_finalize(stmt);
   return n;
}

int db1_server_session_count(const char *since_or_null)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   const char *sql = since_or_null ? "SELECT COUNT(*) FROM server_sessions WHERE created_at >= ?"
                                   : "SELECT COUNT(*) FROM server_sessions";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   if (since_or_null)
      sqlite3_bind_text(stmt, 1, since_or_null, -1, SQLITE_TRANSIENT);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      n = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

int db1_server_session_list_expired(int threshold_seconds, char (*out_ids)[DB1_SS_ID_LEN], int max)
{
   if (!out_ids || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   char sql[256];
   snprintf(sql, sizeof(sql),
            "SELECT id FROM server_sessions"
            " WHERE created_at <= datetime('now', '-%d seconds') LIMIT ?",
            threshold_seconds);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *v = sqlite3_column_text(stmt, 0);
      snprintf(out_ids[n], DB1_SS_ID_LEN, "%s", v ? (const char *)v : "");
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_server_session_delete_expired(int threshold_seconds)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   char sql[256];
   snprintf(sql, sizeof(sql),
            "DELETE FROM server_sessions"
            " WHERE created_at <= datetime('now', '-%d seconds')",
            threshold_seconds);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed;
}
