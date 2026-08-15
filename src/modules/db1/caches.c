/* db1/caches.c: user-local runtime caches (context cache, context snapshots,
 * agent cache). */

#include "caches.h"
#include "db1_internal.h"
#include "aimee.h" /* CACHE_TTL_SECONDS */

#include <stdint.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Context cache --- */

int db1_context_cache_get(const char *hash, char *out, size_t out_len)
{
   if (!hash || !out || out_len == 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT output FROM context_cache WHERE hash = ?"
                            " AND created_at > datetime('now', '-' || ? || ' seconds')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   char ttl[16];
   snprintf(ttl, sizeof(ttl), "%d", CACHE_TTL_SECONDS);
   sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, ttl, -1, SQLITE_TRANSIENT);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *val = sqlite3_column_text(stmt, 0);
      if (val)
      {
         snprintf(out, out_len, "%s", (const char *)val);
         rc = 0;
      }
   }
   sqlite3_finalize(stmt);
   return rc;
}

void db1_context_cache_put(const char *hash, const char *output)
{
   if (!hash || !output)
      return;
   sqlite3 *db = db1_conn();
   if (!db)
      return;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "INSERT OR REPLACE INTO context_cache (hash, output, created_at)"
                            " VALUES (?, ?, datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return;

   sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, output, -1, SQLITE_TRANSIENT);
   (void)sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

void db1_context_cache_invalidate(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return;
   (void)sqlite3_exec(db, "DELETE FROM context_cache", NULL, NULL, NULL);
}

/* --- Context snapshots --- */

int db1_context_snapshot_insert(const char *session_id, int64_t memory_id, double relevance_score)
{
   if (!session_id || !session_id[0] || memory_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "INSERT INTO context_snapshots (session_id, memory_id, relevance_score)"
                            " VALUES (?, ?, ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 2, memory_id);
   sqlite3_bind_double(stmt, 3, relevance_score);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

int db1_context_snapshot_count_memories_with_min_samples(int min_samples)
{
   if (min_samples <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT COUNT(*) FROM ("
                            " SELECT memory_id FROM context_snapshots"
                            " GROUP BY memory_id HAVING COUNT(*) >= ?"
                            " )";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   sqlite3_bind_int(stmt, 1, min_samples);
   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

int db1_context_snapshot_list_memory_ids_with_min_samples(int min_samples, int64_t *out, int max)
{
   if (min_samples <= 0 || !out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT memory_id FROM context_snapshots"
                            " GROUP BY memory_id HAVING COUNT(*) >= ?"
                            " ORDER BY memory_id";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   sqlite3_bind_int(stmt, 1, min_samples);
   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
      out[count++] = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

int db1_context_snapshot_count_for_memory(int64_t memory_id)
{
   if (memory_id <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT COUNT(*) FROM context_snapshots WHERE memory_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   sqlite3_bind_int64(stmt, 1, memory_id);
   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return count;
}

int db1_context_snapshot_list_sessions_for_memory(int64_t memory_id,
                                                  char (*out)[DB1_CONTEXT_SNAPSHOT_SESSION_LEN],
                                                  int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT session_id FROM context_snapshots"
                            " WHERE memory_id = ? ORDER BY id";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   sqlite3_bind_int64(stmt, 1, memory_id);
   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *session_id = sqlite3_column_text(stmt, 0);
      snprintf(out[count], DB1_CONTEXT_SNAPSHOT_SESSION_LEN, "%s",
               session_id ? (const char *)session_id : "");
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}

int db1_context_snapshot_has_memory(int64_t memory_id)
{
   if (memory_id <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT 1 FROM context_snapshots WHERE memory_id = ? LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   sqlite3_bind_int64(stmt, 1, memory_id);
   int found = (sqlite3_step(stmt) == SQLITE_ROW);
   sqlite3_finalize(stmt);
   return found;
}

/* --- Agent result cache --- */

char *db1_agent_cache_get(const char *role, const char *prompt)
{
   if (!role || !prompt)
      return NULL;
   sqlite3 *db = db1_conn();
   if (!db)
      return NULL;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT result FROM agent_cache WHERE role = ? AND prompt = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return NULL;

   sqlite3_bind_text(stmt, 1, role, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, prompt, -1, SQLITE_TRANSIENT);

   char *copy = NULL;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *val = sqlite3_column_text(stmt, 0);
      if (val)
         copy = strdup((const char *)val);
   }
   sqlite3_finalize(stmt);
   return copy;
}

void db1_agent_cache_put(const char *role, const char *prompt, const char *result)
{
   if (!role || !prompt || !result)
      return;
   sqlite3 *db = db1_conn();
   if (!db)
      return;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "INSERT OR REPLACE INTO agent_cache (role, prompt, result)"
                            " VALUES (?, ?, ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return;

   sqlite3_bind_text(stmt, 1, role, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, prompt, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, result, -1, SQLITE_TRANSIENT);
   (void)sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}
