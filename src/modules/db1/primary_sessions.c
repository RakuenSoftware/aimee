/* db1/primary_sessions.c: durable primary-agent conversation transcripts. */

#include "primary_sessions.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db1_primary_session_save(const char *session_id, const char *agent_name, const char *provider,
                             const char *messages_json)
{
   if (!session_id || !session_id[0] || !agent_name || !agent_name[0] || !provider ||
       !provider[0] || !messages_json)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO primary_sessions"
       " (session_id, agent_name, provider, messages_json, created_at, updated_at)"
       " VALUES (?, ?, ?, ?, datetime('now'), datetime('now'))"
       " ON CONFLICT(session_id, agent_name, provider) DO UPDATE SET"
       "  messages_json = excluded.messages_json,"
       "  updated_at = datetime('now')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, agent_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, provider, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, messages_json, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

char *db1_primary_session_load(const char *session_id, const char *agent_name, const char *provider)
{
   if (!session_id || !session_id[0] || !agent_name || !agent_name[0] || !provider || !provider[0])
      return NULL;
   sqlite3 *db = db1_conn();
   if (!db)
      return NULL;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT messages_json FROM primary_sessions"
                            " WHERE session_id = ? AND agent_name = ? AND provider = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return NULL;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, agent_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, provider, -1, SQLITE_TRANSIENT);

   char *out = NULL;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *value = sqlite3_column_text(stmt, 0);
      int n = sqlite3_column_bytes(stmt, 0);
      if (value && n >= 0)
      {
         out = malloc((size_t)n + 1);
         if (out)
         {
            memcpy(out, value, (size_t)n);
            out[n] = '\0';
         }
      }
   }
   sqlite3_finalize(stmt);
   return out;
}

int db1_primary_session_delete(const char *session_id, const char *agent_name, const char *provider)
{
   if (!session_id || !session_id[0] || !agent_name || !agent_name[0] || !provider || !provider[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "DELETE FROM primary_sessions"
                            " WHERE session_id = ? AND agent_name = ? AND provider = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, agent_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, provider, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

static int fill_primary_row(sqlite3_stmt *stmt, db1_primary_session_row_t *row)
{
   memset(row, 0, sizeof(*row));
   db1_copy_col_text(row->session_id, sizeof(row->session_id), stmt, 0);
   db1_copy_col_text(row->agent_name, sizeof(row->agent_name), stmt, 1);
   db1_copy_col_text(row->provider, sizeof(row->provider), stmt, 2);
   db1_copy_col_text(row->created_at, sizeof(row->created_at), stmt, 4);
   db1_copy_col_text(row->updated_at, sizeof(row->updated_at), stmt, 5);

   const unsigned char *messages = sqlite3_column_text(stmt, 3);
   int n = sqlite3_column_bytes(stmt, 3);
   row->messages_json = malloc((size_t)n + 1);
   if (!row->messages_json)
      return -1;
   if (messages && n > 0)
      memcpy(row->messages_json, messages, (size_t)n);
   row->messages_json[n > 0 ? n : 0] = '\0';
   return 0;
}

static int alloc_rows_from_stmt(sqlite3_stmt *stmt, db1_primary_session_row_t **out, int max)
{
   if (!out || max <= 0)
      return 0;
   *out = NULL;
   db1_primary_session_row_t *rows = calloc((size_t)max, sizeof(*rows));
   if (!rows)
      return -1;

   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      if (fill_primary_row(stmt, &rows[count]) != 0)
      {
         db1_primary_session_rows_free(rows, count);
         return -1;
      }
      count++;
   }

   *out = rows;
   return count;
}

int db1_primary_session_alloc_recent(db1_primary_session_row_t **out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT session_id, agent_name, provider, messages_json, created_at, updated_at"
       " FROM primary_sessions ORDER BY updated_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = alloc_rows_from_stmt(stmt, out, max);
   sqlite3_finalize(stmt);
   return n;
}

int db1_primary_session_alloc_search(const char *query, db1_primary_session_row_t **out, int max)
{
   if (!out || max <= 0)
      return 0;
   *out = NULL;
   if (!query || !query[0])
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   char pattern[512];
   snprintf(pattern, sizeof(pattern), "%%%s%%", query);

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT session_id, agent_name, provider, messages_json, created_at, updated_at"
       " FROM primary_sessions"
       " WHERE messages_json LIKE ? OR session_id LIKE ? OR agent_name LIKE ? OR provider LIKE ?"
       " ORDER BY updated_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 5, max);
   int n = alloc_rows_from_stmt(stmt, out, max);
   sqlite3_finalize(stmt);
   return n;
}

int db1_primary_session_get_latest(const char *session_id, db1_primary_session_row_t *out)
{
   if (!session_id || !session_id[0] || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT session_id, agent_name, provider, messages_json, created_at, updated_at"
       " FROM primary_sessions WHERE session_id = ? ORDER BY updated_at DESC LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      rc = fill_primary_row(stmt, out);
   sqlite3_finalize(stmt);
   return rc;
}

void db1_primary_session_row_clear(db1_primary_session_row_t *row)
{
   if (!row)
      return;
   free(row->messages_json);
   memset(row, 0, sizeof(*row));
}

void db1_primary_session_rows_free(db1_primary_session_row_t *rows, int count)
{
   if (!rows)
      return;
   for (int i = 0; i < count; i++)
      free(rows[i].messages_json);
   free(rows);
}
