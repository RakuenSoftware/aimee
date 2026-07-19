/* db2_init.c: DB2 tier lifecycle. Opens the Postgres connection,
 * applies the DB2 schema, and holds the connection as module-private
 * state.
 *
 * Thread-safety: a single process-global connection pointer is guarded
 * by a mutex during init/shutdown. Callers outside src/db2/ never see
 * the libpq handle; future DB2 subsystems fetch it through db2_conn().
 */

#include "db2.h"
#include "db2_internal.h"

#include "db2_pool.h"
#include "db_postgres.h"
#include "db_schema.h"
#include "../headers/log.h" /* LOG_WARN */
#include "entity_edges.h"
#include "eval_support.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h> /* getpid (eval temp-store schema name) */
#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
typedef struct sqlite3 sqlite3;
#else
#include <sqlite3.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h> /* §2b: CLOCK_MONOTONIC poll budget + nanosleep between lock polls */

#if !defined(AIMEE_DISABLE_DB2_SQLITE_SHIM) && (defined(__GNUC__) || defined(__clang__))
#pragma weak sqlite3_close
#pragma weak sqlite3_exec
#pragma weak sqlite3_open
#endif

void db1_stmt_cache_clear_for_sqlite(sqlite3 *db) __attribute__((weak));

#ifndef AIMEE_DISABLE_DB2_SQLITE_SHIM
/* Only the sqlite-shim eval store closes a raw sqlite handle; the libpq build
 * has no such handle, so this would be an unused static there. */
static void db2_maybe_clear_sqlite_cache(sqlite3 *db)
{
   if (!db)
      return;
   if (db1_stmt_cache_clear_for_sqlite)
      db1_stmt_cache_clear_for_sqlite(db);
}
#endif

static void *g_conn = NULL;
static char g_pg_url[512] = "";
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
/* Embedding dimension for the DB2 halfvec columns (one embedder per deployment:
 * 1024 for pplx-0.6b, 2560 for pplx-4b). Set from the loaded config by the
 * server / aimee-kb startup via db2_set_embedding_dim() before db2_init(), so
 * this layer needs no config dependency. 0 = unset -> db2_embedding_dim()
 * reports the 1024 default (the default embedder is pplx-0.6b). */
static int g_embed_dim = 0;

/* §2a: whether the operator pinned the dim. When 0 (default) and nothing was
 * pinned, db2_init prefers a recorded kb_meta.schema_embedding_dim over the
 * default. Reset in db2_shutdown so a reopen / a later test never inherits it. */
static int g_embed_dim_pinned = 0;

void db2_set_embedding_dim(int dim)
{
   g_embed_dim = dim;
}

/* §2c: expose the init mutex so the dim-change reset (db2_reembed.c) serializes its
 * destructive execute + in-memory dim swap against db2_init and any concurrent reset
 * — the two paths that mutate the schema + recorded/in-memory dim together. The
 * accessors only touch g_init_lock; db2_set_embedding_dim/db2_embedding_dim take no
 * lock, so holding it across the swap cannot self-deadlock. */
void db2_init_lock(void)
{
   pthread_mutex_lock(&g_init_lock);
}
void db2_init_unlock(void)
{
   pthread_mutex_unlock(&g_init_lock);
}

int db2_embedding_dim(void)
{
   return g_embed_dim > 0 ? g_embed_dim : 1024;
}

void db2_set_embedding_dim_pinned(int pinned)
{
   g_embed_dim_pinned = pinned ? 1 : 0;
}

/* §2b: the embedder /health probe seam + its wall-clock budget. NULL/0 by default
 * (the §2b path is skipped → behavior identical to §2a). Both reset in
 * db2_shutdown so a reopen / a later test never inherits them. */
static db2_embedder_probe_fn g_embedder_probe = NULL;
static int g_dim_probe_budget_ms = 120000;

void db2_set_embedder_probe(db2_embedder_probe_fn fn)
{
   g_embedder_probe = fn;
}

void db2_set_dim_probe_budget_ms(int ms)
{
   if (ms > 0)
      g_dim_probe_budget_ms = ms;
}

/* §2c: probe the running embedder for its CURRENT output dim via the registered
 * §2b probe. The dim-change reset needs this as its target (after a model swap the
 * running db2_embedding_dim() is still the old/recorded value). Returns 0 + *out on
 * success; -1 if no probe is registered or it fails. */
int db2_probe_embedder_dim(int budget_ms, int *out)
{
   if (out)
      *out = 0;
   if (!g_embedder_probe)
      return -1;
   int dim = 0;
   char err[DB2_PROBE_ERR_LEN] = "";
   if (g_embedder_probe(&dim, budget_ms > 0 ? budget_ms : 8000, err, sizeof(err)) != 0 || dim <= 0)
      return -1;
   if (out)
      *out = dim;
   return 0;
}

/* Model-identity drift guard (unified-llm-container §2). A dim-only guard is
 * insufficient: two different models can share a dim (pplx-embed and the default
 * Qwen3-Embedding-0.6B are BOTH 1024-d), so a same-dim swap would silently mix
 * incompatible vector spaces. These globals carry the configured embedder model
 * identity (repo@sha), the reranker identity + scoring contract, and the
 * compat-list of admitted transitions, set from config before db2_init like the
 * dim above. INVARIANT: set at exactly the sites that call db2_set_embedding_dim()
 * — the serving config-load paths (cmd_core bootstrap_db2, kb_main, cmd_doctor);
 * the connectivity-probe path (bootstrap_db2_try_url) and tests deliberately set
 * neither (a probe shuts down before any serving schema applies). ALL DEFAULT
 * EMPTY: an empty embedder model_id makes the guard a no-op, so a deployment that
 * has not yet adopted the unified container (the live torch embedder reports no
 * identity) is unaffected. */
static char g_embedder_model_id[160] = "";
static char g_reranker_model_id[160] = "";
static char g_reranker_contract[96] = "";
static char g_embedding_compat[1024] = ""; /* CSV of "old_id->new_id" transitions */

void db2_set_embedder_model_id(const char *model_id)
{
   snprintf(g_embedder_model_id, sizeof(g_embedder_model_id), "%s", model_id ? model_id : "");
}

const char *db2_embedder_model_id(void)
{
   return g_embedder_model_id;
}

void db2_set_reranker_identity(const char *model_id, const char *contract)
{
   snprintf(g_reranker_model_id, sizeof(g_reranker_model_id), "%s", model_id ? model_id : "");
   snprintf(g_reranker_contract, sizeof(g_reranker_contract), "%s", contract ? contract : "");
}

const char *db2_reranker_model_id(void)
{
   return g_reranker_model_id;
}

const char *db2_reranker_contract(void)
{
   return g_reranker_contract;
}

void db2_set_embedding_compat(const char *compat_csv)
{
   snprintf(g_embedding_compat, sizeof(g_embedding_compat), "%s", compat_csv ? compat_csv : "");
}

const char *db2_embedding_compat(void)
{
   return g_embedding_compat;
}

int db2_effective_dim(int pinned, int configured, int recorded)
{
   if (pinned)
      return configured; /* operator pin is authoritative */
   if (recorded > 0)
      return recorded; /* recorded wins over the configured default */
   return configured;  /* fresh DB / nothing recorded: the default */
}

/* §2b precedence (pure): pin > recorded > probe > default. */
db2_dim_source_t db2_dim_source(int pinned, int recorded_present, int probe_available)
{
   if (pinned)
      return DB2_DIM_SRC_PIN;
   if (recorded_present)
      return DB2_DIM_SRC_RECORDED;
   if (probe_available)
      return DB2_DIM_SRC_PROBE;
   return DB2_DIM_SRC_DEFAULT;
}

/* §2b: a Postgres advisory lock serialises fresh-DB dim bootstrap across racing kb
 * starts. Keyed by hashtext of this string (the mining.c pattern). Bump the _vN
 * suffix only on a semantics-changing lock change (added waiters / xact-scoped),
 * never on a code refactor — the value is a wire contract between concurrent kbs. */
#define DB2_DIM_LOCK_KEY "aimee_dim_bootstrap_v1"

static long db2_mono_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Acquire the dim-bootstrap advisory lock, polling pg_try_advisory_lock every
 * ~250ms up to budget_ms. Returns 0 = acquired, 1 = timed out (another session
 * holds it for the whole budget), -1 = query error. *elapsed_ms (optional) gets
 * the wall time spent so the caller can subtract it from the probe budget. On the
 * sqlite test shim there is no advisory lock (single-process tests) → acquired. */
static int db2_dim_lock_acquire(void *conn, int budget_ms, int *elapsed_ms)
{
   long start = db2_mono_ms();
   if (aimee_pg_is_shim())
   {
      if (elapsed_ms)
         *elapsed_ms = 0;
      return 0;
   }
   for (;;)
   {
      char err[256] = "";
      /* ::int — pg_try_advisory_lock returns BOOLEAN; aimee_pg_column_int does
       * atoi(PQgetvalue) and atoi("t")==0, so the bool must be cast to 1/0 text or
       * the lock would never read as acquired. */
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, "SELECT pg_try_advisory_lock(hashtext(?1))::int",
                                             err, sizeof(err));
      int got = 0, ok = 0;
      if (st)
      {
         aimee_pg_bind_text(st, "?1", DB2_DIM_LOCK_KEY);
         if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            ok = 1;
            got = aimee_pg_column_int(st, 0) ? 1 : 0;
         }
         aimee_pg_finalize(st);
      }
      int spent = (int)(db2_mono_ms() - start);
      if (elapsed_ms)
         *elapsed_ms = spent;
      if (!ok)
         return -1;
      if (got)
         return 0;
      if (spent >= budget_ms)
         return 1;
      struct timespec ts = {0, 250L * 1000 * 1000};
      nanosleep(&ts, NULL);
   }
}

static void db2_dim_lock_release(void *conn)
{
   if (aimee_pg_is_shim())
      return;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT pg_advisory_unlock(hashtext(?1))", err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", DB2_DIM_LOCK_KEY);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

static pthread_key_t g_thread_conn_key;
static pthread_once_t g_thread_conn_key_once = PTHREAD_ONCE_INIT;
/* The thread that ran db2_init() owns g_conn and uses it directly; every other
 * thread gets its own libpq connection via db2_conn() (see the comment there). */
static pthread_t g_init_thread;
static int g_init_thread_set = 0;

/* A non-init thread's current connection: a pool lease (pooled=1) or, when the
 * pool is exhausted/unavailable, a private overflow connection (pooled=0).
 * Stored in g_thread_conn_key; the destructor returns/closes it on thread exit
 * (this is the reliable reclaim-on-thread-death the pool reaper deliberately
 * leaves to us). g_lease_depth refcounts db2_lease_begin/_end so nested units
 * reuse one lease. */
typedef struct
{
   void *conn;
   int pooled;
} db2_thread_lease_t;

static int g_pool_size = 16; /* set via db2_set_pool_size before db2_init */
static __thread int g_lease_depth = 0;

void db2_set_pool_size(int size)
{
   if (size > 0)
      g_pool_size = size;
}

static void thread_conn_destructor(void *p)
{
   db2_thread_lease_t *L = (db2_thread_lease_t *)p;
   if (!L)
      return;
   if (L->conn)
   {
      if (L->pooled)
         db2_pool_return(L->conn);
      else
         aimee_pg_close(L->conn);
   }
   free(L);
}

static void thread_conn_key_init(void)
{
   pthread_key_create(&g_thread_conn_key, thread_conn_destructor);
}
/* Test-shim sqlite handle. Set by db2_register_shared_sqlite (tests
 * and db2_eval_open_temp_store). Does not own the handle; callers are
 * responsible for lifetime. */
static sqlite3 *g_shared_sqlite = NULL;
static int g_shared_ephemeral = 0;
static pthread_mutex_t g_shared_sqlite_lock = PTHREAD_MUTEX_INITIALIZER;

void db2_register_shared_sqlite(sqlite3 *h)
{
   pthread_mutex_lock(&g_shared_sqlite_lock);
   g_shared_sqlite = h;
   /* Fresh registration: assume non-ephemeral unless the caller opts in
    * via db2_set_ephemeral.  Clearing on re-register avoids leaking a
    * stale flag from a prior test. */
   g_shared_ephemeral = 0;
   pthread_mutex_unlock(&g_shared_sqlite_lock);
}

sqlite3 *db2_shared_sqlite(void)
{
   pthread_mutex_lock(&g_shared_sqlite_lock);
   sqlite3 *h = g_shared_sqlite;
   pthread_mutex_unlock(&g_shared_sqlite_lock);
   return h;
}

void db2_set_ephemeral(int ephemeral)
{
   /* Ephemeral is a TEST/EVAL-only mode (the in-memory sqlite shim). A real
    * libpq instance must never enter it: ephemeral suppresses durable vector
    * writes (db2_vector_index_sync_suppressed), so flipping it on in production
    * would silently stop memory embeddings from persisting. Refuse, fail-safe. */
   if (ephemeral && !aimee_pg_is_shim())
      return;
   pthread_mutex_lock(&g_shared_sqlite_lock);
   g_shared_ephemeral = ephemeral ? 1 : 0;
   pthread_mutex_unlock(&g_shared_sqlite_lock);
}

int db2_is_ephemeral(void)
{
   /* Belt-and-suspenders with db2_set_ephemeral: a real libpq (production)
    * instance is NEVER ephemeral, regardless of the stored flag. Ephemeral
    * only has meaning under the sqlite shim used by tests/evals. */
   if (!aimee_pg_is_shim())
      return 0;
   pthread_mutex_lock(&g_shared_sqlite_lock);
   int v = g_shared_ephemeral;
   pthread_mutex_unlock(&g_shared_sqlite_lock);
   return v;
}

static int db2_query_flag(const char *sql, const char *param_name, const char *param_value)
{
   char errbuf[256] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(g_conn, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;

   if (param_name && aimee_pg_bind_text(stmt, param_name, param_value ? param_value : "") != 0)
   {
      aimee_pg_finalize(stmt);
      return -1;
   }

   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   if (rc == AIMEE_PG_ROW)
      return 1;
   if (rc == AIMEE_PG_DONE)
      return 0;
   return -1;
}

/* Acquire this thread's connection: a pool lease, or — if the pool is
 * unavailable/exhausted — a private overflow connection. Stores it in the
 * thread key so the destructor returns/closes it on thread exit. Caller holds
 * no lock. Returns NULL only if everything fails (then db2_conn falls back to
 * g_conn as a last resort). */
static void *db2_thread_acquire(void)
{
   void *conn = NULL;
   int pooled = 0;
   if (db2_pool_active())
   {
      conn = db2_pool_lease(0); /* bounded; NULL on exhaustion */
      pooled = (conn != NULL);
   }
   if (!conn)
   {
      /* Pool off or exhausted: a private overflow connection keeps the unit of
       * work alive (libpq forbids sharing one PGconn across threads). The
       * WP-D startup check leaves headroom under max_connections for these. */
      char errbuf[256] = "";
      conn = aimee_pg_open(g_pg_url[0] ? g_pg_url : NULL, errbuf, sizeof(errbuf));
      if (!conn)
      {
         fprintf(stderr, "aimee: db2_conn: connection acquire failed (%s)\n",
                 errbuf[0] ? errbuf : "unknown");
         return NULL;
      }
   }
   db2_thread_lease_t *L = (db2_thread_lease_t *)pthread_getspecific(g_thread_conn_key);
   if (!L)
   {
      L = (db2_thread_lease_t *)calloc(1, sizeof(*L));
      if (!L)
      {
         if (pooled)
            db2_pool_return(conn);
         else
            aimee_pg_close(conn);
         return NULL;
      }
      pthread_setspecific(g_thread_conn_key, L);
   }
   L->conn = conn;
   L->pooled = pooled;
   return conn;
}

void *db2_conn(void)
{
   pthread_once(&g_thread_conn_key_once, thread_conn_key_init);
   db2_thread_lease_t *L = (db2_thread_lease_t *)pthread_getspecific(g_thread_conn_key);
   if (L && L->conn)
      return L->conn; /* the thread's current lease (re-entrant) */
   /* The db2_init() owner thread uses its dedicated g_conn directly. */
   if (!g_init_thread_set || pthread_equal(pthread_self(), g_init_thread))
      return g_conn;
   /* Every other thread leases from the pool (lazily; returned on thread exit
    * by the destructor, or sooner via db2_lease_end at a job boundary). */
   void *c = db2_thread_acquire();
   return c ? c : g_conn;
}

void db2_lease_begin(void)
{
   pthread_once(&g_thread_conn_key_once, thread_conn_key_init);
   /* The init thread is never pooled. */
   if (!g_init_thread_set || pthread_equal(pthread_self(), g_init_thread))
      return;
   if (g_lease_depth++ == 0)
   {
      db2_thread_lease_t *L = (db2_thread_lease_t *)pthread_getspecific(g_thread_conn_key);
      if (!L || !L->conn)
         (void)db2_thread_acquire(); /* eager lease for the unit of work */
   }
}

void db2_lease_end(void)
{
   if (!g_init_thread_set || pthread_equal(pthread_self(), g_init_thread))
      return;
   if (g_lease_depth > 0 && --g_lease_depth == 0)
   {
      db2_thread_lease_t *L = (db2_thread_lease_t *)pthread_getspecific(g_thread_conn_key);
      if (L && L->conn)
      {
         if (L->pooled)
            db2_pool_return(L->conn);
         else
            aimee_pg_close(L->conn);
         L->conn = NULL;
         L->pooled = 0;
      }
   }
}

void db2_lease_release_idle(void)
{
   /* Release a connection acquired lazily by db2_conn() OUTSIDE any
    * db2_lease_begin/_end scope (depth 0). Long-lived periodic workers (curator
    * drain, maintenance timer) otherwise pin a pool connection for their whole
    * lifetime — the reaper flags it as a stuck lease and it permanently shrinks
    * the pool. They call this at a job boundary (between cycles) to return the
    * connection while idle. No-op inside a lease scope or on the init thread. */
   if (!g_init_thread_set || pthread_equal(pthread_self(), g_init_thread))
      return;
   if (g_lease_depth != 0)
      return; /* inside an explicit begin/end unit — leave it owned */
   db2_thread_lease_t *L = (db2_thread_lease_t *)pthread_getspecific(g_thread_conn_key);
   if (L && L->conn)
   {
      if (L->pooled)
         db2_pool_return(L->conn);
      else
         aimee_pg_close(L->conn);
      L->conn = NULL;
      L->pooled = 0;
   }
}

void *db2_thread_conn_open(char *errbuf, size_t errlen)
{
   pthread_once(&g_thread_conn_key_once, thread_conn_key_init);
   const char *url = g_pg_url[0] ? g_pg_url : NULL;
   if (!url)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "db2 not initialized");
      return NULL;
   }
   void *conn = aimee_pg_open(url, errbuf, errlen);
   if (conn)
      pthread_setspecific(g_thread_conn_key, conn);
   return conn;
}

void db2_thread_conn_close(void)
{
   pthread_once(&g_thread_conn_key_once, thread_conn_key_init);
   void *conn = pthread_getspecific(g_thread_conn_key);
   if (conn)
   {
      aimee_pg_close(conn);
      pthread_setspecific(g_thread_conn_key, NULL);
   }
}

int db2_is_initialized(void)
{
   return g_conn ? 1 : 0;
}

static const char *db2_postgres_url(void)
{
   return g_pg_url[0] ? g_pg_url : NULL;
}

int db2_fork_conn_url(char *out, size_t cap)
{
   if (!out || cap == 0)
      return 0;
   out[0] = '\0';

   const char *url = db2_postgres_url();
   if (!url || !url[0])
      return 0;
   snprintf(out, cap, "%s", url);
   return 1;
}

int db2_init(const char *libpq_url)
{
   char errbuf[256] = "";

   if (!libpq_url || !libpq_url[0])
      return -1;

   pthread_mutex_lock(&g_init_lock);

   if (g_conn)
   {
      int same_url = (strcmp(g_pg_url, libpq_url) == 0);
      pthread_mutex_unlock(&g_init_lock);
      return same_url ? 0 : -1;
   }

   void *conn = aimee_pg_open(libpq_url, errbuf, sizeof(errbuf));
   if (!conn)
   {
      pthread_mutex_unlock(&g_init_lock);
      return -1;
   }

   /* The deployment runs a single embedder (0.6b=1024 / 4b=2560); the configured
    * embedding_dim drives the dimension of the DB2 halfvec embedding columns.
    * The dimension is supplied by db2_set_embedding_dim() at startup (the server
    * and aimee-kb, which hold the loaded config) so this low-level layer stays
    * config-free; it defaults to 1024 when unset. */
   int configured_dim = db2_embedding_dim();
   /* §2a/§2b precedence: pin > recorded > probe > default. When the operator did
    * NOT pin, prefer the recorded kb_meta.schema_embedding_dim (the populated-DB
    * source of truth); on a FRESH DB (nothing recorded) §2b derives the dim from
    * the embedder /health probe under an advisory lock. Pinned + recorded paths
    * keep §2a behavior. A DB read ERROR fails fast (never guess a default). */
   int recorded_dim = 0;
   db2_dim_read_t rd =
       g_embed_dim_pinned ? DB2_DIM_ABSENT : db2_embedding_dim_read(conn, &recorded_dim);
   if (rd == DB2_DIM_ERROR)
   {
      fprintf(stderr, "aimee: db2_init: reading recorded embedding dim failed\n");
      aimee_pg_close(conn);
      pthread_mutex_unlock(&g_init_lock);
      return -1;
   }
   int effective_dim = db2_effective_dim(g_embed_dim_pinned, configured_dim,
                                         rd == DB2_DIM_FOUND ? recorded_dim : 0);

   /* §2b fresh-DB probe leg: unpinned + nothing recorded + a probe registered. The
    * advisory lock serialises racing kb starts; FAIL-FAST on any probe/lock/read
    * failure and record NOTHING (kb_main retries; never poison the recorded dim). */
   int dim_lock_held = 0, derived_via_probe = 0;
   if (!g_embed_dim_pinned && rd == DB2_DIM_ABSENT && g_embedder_probe)
   {
      /* Wait up to the FULL budget for the lock: a peer mid-bootstrap can take the
       * whole budget (cold embedder), so the waiter must be at least as patient —
       * by the time it acquires (or the peer releases) the dim is recorded and the
       * waiter's double-check below sees it. A genuine budget-long timeout means a
       * stuck/dead holder → fail fast; kb_main.c's bounded retry (24×5s) is the
       * outer safety net so a transient stall self-heals without thrash. */
      int total = g_dim_probe_budget_ms;
      int elapsed = 0;
      int lrc = db2_dim_lock_acquire(conn, total, &elapsed);
      if (lrc != 0)
      {
         /* Timed out or a lock query error: another starter may have bootstrapped
          * in the meantime. Re-read; use a now-recorded dim, else fail fast. */
         int rec2 = 0;
         if (db2_embedding_dim_read(conn, &rec2) == DB2_DIM_FOUND)
            effective_dim = rec2;
         else
         {
            LOG_WARN("db2",
                     "embedder dim bootstrap: advisory lock not acquired in %dms and no dim "
                     "recorded; not derived (kb will retry)",
                     total);
            aimee_pg_close(conn);
            pthread_mutex_unlock(&g_init_lock);
            return -1;
         }
      }
      else
      {
         dim_lock_held = 1;
         int rec2 = 0;
         db2_dim_read_t rd2 = db2_embedding_dim_read(conn, &rec2); /* double-check under the lock */
         if (rd2 == DB2_DIM_ERROR)
         {
            db2_dim_lock_release(conn);
            aimee_pg_close(conn);
            pthread_mutex_unlock(&g_init_lock);
            return -1;
         }
         if (rd2 == DB2_DIM_FOUND)
         {
            effective_dim = rec2; /* another starter bootstrapped between our reads */
         }
         else
         {
            int probed = 0;
            char perr[DB2_PROBE_ERR_LEN] = "";
            /* Remaining budget after the lock wait, floored so a late acquirer
             * (rare: a crashed peer that never recorded) still gets one attempt. */
            int pbudget = total - elapsed;
            if (pbudget < 1000)
               pbudget = 1000;
            int prc = g_embedder_probe(&probed, pbudget, perr, sizeof(perr));
            /* Bound to a valid halfvec width: an out-of-range value must NOT fall
             * through to db_apply's clamp-to-1024 (that would record a wrong dim). */
            if (prc != 0 || probed <= 0 || probed > EMBED_MAX_DIM)
            {
               LOG_WARN("db2", "embedder dim probe failed/out-of-range (got %d, max %d): %s",
                        probed, EMBED_MAX_DIM, perr[0] ? perr : "(no detail)");
               db2_dim_lock_release(conn);
               aimee_pg_close(conn);
               pthread_mutex_unlock(&g_init_lock);
               return -1;
            }
            effective_dim = probed;
            derived_via_probe = 1;
         }
      }
   }

   if (effective_dim != configured_dim)
   {
      if (derived_via_probe)
         LOG_WARN("db2", "fresh DB: derived embedding dim %d from the embedder /health probe",
                  effective_dim);
      else
         LOG_WARN("db2",
                  "using recorded embedding dim %d (no operator pin; configured default was %d)",
                  effective_dim, configured_dim);
      db2_set_embedding_dim(effective_dim); /* halfvec columns + all readers agree */
   }
   if (db_apply_schema_postgres(conn, effective_dim, errbuf, sizeof(errbuf)) != 0)
   {
      /* Surface the postgres error so callers see WHICH statement failed.
       * Silently returning -1 hid bugs like a stale CREATE INDEX referencing
       * a table that lives in a different tier's schema. */
      fprintf(stderr, "aimee: db2_init: schema apply failed: %s\n", errbuf);
      if (dim_lock_held)
         db2_dim_lock_release(conn);
      aimee_pg_close(conn);
      pthread_mutex_unlock(&g_init_lock);
      return -1;
   }
   /* Schema (incl. the halfvec columns) is now applied AND the dim recorded last
    * (record-after-DDL), so the lock can release: a later starter reads the dim. */
   if (dim_lock_held)
      db2_dim_lock_release(conn);

   /* unified-llm-container §2: model-identity drift guard, applied here (where the
    * configured identity globals live) rather than in the lower db_schema layer.
    * Record/check the EMBEDDER model identity alongside the dim — closing the
    * same-dim different-model footgun (two models can share a dim) — and record
    * the reranker identity. Both no-op when the identity is unset (the legacy
    * torch embedder reports none), so existing deployments are unaffected; they
    * activate when the unified container supplies the identity via the setters. */
   if (db2_embedding_model_record_or_check(conn, g_embedder_model_id, g_embedding_compat, errbuf,
                                           sizeof(errbuf)) != 0 ||
       db2_reranker_model_record(conn, g_reranker_model_id, g_reranker_contract, errbuf,
                                 sizeof(errbuf)) != 0)
   {
      fprintf(stderr, "aimee: db2_init: model-identity guard failed: %s\n", errbuf);
      aimee_pg_close(conn);
      pthread_mutex_unlock(&g_init_lock);
      return -1;
   }

   /* pg_trgm is required: db2/schema.sql creates a GIN index on
    * memories.memories_code_fts_text using gin_trgm_ops, and the
    * memory_query path issues % / similarity() against memory text.
    * The schema CREATE EXTENSION tolerates failure (so a least-privileged
    * connect can still touch the schema for read-only diagnostics);
    * here we fail hard at init if the extension didn't end up
    * installed, since trigram fallback was never a supported path.
    *
    * Skip the check under the test shim — the in-memory sqlite shim
    * has no pg_extension catalog. */
   if (!aimee_pg_is_shim())
   {
      char errcheck[256] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "SELECT 1 FROM pg_extension WHERE extname = 'pg_trgm'", errcheck, sizeof(errcheck));
      if (!st)
      {
         aimee_pg_close(conn);
         pthread_mutex_unlock(&g_init_lock);
         return -1;
      }
      aimee_pg_step_t step = aimee_pg_step(st, errcheck, sizeof(errcheck));
      aimee_pg_finalize(st);
      if (step != AIMEE_PG_ROW)
      {
         aimee_pg_close(conn);
         pthread_mutex_unlock(&g_init_lock);
         fprintf(stderr, "aimee: db2_init: pg_trgm extension is not installed in this database. "
                         "Enable it with: CREATE EXTENSION pg_trgm;  (requires a role with the "
                         "appropriate privileges)\n");
         return -1;
      }
   }

   g_conn = conn;
   /* Record the owning thread: db2_conn() hands g_conn to this thread and a
    * private per-thread connection to every other thread. */
   g_init_thread = pthread_self();
   g_init_thread_set = 1;
   snprintf(g_pg_url, sizeof(g_pg_url), "%s", libpq_url);

   /* The code-graph projection upserts edges with
    * ON CONFLICT (source, relation, target), which requires a unique index the
    * base schema.sql deliberately does NOT declare (legacy instances may hold
    * duplicate triples, which would make a plain CREATE UNIQUE INDEX in the
    * schema fail on every startup). Build it best-effort here: on a clean
    * instance it creates the index (projection works); if a legacy instance
    * still holds duplicate triples it logs and continues (no startup regression;
    * dedup is a separate migration). The shim has no real index machinery, so
    * skip it there.
    *
    * This MUST run after g_conn/g_init_thread are recorded above: the builder
    * reaches Postgres through db2_conn(), which returns g_conn only once the init
    * thread is set. Running it earlier (the original site, before g_conn was
    * assigned) made db2_conn() return NULL, so the build silently failed and the
    * index was never created on any instance — graph-code fusion's whole
    * substrate produced zero edges. */
   if (!aimee_pg_is_shim())
   {
      int ee_idx_existed = 0;
      if (db2_entity_edge_build_unique_index(&ee_idx_existed) != 0)
         fprintf(stderr, "aimee: db2_init: entity_edges unique index not built; code-graph "
                         "projection ON CONFLICT will no-op until duplicate triples are deduped\n");
   }
   /* Initialize the connection pool that every non-init thread leases from. On
    * failure, db2_conn falls back to private per-thread connections (degraded
    * but functional). Skipped under the sqlite test shim (a shared sqlite handle
    * is registered): pooling Postgres-only reset SQL has no meaning there, and
    * db2_conn returns the shim's shared connection. */
   if (!db2_shared_sqlite())
   {
      char perr[256] = "";
      if (db2_pool_init(libpq_url, g_pool_size, perr, sizeof(perr)) != 0)
         fprintf(stderr,
                 "aimee: db2_init: connection pool init failed (%s); using per-thread "
                 "connections\n",
                 perr);
   }
   pthread_mutex_unlock(&g_init_lock);
   return 0;
}

int db2_health_probe(int *schema_ok, int *have_pg_trgm)
{
   char errbuf[256] = "";

   if (schema_ok)
      *schema_ok = 0;
   if (have_pg_trgm)
      *have_pg_trgm = 0;

   if (!g_conn)
      return -1;

   if (aimee_pg_exec(g_conn, "SELECT 1", errbuf, sizeof(errbuf)) != 0)
      return -1;

   int schema_present = db2_query_flag("SELECT 1 FROM information_schema.tables "
                                       "WHERE table_schema = current_schema() AND table_name = :t",
                                       "t", "memories");
   if (schema_present < 0)
      return -1;
   if (schema_ok)
      *schema_ok = schema_present;

   /* pg_trgm is required by db2_init; reporting its presence here for
    * doctor/diagnostics. The shim has no pg_extension catalog, so
    * report installed=1 unconditionally under the shim (doctor path
    * never runs against the shim in production but tests can call
    * health_probe). */
   int ext_present;
   if (aimee_pg_is_shim())
      ext_present = 1;
   else
      ext_present =
          db2_query_flag("SELECT 1 FROM pg_extension WHERE extname = 'pg_trgm'", NULL, NULL);
   if (ext_present < 0)
      return -1;
   if (have_pg_trgm)
      *have_pg_trgm = ext_present;

   return 0;
}

int db2_kb_health_probe(int *kb_tables_ok)
{
   if (kb_tables_ok)
      *kb_tables_ok = 0;
   if (!g_conn)
      return -1;
   int docs_ok = db2_query_flag("SELECT 1 FROM information_schema.tables "
                                "WHERE table_schema = current_schema() AND table_name = :t",
                                "t", "kb_documents");
   int jobs_ok = db2_query_flag("SELECT 1 FROM information_schema.tables "
                                "WHERE table_schema = current_schema() AND table_name = :t",
                                "t", "kb_async_jobs");
   if (docs_ok < 0 || jobs_ok < 0)
      return -1;
   if (kb_tables_ok)
      *kb_tables_ok = (docs_ok && jobs_ok) ? 1 : 0;
   return 0;
}

/* Postgres-native stat snapshot for `aimee doctor` and ops diagnostics.
 * All outputs are best-effort; missing values are reported as -1 so the
 * caller can decide whether to surface the gap. Returns 0 if the probe
 * connection is alive, -1 otherwise. Skipped under the test shim. */
int db2_pg_stat_summary(int *active_conns, int *max_conns, int *is_replica,
                        int64_t *replica_lag_bytes)
{
   if (active_conns)
      *active_conns = -1;
   if (max_conns)
      *max_conns = -1;
   if (is_replica)
      *is_replica = -1;
   if (replica_lag_bytes)
      *replica_lag_bytes = -1;

   if (!g_conn)
      return -1;

   if (aimee_pg_is_shim())
      return 0;

   char errbuf[256] = "";
   aimee_pg_stmt_t *st = NULL;

   if (active_conns)
   {
      st = aimee_pg_prepare(g_conn,
                            "SELECT count(*)::int FROM pg_stat_activity "
                            "WHERE datname = current_database()",
                            errbuf, sizeof(errbuf));
      if (st)
      {
         if (aimee_pg_step(st, errbuf, sizeof(errbuf)) == AIMEE_PG_ROW)
            *active_conns = aimee_pg_column_int(st, 0);
         aimee_pg_finalize(st);
      }
   }

   if (max_conns)
   {
      st = aimee_pg_prepare(g_conn, "SELECT current_setting('max_connections')::int", errbuf,
                            sizeof(errbuf));
      if (st)
      {
         if (aimee_pg_step(st, errbuf, sizeof(errbuf)) == AIMEE_PG_ROW)
            *max_conns = aimee_pg_column_int(st, 0);
         aimee_pg_finalize(st);
      }
   }

   if (is_replica)
   {
      st = aimee_pg_prepare(g_conn, "SELECT pg_is_in_recovery()::int", errbuf, sizeof(errbuf));
      if (st)
      {
         if (aimee_pg_step(st, errbuf, sizeof(errbuf)) == AIMEE_PG_ROW)
            *is_replica = aimee_pg_column_int(st, 0);
         aimee_pg_finalize(st);
      }
   }

   /* Replication lag in WAL bytes, applicable only when this connection
    * is on a standby. Returns 0 when not in recovery. */
   if (replica_lag_bytes && is_replica && *is_replica == 1)
   {
      st = aimee_pg_prepare(
          g_conn,
          "SELECT COALESCE("
          "  pg_wal_lsn_diff(pg_last_wal_receive_lsn(), pg_last_wal_replay_lsn()), 0)::bigint",
          errbuf, sizeof(errbuf));
      if (st)
      {
         if (aimee_pg_step(st, errbuf, sizeof(errbuf)) == AIMEE_PG_ROW)
            *replica_lag_bytes = aimee_pg_column_int64(st, 0);
         aimee_pg_finalize(st);
      }
   }

   return 0;
}

void db2_shutdown(void)
{
   /* Drain + close the pool first (its reaper thread + members) before the
    * owner connection. */
   db2_pool_shutdown();
   pthread_mutex_lock(&g_init_lock);
   if (g_conn)
   {
      aimee_pg_close(g_conn);
      g_conn = NULL;
   }
   g_pg_url[0] = '\0';
   /* §2a: clear the dim + pinned flag so a reopen / a later unit test starts from
    * the unpinned default rather than inheriting this run's state. db2_init may
    * have re-set g_embed_dim to a recorded value, so reset it for symmetry — every
    * real caller sets it again via db2_set_embedding_dim before the next init. */
   g_embed_dim = 0;
   g_embed_dim_pinned = 0;
   /* §2b: defensively clear the probe seam + budget (the caller SHOULD deregister
    * before db2_shutdown, but back it up here, mirroring g_embed_dim_pinned). */
   g_embedder_probe = NULL;
   g_dim_probe_budget_ms = 120000;
   g_embedder_model_id[0] = '\0';
   g_reranker_model_id[0] = '\0';
   g_reranker_contract[0] = '\0';
   g_embedding_compat[0] = '\0';
   pthread_mutex_unlock(&g_init_lock);
}

#ifndef AIMEE_DISABLE_DB2_SQLITE_SHIM
static sqlite3 *g_eval_temp_store_handle = NULL;
#endif

#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
/* ---- Real-Postgres eval scratch store (libpq build; no sqlite shim here) ----------
 * Instead of an in-memory sqlite, carve an ISOLATED throwaway SCHEMA in a DISPOSABLE
 * Postgres, apply the full DB2 schema (incl. the memory_negation_fts_tsv FTS projection
 * and pgvector columns) into it, and pin this process's db2 connection (g_conn -- what
 * db2_conn() returns) at it. Lets the corpus-file retrieval eval exercise Postgres-only
 * features (e.g. memory_negation) that the sqlite shim can't.
 *
 * SAFETY: (1) an eval requires an EXPLICIT AIMEE_DB2_EVAL_URL -- there is deliberately NO
 * fallback to the production AIMEE_DB2_URL, so it can only hit a database an operator
 * designated disposable. (2) search_path is the eval schema first; every aimee table is
 * (re)created there, so a bare `memories`/etc. resolves to the eval copy and SHADOWS any
 * same-named production table -- `public` remains on the path ONLY so pgvector/pg_trgm
 * TYPES resolve. (3) the schema is DROP ... CASCADE'd on close. This takes over the
 * global g_conn, so it is for a STANDALONE eval process only and must NEVER run inside
 * the live multi-threaded server. */
static char g_eval_pg_schema[64] = "";

static void db2_eval_pg_drop_schema(void *conn, const char *schema)
{
   if (!conn || !schema || !schema[0])
      return;
   char sql[128], err[256] = {0};
   snprintf(sql, sizeof(sql), "DROP SCHEMA IF EXISTS \"%s\" CASCADE", schema);
   (void)aimee_pg_exec(conn, sql, err, sizeof(err));
}

static int db2_eval_open_temp_store_pg(void)
{
   const char *url = getenv("AIMEE_DB2_EVAL_URL");
   if (!url || !url[0])
   {
      fprintf(stderr, "aimee: eval temp store needs AIMEE_DB2_EVAL_URL (a DISPOSABLE "
                      "Postgres; never the production DSN)\n");
      return -1;
   }
   char err[512] = {0};
   static unsigned g_eval_seq = 0;
   char schema[64];
   snprintf(schema, sizeof(schema), "aimee_eval_%ld_%u", (long)getpid(), ++g_eval_seq);

   void *conn = aimee_pg_open(url, err, sizeof(err));
   if (!conn)
   {
      fprintf(stderr, "aimee: eval temp store: connect failed: %s\n", err);
      return -1;
   }
   char sql[192];
   snprintf(sql, sizeof(sql), "CREATE SCHEMA \"%s\"", schema);
   if (aimee_pg_exec(conn, sql, err, sizeof(err)) != 0)
   {
      fprintf(stderr, "aimee: eval temp store: CREATE SCHEMA failed: %s\n", err);
      aimee_pg_close(conn);
      return -1;
   }
   snprintf(sql, sizeof(sql), "SET search_path TO \"%s\", public", schema);
   if (aimee_pg_exec(conn, sql, err, sizeof(err)) != 0)
   {
      fprintf(stderr, "aimee: eval temp store: SET search_path failed: %s\n", err);
      db2_eval_pg_drop_schema(conn, schema);
      aimee_pg_close(conn);
      return -1;
   }
   if (db_apply_schema_postgres(conn, db2_embedding_dim(), err, sizeof(err)) != 0)
   {
      fprintf(stderr, "aimee: eval temp store: schema apply failed: %s\n", err);
      db2_eval_pg_drop_schema(conn, schema);
      aimee_pg_close(conn);
      return -1;
   }
   g_conn = conn; /* memory ops via db2_conn() now resolve against the eval schema */
   db2_set_ephemeral(1);
   snprintf(g_eval_pg_schema, sizeof(g_eval_pg_schema), "%s", schema);
   return 0;
}
#endif

int db2_eval_open_temp_store(void)
{
#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
   return db2_eval_open_temp_store_pg();
#else
   db2_eval_close_temp_store();
   if (!sqlite3_open || !sqlite3_close || !sqlite3_exec)
      return -1;
   sqlite3 *raw = NULL;
   if (sqlite3_open(":memory:", &raw) != SQLITE_OK || !raw)
   {
      if (raw)
         sqlite3_close(raw);
      return -1;
   }
   /* :memory: ignores disk-oriented pragmas (WAL, sync, mmap); the only
    * one that matters for the schema's REFERENCES clauses is foreign_keys. */
   sqlite3_exec(raw, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
   char err[512] = {0};
   if (db2_apply_schema_sqlite_shim(raw, err, sizeof(err)) != 0)
   {
      sqlite3_close(raw);
      return -1;
   }
   db2_register_shared_sqlite(raw);
   db2_set_ephemeral(1);
   g_eval_temp_store_handle = raw;
   /* In test builds the aimee_pg_* shim resolves "shim" to the
    * registered sqlite handle; in production with real libpq this
    * would attempt a network connection, so skip it there. */
   if (aimee_pg_is_shim())
      (void)db2_init("shim");
   return 0;
#endif
}

void db2_eval_close_temp_store(void)
{
#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
   /* Real-Postgres eval store: DROP the throwaway schema (CASCADE removes every table,
    * index and the negation FTS projection) and close the dedicated connection. */
   if (!g_eval_pg_schema[0])
      return;
   db2_set_ephemeral(0);
   db2_eval_pg_drop_schema(g_conn, g_eval_pg_schema);
   aimee_pg_close(g_conn);
   g_conn = NULL;
   g_eval_pg_schema[0] = '\0';
#else
   if (!g_eval_temp_store_handle)
      return;
   /* Restore non-ephemeral when the eval scratch store goes away, so the flag
    * can never outlive the eval that set it. */
   db2_set_ephemeral(0);
   if (aimee_pg_is_shim())
      db2_shutdown();
   db2_maybe_clear_sqlite_cache(g_eval_temp_store_handle);
   db2_register_shared_sqlite(NULL);
   if (sqlite3_close)
      sqlite3_close(g_eval_temp_store_handle);
   g_eval_temp_store_handle = NULL;
#endif
}

/* --- Generic scalar/exec convenience helpers ------------------------------
 * See db2_internal.h for contract. These collapse the prepare/bind/step/
 * finalize boilerplate that recurred verbatim across the per-column getters,
 * counters, and fire-and-forget setters in the db2 sources. */

#define DB2Q_ERRBUF 256

int db2_scalar_int(const char *sql, int dflt)
{
   void *conn = db2_conn();
   if (!conn)
      return dflt;
   char err[DB2Q_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return dflt;
   int value = dflt;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      value = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return value;
}

int db2_scalar_int_text(const char *sql, const char *a1, int dflt)
{
   void *conn = db2_conn();
   if (!conn)
      return dflt;
   char err[DB2Q_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return dflt;
   aimee_pg_bind_text(st, "?1", a1);
   int value = dflt;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      value = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return value;
}

double db2_scalar_double_id(const char *sql, int64_t id, double dflt)
{
   void *conn = db2_conn();
   if (!conn)
      return dflt;
   char err[DB2Q_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return dflt;
   aimee_pg_bind_int64(st, "?1", id);
   double value = dflt;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      value = aimee_pg_column_double(st, 0);
   aimee_pg_finalize(st);
   return value;
}

void db2_exec_id(const char *sql, int64_t id)
{
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[DB2Q_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_exec_conn_int64(void *conn, const char *sql, int64_t arg)
{
   if (!conn)
      return -1;
   char err[DB2Q_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", arg);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

void db2_exec_text(const char *sql, const char *a1)
{
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[DB2Q_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", a1);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_exec_text_id(const char *sql, const char *a1, int64_t id)
{
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[DB2Q_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", a1);
   aimee_pg_bind_int64(st, "?2", id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_copy_text(char *dst, size_t cap, const char *src)
{
   if (!dst || cap == 0)
      return;
   snprintf(dst, cap, "%s", src ? src : "");
}

void db2_copy_col_text(char *dst, size_t cap, aimee_pg_stmt_t *st, int col)
{
   const char *value = aimee_pg_column_text(st, col);
   snprintf(dst, cap, "%s", value ? value : "");
}
