/* db1_init.c: DB1 tier lifecycle. Opens the SQLite connection, applies
 * the DB1 schema, and holds the connection as module-private state.
 *
 * Thread-safety: the connection is opened with SQLITE_OPEN_FULLMUTEX so
 * concurrent calls from multiple threads serialize internally. Single
 * connection per process; fork() invalidates it (forked children must
 * call db1_init again). */

#include "db1.h"
#include "db1_internal.h"
#include "db_schema.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sqlite3.h>

static sqlite3 *g_conn = NULL;
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

#define DB1_BUSY_MAX_RETRIES 15

static int db1_busy_cb(void *arg, int count)
{
   (void)arg;
   if (count >= DB1_BUSY_MAX_RETRIES)
      return 0;
   /* Random jitter 20-150 ms; same range as http_retry.c */
   long jitter_ns = (20L + (long)(rand() % 130)) * 1000000L;
   struct timespec ts = {0, jitter_ns};
   nanosleep(&ts, NULL);
   return 1;
}

static void db1_install_busy_handler(sqlite3 *db)
{
   sqlite3_busy_handler(db, db1_busy_cb, NULL);
}

int db1_init(const char *path)
{
   if (!path || !path[0])
      return -1;

   pthread_mutex_lock(&g_init_lock);
   if (g_conn)
   {
      pthread_mutex_unlock(&g_init_lock);
      return 0; /* already initialized */
   }

   int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
   int rc = sqlite3_open_v2(path, &g_conn, flags, NULL);
   if (rc != SQLITE_OK)
   {
      if (g_conn)
      {
         sqlite3_close(g_conn);
         g_conn = NULL;
      }
      pthread_mutex_unlock(&g_init_lock);
      return -1;
   }

   db1_install_busy_handler(g_conn);
   sqlite3_exec(g_conn, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
   sqlite3_exec(g_conn, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
   sqlite3_exec(g_conn, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

   char err[512] = {0};
   if (db1_apply_schema_sqlite(g_conn, err, sizeof(err)) != 0)
   {
      fprintf(stderr, "db1_init: schema apply failed: %s\n", err[0] ? err : "(no message)");
      sqlite3_close(g_conn);
      g_conn = NULL;
      pthread_mutex_unlock(&g_init_lock);
      return -1;
   }
   db1_reconcile_columns(g_conn);

   pthread_mutex_unlock(&g_init_lock);
   return 0;
}

void db1_shutdown(void)
{
   pthread_mutex_lock(&g_init_lock);
   if (g_conn)
   {
      sqlite3_close(g_conn);
      g_conn = NULL;
   }
   pthread_mutex_unlock(&g_init_lock);
}

int db1_is_initialized(void)
{
   return g_conn != NULL;
}

void db1_apply_server_pragmas(void)
{
   if (!g_conn)
      return;
   sqlite3_exec(g_conn, "PRAGMA wal_autocheckpoint=1000", NULL, NULL, NULL);
   sqlite3_exec(g_conn, "PRAGMA cache_size=-8192", NULL, NULL, NULL);   /* 8MB */
   sqlite3_exec(g_conn, "PRAGMA mmap_size=67108864", NULL, NULL, NULL); /* 64MB */
}

sqlite3 *db1_conn(void)
{
   /* No lock — g_conn is stable after db1_init returns, nulled only on
    * shutdown. Callers that call after shutdown get NULL and return an
    * error at the domain-function layer. */
   return g_conn;
}

/* See db1_internal.h. One process-wide gate: explicit transactions on the
 * shared connection are mutually exclusive across threads. */
static pthread_mutex_t g_txn_gate = PTHREAD_MUTEX_INITIALIZER;

int db1_txn_begin(sqlite3 *db, const char *begin_sql)
{
   if (!db)
      return -1;
   pthread_mutex_lock(&g_txn_gate);
   if (sqlite3_exec(db, begin_sql, NULL, NULL, NULL) != SQLITE_OK)
   {
      pthread_mutex_unlock(&g_txn_gate);
      return -1;
   }
   return 0;
}

int db1_txn_end(sqlite3 *db, const char *end_sql)
{
   int ok = db && sqlite3_exec(db, end_sql, NULL, NULL, NULL) == SQLITE_OK;
   pthread_mutex_unlock(&g_txn_gate);
   return ok ? 0 : -1;
}
