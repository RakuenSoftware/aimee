/* db.c: DB1 SQLite low-level helpers — prepared statement cache
 * (FNV-1a keyed), pragma profiles, and the FTS5 capability check.
 * Path-based maintenance (backup, check, recover) lives in
 * maintenance.c; the schema bootstrap lives in db_schema.c. */
#include "db.h"
#include "aimee.h"
#include "log.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
/* --- Prepared statement cache (FNV-1a hash keyed) --- */
typedef struct
{
   uint32_t hash;
   sqlite3 *db; /* the connection this stmt belongs to */
   sqlite3_stmt *stmt;
} stmt_entry_t;

static stmt_entry_t stmt_cache[MAX_STMT_CACHE];
static int stmt_cache_count = 0;
static pthread_mutex_t stmt_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t fnv1a(const char *s)
{
   uint32_t h = 2166136261u;
   for (; *s; s++)
      h = (h ^ (uint8_t)*s) * 16777619u;
   return h;
}
/* Statement cache: keyed by SQL string hash + db pointer.
 * Threading model: the cache mutex protects lookup/insertion, but the returned
 * statement is used without holding the lock. This is safe because each server
 * request uses its own db connection (and thus its own cached statements).
 * Do NOT share a db connection across threads with this cache. */
sqlite3_stmt *db1_prepare(sqlite3 *db, const char *sql)
{
   uint32_t h = fnv1a(sql);
   pthread_mutex_lock(&stmt_cache_mutex);
   for (int i = 0; i < stmt_cache_count; i++)
   {
      if (stmt_cache[i].hash == h && stmt_cache[i].db == db &&
          strcmp(sqlite3_sql(stmt_cache[i].stmt), sql) == 0)
      {
         sqlite3_reset(stmt_cache[i].stmt);
         sqlite3_clear_bindings(stmt_cache[i].stmt);
         pthread_mutex_unlock(&stmt_cache_mutex);
         return stmt_cache[i].stmt;
      }
   }
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
   {
      pthread_mutex_unlock(&stmt_cache_mutex);
      LOG_ERROR("db", "db1_prepare: %s  SQL: %.200s", sqlite3_errmsg(db), sql);
      return NULL;
   }
   if (stmt_cache_count < MAX_STMT_CACHE)
   {
      stmt_cache[stmt_cache_count].hash = h;
      stmt_cache[stmt_cache_count].db = db;
      stmt_cache[stmt_cache_count].stmt = stmt;
      stmt_cache_count++;
   }
   pthread_mutex_unlock(&stmt_cache_mutex);
   return stmt;
}
void db1_stmt_cache_clear(void)
{
   pthread_mutex_lock(&stmt_cache_mutex);
   for (int i = 0; i < stmt_cache_count; i++)
   {
      if (stmt_cache[i].stmt)
         sqlite3_finalize(stmt_cache[i].stmt);
      stmt_cache[i].stmt = NULL;
      stmt_cache[i].hash = 0;
      stmt_cache[i].db = NULL;
   }
   stmt_cache_count = 0;
   pthread_mutex_unlock(&stmt_cache_mutex);
}
void db1_stmt_cache_clear_for_sqlite(sqlite3 *db)
{
   pthread_mutex_lock(&stmt_cache_mutex);
   int write_idx = 0;
   for (int i = 0; i < stmt_cache_count; i++)
   {
      if (stmt_cache[i].db == db)
      {
         if (stmt_cache[i].stmt)
            sqlite3_finalize(stmt_cache[i].stmt);
      }
      else
      {
         if (write_idx != i)
            stmt_cache[write_idx] = stmt_cache[i];
         write_idx++;
      }
   }
   for (int i = write_idx; i < stmt_cache_count; i++)
   {
      stmt_cache[i].stmt = NULL;
      stmt_cache[i].hash = 0;
      stmt_cache[i].db = NULL;
   }
   stmt_cache_count = write_idx;
   pthread_mutex_unlock(&stmt_cache_mutex);
}
/* --- FTS5 check --- */
int db1_fts5_available(sqlite3 *db)
{
   sqlite3_stmt *stmt = NULL;
   int found = 0;
   if (sqlite3_prepare_v2(db, "PRAGMA compile_options", -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *opt = (const char *)sqlite3_column_text(stmt, 0);
      if (opt && strcmp(opt, "ENABLE_FTS5") == 0)
      {
         found = 1;
         break;
      }
   }
   sqlite3_finalize(stmt);
   return found;
}

/* --- Pragma profiles --- */

void db1_apply_pragmas(sqlite3 *db, db_mode_t mode)
{
   /* Common pragmas for all modes */
   sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
   sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
   sqlite3_exec(db, "PRAGMA wal_autocheckpoint=1000", NULL, NULL, NULL);

   if (mode == DB_MODE_SERVER)
   {
      /* Server: long-lived, concurrent queries, trades crash durability for throughput.
       * Lower busy timeout since server connections use transactions and retry at
       * a higher level. */
      sqlite3_busy_timeout(db, 5000);
      sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
      sqlite3_exec(db, "PRAGMA cache_size=-8192", NULL, NULL, NULL);   /* 8MB */
      sqlite3_exec(db, "PRAGMA mmap_size=67108864", NULL, NULL, NULL); /* 64MB */
   }
   else
   {
      /* CLI: short-lived, full durability. Higher busy timeout because multiple
       * concurrent sessions (hooks, agents) contend on the same DB file and CLI
       * callers can afford to block. */
      sqlite3_busy_timeout(db, 15000);
      sqlite3_exec(db, "PRAGMA synchronous=FULL", NULL, NULL, NULL);
      sqlite3_exec(db, "PRAGMA cache_size=-2048", NULL, NULL, NULL); /* 2MB */
   }
}
