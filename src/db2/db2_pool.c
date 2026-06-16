/* db2_pool.c: bounded DB2 connection pool. See db2_pool.h. */
#include "db2_pool.h"
#include "db_postgres.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Reaper cadence + the hold ceiling past which a still-leased connection is
 * logged as stuck (a live thread that missed its lease_end — NOT reclaimed,
 * since the thread may still use it; dead threads are handled by the lease
 * thread-local destructor in db2_init.c). */
#define POOL_LOG(fmt, ...) fprintf(stderr, "aimee: db2.pool: " fmt "\n", ##__VA_ARGS__)

#define DB2_POOL_REAP_INTERVAL_S 30
#define DB2_POOL_REOPEN_MIN_MS   5000 /* rate-cap poison reopen per member */

typedef struct
{
   void *conn;          /* opaque PGconn* via aimee_pg_* */
   int leased;          /* 1 while checked out */
   pthread_t owner;     /* owner thread while leased */
   int64_t lease_ms;    /* CLOCK_MONOTONIC ms at lease time */
   int64_t last_reopen; /* CLOCK_MONOTONIC ms of last poison-reopen */
} db2_pool_member_t;

static db2_pool_member_t g_members[DB2_POOL_MAX_SIZE];
static int g_size = 0;
static char g_url[512];
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static int g_active = 0;
static int g_waiters = 0;
static long g_grants = 0, g_timeouts = 0, g_stuck = 0, g_poisoned = 0;
static int64_t g_hold_ceiling_ms = 300000; /* settable for tests */
static pthread_t g_reaper_thread;
static volatile int g_reaper_stop = 0;
static int g_reaper_running = 0;

static void *db2_pool_reaper_main(void *arg);

/* Reset session state before a connection returns to the pool. ROLLBACK first
 * (exits any open/aborted txn; harmless warning otherwise) so DISCARD ALL —
 * which cannot run in a txn — works; RESET SESSION AUTHORIZATION (DISCARD ALL
 * does NOT undo it); then DISCARD ALL (= RESET ALL + DEALLOCATE ALL + CLOSE ALL
 * + UNLISTEN * + DISCARD TEMP/PLANS/SEQUENCES). Returns 0 clean, -1 -> poison. */
static int member_reset_real(void *conn)
{
   char e[256];
   (void)aimee_pg_exec(conn, "ROLLBACK", e, sizeof(e)); /* best-effort */
   if (aimee_pg_exec(conn, "RESET SESSION AUTHORIZATION", e, sizeof(e)) != 0)
      return -1;
   if (aimee_pg_exec(conn, "DISCARD ALL", e, sizeof(e)) != 0)
      return -1;
   return 0;
}

/* Connection ops, overridable for unit tests (the test harness shims
 * aimee_pg_* to sqlite, which rejects Postgres-only reset SQL). Production uses
 * the real libpq-backed ops. */
static void *(*g_open)(const char *, char *, size_t) = aimee_pg_open;
static void (*g_close)(void *) = aimee_pg_close;
static int (*g_reset)(void *) = member_reset_real;

void db2_pool_set_test_ops(void *(*open_fn)(const char *, char *, size_t), void (*close_fn)(void *),
                           int (*reset_fn)(void *))
{
   g_open = open_fn ? open_fn : aimee_pg_open;
   g_close = close_fn ? close_fn : aimee_pg_close;
   g_reset = reset_fn ? reset_fn : member_reset_real;
}

static int64_t mono_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int db2_pool_init(const char *libpq_url, int size, char *errbuf, size_t errbuf_len)
{
   if (!libpq_url || !libpq_url[0])
   {
      if (errbuf && errbuf_len)
         snprintf(errbuf, errbuf_len, "db2_pool: empty libpq url");
      return -1;
   }
   pthread_mutex_lock(&g_mtx);
   if (g_active)
   {
      int same = (strcmp(g_url, libpq_url) == 0);
      pthread_mutex_unlock(&g_mtx);
      return same ? 0 : -1;
   }
   (void)errbuf;
   (void)errbuf_len;
   if (size < 1)
      size = 1;
   if (size > DB2_POOL_MAX_SIZE)
      size = DB2_POOL_MAX_SIZE;
   snprintf(g_url, sizeof(g_url), "%s", libpq_url);
   /* Counters are per-pool-lifetime. */
   g_grants = g_timeouts = g_stuck = g_poisoned = 0;
   /* Lazy pool: members are opened on first lease, up to `size`. Keeps db2_init
    * cheap (it opens only its own connection) and avoids holding idle
    * connections the deployment never needs. */
   for (int i = 0; i < size; i++)
   {
      g_members[i].conn = NULL;
      g_members[i].leased = 0;
      g_members[i].lease_ms = 0;
      g_members[i].last_reopen = 0;
   }
   g_size = size; /* the cap */
   g_active = 1;
   pthread_mutex_unlock(&g_mtx);

   g_reaper_stop = 0;
   if (pthread_create(&g_reaper_thread, NULL, db2_pool_reaper_main, NULL) == 0)
      g_reaper_running = 1;
   else
      POOL_LOG("reaper thread failed to start; leak-reclaim disabled");

   POOL_LOG("initialized: up to %d connections (opened on demand)", size);
   return 0;
}

int db2_pool_active(void)
{
   pthread_mutex_lock(&g_mtx);
   int a = g_active;
   pthread_mutex_unlock(&g_mtx);
   return a;
}

void *db2_pool_lease(int timeout_ms)
{
   if (timeout_ms <= 0)
      timeout_ms = DB2_POOL_DEFAULT_LEASE_TIMEOUT_MS;
   struct timespec deadline;
   clock_gettime(CLOCK_REALTIME, &deadline);
   deadline.tv_sec += timeout_ms / 1000;
   deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
   if (deadline.tv_nsec >= 1000000000L)
   {
      deadline.tv_sec += 1;
      deadline.tv_nsec -= 1000000000L;
   }

   pthread_mutex_lock(&g_mtx);
   for (;;)
   {
      if (!g_active)
      {
         pthread_mutex_unlock(&g_mtx);
         return NULL;
      }
      /* 1) hand out an already-open free member. */
      for (int i = 0; i < g_size; i++)
      {
         if (!g_members[i].leased && g_members[i].conn)
         {
            g_members[i].leased = 1;
            g_members[i].owner = pthread_self();
            g_members[i].lease_ms = mono_ms();
            g_grants++;
            void *c = g_members[i].conn;
            pthread_mutex_unlock(&g_mtx);
            return c;
         }
      }
      /* 2) grow: open an unused slot on demand (under the lock — warm-up only,
       * at most `size` times for the pool's life). */
      int slot = -1;
      for (int i = 0; i < g_size; i++)
         if (!g_members[i].conn && !g_members[i].leased)
         {
            slot = i;
            break;
         }
      if (slot >= 0)
      {
         char e[256] = "";
         void *c = g_open(g_url, e, sizeof(e));
         if (c)
         {
            g_members[slot].conn = c;
            g_members[slot].leased = 1;
            g_members[slot].owner = pthread_self();
            g_members[slot].lease_ms = mono_ms();
            g_grants++;
            pthread_mutex_unlock(&g_mtx);
            return c;
         }
         POOL_LOG("on-demand member open failed: %s", e[0] ? e : "unknown");
         /* fall through to the bounded wait rather than busy-retrying open */
      }
      /* 3) at the cap and all leased -> bounded wait. */
      g_waiters++;
      int rc = pthread_cond_timedwait(&g_cond, &g_mtx, &deadline);
      g_waiters--;
      if (rc == ETIMEDOUT)
      {
         g_timeouts++;
         pthread_mutex_unlock(&g_mtx);
         POOL_LOG("lease timeout (%d ms); pool exhausted (cap=%d)", timeout_ms, g_size);
         return NULL;
      }
   }
}

/* Find the member index for a leased conn; -1 if not found. Caller holds g_mtx. */
static int member_index_locked(void *conn)
{
   for (int i = 0; i < g_size; i++)
      if (g_members[i].conn == conn)
         return i;
   return -1;
}

void db2_pool_return(void *conn)
{
   if (!conn)
      return;
   /* Reset OUTSIDE the lock (it does I/O). The member stays marked leased so no
    * one else can grab it mid-reset. */
   int poisoned = (g_reset(conn) != 0);

   pthread_mutex_lock(&g_mtx);
   int i = member_index_locked(conn);
   if (i < 0)
   {
      /* Not ours (already replaced by a reaper poison, or stale). Close it. */
      pthread_mutex_unlock(&g_mtx);
      if (poisoned)
         g_close(conn);
      return;
   }
   if (poisoned)
   {
      int64_t now = mono_ms();
      g_close(g_members[i].conn);
      g_members[i].conn = NULL;
      char e[256] = "";
      /* Rate-cap reopen to avoid thrash under a bad-connection storm. */
      if (now - g_members[i].last_reopen >= DB2_POOL_REOPEN_MIN_MS)
      {
         g_members[i].conn = g_open(g_url, e, sizeof(e));
         g_members[i].last_reopen = now;
      }
      g_poisoned++;
      if (!g_members[i].conn)
         POOL_LOG("member %d poisoned; reopen deferred/failed: %s", i, e);
   }
   g_members[i].leased = 0;
   g_members[i].lease_ms = 0;
   pthread_cond_signal(&g_cond);
   pthread_mutex_unlock(&g_mtx);
}

/* Observability sweep. Reclaim-on-thread-death is handled reliably by the
 * lease thread-local destructor (db2_init.c, WP-B) — by the time a thread is
 * dead its lease has already been returned. A lease still held past the ceiling
 * therefore means a *live* thread that missed its db2_lease_end(); we CANNOT
 * safely reclaim it (the thread may still use the connection — libpq forbids
 * concurrent use), so we log + count it for ops. Returns the number of stuck
 * leases observed this sweep. */
int db2_pool_reaper_sweep(void)
{
   int stuck = 0;
   pthread_mutex_lock(&g_mtx);
   int64_t now = mono_ms();
   for (int i = 0; i < g_size; i++)
   {
      if (!g_members[i].leased || !g_members[i].conn || !g_members[i].lease_ms)
         continue;
      if (now - g_members[i].lease_ms > g_hold_ceiling_ms)
      {
         stuck++;
         g_stuck++;
         POOL_LOG("member %d leased %lldms (> %lldms ceiling) — missed lease_end?", i,
                  (long long)(now - g_members[i].lease_ms), (long long)g_hold_ceiling_ms);
      }
   }
   pthread_mutex_unlock(&g_mtx);
   return stuck;
}

static void *db2_pool_reaper_main(void *arg)
{
   (void)arg;
   while (!g_reaper_stop)
   {
      for (int s = 0; s < DB2_POOL_REAP_INTERVAL_S && !g_reaper_stop; s++)
         sleep(1);
      if (g_reaper_stop)
         break;
      (void)db2_pool_reaper_sweep();
   }
   return NULL;
}

void db2_pool_shutdown(void)
{
   if (g_reaper_running)
   {
      g_reaper_stop = 1;
      pthread_join(g_reaper_thread, NULL);
      g_reaper_running = 0;
   }
   pthread_mutex_lock(&g_mtx);
   for (int i = 0; i < g_size; i++)
   {
      if (g_members[i].conn)
         g_close(g_members[i].conn);
      g_members[i].conn = NULL;
      g_members[i].leased = 0;
   }
   g_size = 0;
   g_active = 0;
   pthread_mutex_unlock(&g_mtx);
}

void db2_pool_set_test_ceiling_ms(int ceiling_ms)
{
   pthread_mutex_lock(&g_mtx);
   g_hold_ceiling_ms = ceiling_ms > 0 ? ceiling_ms : 300000;
   pthread_mutex_unlock(&g_mtx);
}

void db2_pool_stats(int *size, int *in_use, int *waiters, long *lease_grants, long *lease_timeouts,
                    long *stuck, long *poisoned)
{
   pthread_mutex_lock(&g_mtx);
   int used = 0;
   for (int i = 0; i < g_size; i++)
      if (g_members[i].leased)
         used++;
   if (size)
      *size = g_size;
   if (in_use)
      *in_use = used;
   if (waiters)
      *waiters = g_waiters;
   if (lease_grants)
      *lease_grants = g_grants;
   if (lease_timeouts)
      *lease_timeouts = g_timeouts;
   if (stuck)
      *stuck = g_stuck;
   if (poisoned)
      *poisoned = g_poisoned;
   pthread_mutex_unlock(&g_mtx);
}
