/* db1/wm.c: working_memory — SQLite-backed implementation.
 *
 * Production implementation of wm.h. All SQL lives here. Domain functions
 * pull the private connection from db1_conn() and drive sqlite3 directly. */

#include "wm.h"
#include "db1_internal.h"
#include "aimee.h" /* now_utc */

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Row helper --- */

static void row_to_wm(sqlite3_stmt *stmt, wm_entry_t *e)
{
   const unsigned char *sid = sqlite3_column_text(stmt, 1);
   const unsigned char *key = sqlite3_column_text(stmt, 2);
   const unsigned char *val = sqlite3_column_text(stmt, 3);
   const unsigned char *cat = sqlite3_column_text(stmt, 4);
   const unsigned char *ca = sqlite3_column_text(stmt, 5);
   const unsigned char *ua = sqlite3_column_text(stmt, 6);
   const unsigned char *ea = sqlite3_column_text(stmt, 7);

   e->id = sqlite3_column_int64(stmt, 0);
   snprintf(e->session_id, sizeof(e->session_id), "%s", sid ? (const char *)sid : "");
   snprintf(e->key, sizeof(e->key), "%s", key ? (const char *)key : "");
   snprintf(e->value, sizeof(e->value), "%s", val ? (const char *)val : "");
   snprintf(e->category, sizeof(e->category), "%s", cat ? (const char *)cat : "general");
   snprintf(e->created_at, sizeof(e->created_at), "%s", ca ? (const char *)ca : "");
   snprintf(e->updated_at, sizeof(e->updated_at), "%s", ua ? (const char *)ua : "");
   snprintf(e->expires_at, sizeof(e->expires_at), "%s", ea ? (const char *)ea : "");
}

static void compute_expires(int ttl_seconds, char *buf, size_t len)
{
   if (ttl_seconds <= 0)
   {
      buf[0] = '\0';
      return;
   }
   time_t t = time(NULL) + ttl_seconds;
   struct tm tm_buf;
   gmtime_r(&t, &tm_buf);
   strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
}

/* --- Public API --- */

int db1_wm_set(const char *session_id, const char *key, const char *value, const char *category,
               int ttl_seconds)
{
   if (!session_id || !key || !value)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql =
       "INSERT OR REPLACE INTO working_memory"
       " (session_id, key, value, category, created_at, updated_at, expires_at)"
       " VALUES (?, ?, ?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char expires[32];
   compute_expires(ttl_seconds, expires, sizeof(expires));

   if (!category || !category[0])
      category = "general";

   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, value, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, category, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_TRANSIENT);
   if (expires[0])
      sqlite3_bind_text(stmt, 7, expires, -1, SQLITE_TRANSIENT);
   else
      sqlite3_bind_null(stmt, 7);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_wm_get(const char *session_id, const char *key, wm_entry_t *out)
{
   if (!session_id || !key || !out)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql =
       "SELECT id, session_id, key, value, category, created_at, updated_at, expires_at"
       " FROM working_memory"
       " WHERE session_id = ? AND key = ? AND (expires_at IS NULL OR expires_at > ?)";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, ts, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   int result = -1;
   if (rc == SQLITE_ROW)
   {
      row_to_wm(stmt, out);
      result = 0;
   }
   sqlite3_finalize(stmt);
   return result;
}

int db1_wm_list(const char *session_id, const char *category, wm_entry_t *out, int max)
{
   if (!session_id || !out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   const char *sql_all =
       "SELECT id, session_id, key, value, category, created_at, updated_at, expires_at"
       " FROM working_memory"
       " WHERE session_id = ? AND (expires_at IS NULL OR expires_at > ?)"
       " ORDER BY updated_at DESC";
   const char *sql_cat =
       "SELECT id, session_id, key, value, category, created_at, updated_at, expires_at"
       " FROM working_memory"
       " WHERE session_id = ? AND (expires_at IS NULL OR expires_at > ?) AND category = ?"
       " ORDER BY updated_at DESC";

   char ts[32];
   now_utc(ts, sizeof(ts));

   sqlite3_stmt *stmt = NULL;
   if (category && category[0])
   {
      if (sqlite3_prepare_v2(db, sql_cat, -1, &stmt, NULL) != SQLITE_OK)
         return 0;
      sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, category, -1, SQLITE_STATIC);
   }
   else
   {
      if (sqlite3_prepare_v2(db, sql_all, -1, &stmt, NULL) != SQLITE_OK)
         return 0;
      sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_TRANSIENT);
   }

   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      row_to_wm(stmt, &out[count]);
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}

int db1_wm_search_session_ids(const char *query, char out[][WM_SESSION_ID_LEN], int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   static const char *sql = "SELECT DISTINCT session_id FROM working_memory"
                            " WHERE key LIKE ? OR value LIKE ?"
                            " ORDER BY session_id DESC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   char pattern[WM_MAX_VALUE_LEN];
   snprintf(pattern, sizeof(pattern), "%%%s%%", query);
   sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, max);

   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *sid = sqlite3_column_text(stmt, 0);
      snprintf(out[count], WM_SESSION_ID_LEN, "%s", sid ? (const char *)sid : "");
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}

int db1_wm_delete(const char *session_id, const char *key)
{
   if (!session_id || !key)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql = "DELETE FROM working_memory WHERE session_id = ? AND key = ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_wm_clear(const char *session_id)
{
   if (!session_id)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql = "DELETE FROM working_memory WHERE session_id = ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_STATIC);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_wm_gc(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   char ts[32];
   now_utc(ts, sizeof(ts));

   sqlite3_stmt *cs = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT COUNT(*) FROM working_memory"
                          " WHERE expires_at IS NOT NULL AND expires_at <= ?",
                          -1, &cs, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(cs, 1, ts, -1, SQLITE_TRANSIENT);
   int removed = 0;
   if (sqlite3_step(cs) == SQLITE_ROW)
      removed = sqlite3_column_int(cs, 0);
   sqlite3_finalize(cs);
   if (removed == 0)
      return 0;

   sqlite3_stmt *ds = NULL;
   if (sqlite3_prepare_v2(db,
                          "DELETE FROM working_memory"
                          " WHERE expires_at IS NOT NULL AND expires_at <= ?",
                          -1, &ds, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(ds, 1, ts, -1, SQLITE_TRANSIENT);
   sqlite3_step(ds);
   sqlite3_finalize(ds);
   return removed;
}

char *db1_wm_assemble_context(const char *session_id)
{
   if (!session_id)
      return NULL;

   wm_entry_t entries[WM_MAX_RESULTS];
   int count = db1_wm_list(session_id, NULL, entries, WM_MAX_RESULTS);
   if (count == 0)
      return NULL;

   size_t buf_size = 64;
   for (int i = 0; i < count; i++)
      buf_size +=
          strlen(entries[i].category) + strlen(entries[i].key) + strlen(entries[i].value) + 16;

   char *buf = malloc(buf_size);
   if (!buf)
      return NULL;

   int pos = snprintf(buf, buf_size, "## Working Memory\n");
   for (int i = 0; i < count; i++)
   {
      pos += snprintf(buf + pos, buf_size - (size_t)pos, "[%s] %s: %s\n", entries[i].category,
                      entries[i].key, entries[i].value);
   }

   return buf;
}
