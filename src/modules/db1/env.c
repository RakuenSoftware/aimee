/* db1/env.c: user-local environment state — env_capabilities and
 * maintenance_state. */

#include "env.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* --- env_capabilities --- */

int db1_env_capability_set(const char *key, const char *value)
{
   if (!key)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "INSERT OR REPLACE INTO env_capabilities (key, value, detected_at)"
                            " VALUES (?, ?, datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, value ? value : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_env_capability_get(const char *key, char *value_out, size_t value_len,
                           char *detected_at_out, size_t detected_at_len)
{
   if (value_out && value_len > 0)
      value_out[0] = '\0';
   if (detected_at_out && detected_at_len > 0)
      detected_at_out[0] = '\0';
   if (!key || !key[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT value, detected_at FROM env_capabilities WHERE key = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW)
   {
      const unsigned char *v = sqlite3_column_text(stmt, 0);
      const unsigned char *t = sqlite3_column_text(stmt, 1);
      if (value_out && value_len > 0)
         snprintf(value_out, value_len, "%s", v ? (const char *)v : "");
      if (detected_at_out && detected_at_len > 0)
         snprintf(detected_at_out, detected_at_len, "%s", t ? (const char *)t : "");
      sqlite3_finalize(stmt);
      return 1;
   }
   sqlite3_finalize(stmt);
   if (rc == SQLITE_DONE)
      return 0;
   return -1;
}

int db1_env_capability_list(db1_env_capability_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT key, value, detected_at FROM env_capabilities ORDER BY key";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *k = sqlite3_column_text(stmt, 0);
      const unsigned char *v = sqlite3_column_text(stmt, 1);
      const unsigned char *t = sqlite3_column_text(stmt, 2);
      snprintf(out[count].key, sizeof(out[count].key), "%s", k ? (const char *)k : "");
      snprintf(out[count].value, sizeof(out[count].value), "%s", v ? (const char *)v : "");
      snprintf(out[count].detected_at, sizeof(out[count].detected_at), "%s",
               t ? (const char *)t : "");
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}

/* --- maintenance_state --- */

int db1_maintenance_state_load(const char *key, db1_maintenance_state_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!key)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT last_run_at, last_memory_count, last_changes, last_elapsed_ms,"
                            "       last_summary_json FROM maintenance_state WHERE key = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      out->present = 1;
      const unsigned char *ts = sqlite3_column_text(stmt, 0);
      snprintf(out->last_run_at, sizeof(out->last_run_at), "%s", ts ? (const char *)ts : "");
      out->last_memory_count = sqlite3_column_int64(stmt, 1);
      out->last_changes = sqlite3_column_int(stmt, 2);
      out->last_elapsed_ms = sqlite3_column_double(stmt, 3);
      const unsigned char *js = sqlite3_column_text(stmt, 4);
      snprintf(out->last_summary_json, sizeof(out->last_summary_json), "%s",
               js ? (const char *)js : "");
   }
   sqlite3_finalize(stmt);
   return 0;
}

int db1_maintenance_state_save(const char *key, const db1_maintenance_state_t *st)
{
   if (!key || !st)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "INSERT INTO maintenance_state"
                            "  (key, last_run_at, last_memory_count, last_changes, last_elapsed_ms,"
                            "   last_summary_json)"
                            " VALUES (?, ?, ?, ?, ?, ?)"
                            " ON CONFLICT(key) DO UPDATE SET"
                            "   last_run_at = excluded.last_run_at,"
                            "   last_memory_count = excluded.last_memory_count,"
                            "   last_changes = excluded.last_changes,"
                            "   last_elapsed_ms = excluded.last_elapsed_ms,"
                            "   last_summary_json = excluded.last_summary_json";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, st->last_run_at, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 3, st->last_memory_count);
   sqlite3_bind_int(stmt, 4, st->last_changes);
   sqlite3_bind_double(stmt, 5, st->last_elapsed_ms);
   sqlite3_bind_text(stmt, 6, st->last_summary_json, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}
