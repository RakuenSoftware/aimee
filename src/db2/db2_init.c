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
#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
typedef struct sqlite3 sqlite3;
#else
#include <sqlite3.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if !defined(AIMEE_DISABLE_DB2_SQLITE_SHIM) && (defined(__GNUC__) || defined(__clang__))
#pragma weak sqlite3_close
#pragma weak sqlite3_exec
#pragma weak sqlite3_open
#endif

void db1_stmt_cache_clear_for_sqlite(sqlite3 *db) __attribute__((weak));

static void db2_maybe_clear_sqlite_cache(sqlite3 *db)
{
   if (!db)
      return;
   if (db1_stmt_cache_clear_for_sqlite)
      db1_stmt_cache_clear_for_sqlite(db);
}

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

int db2_embedding_dim(void)
{
   return g_embed_dim > 0 ? g_embed_dim : 1024;
}

void db2_set_embedding_dim_pinned(int pinned)
{
   g_embed_dim_pinned = pinned ? 1 : 0;
}

int db2_effective_dim(int pinned, int configured, int recorded)
{
   if (pinned)
      return configured; /* operator pin is authoritative */
   if (recorded > 0)
      return recorded; /* recorded wins over the configured default */
   return configured;  /* fresh DB / nothing recorded: the default */
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
   /* §2a precedence: when the operator did NOT pin a dim, prefer the recorded
    * kb_meta.schema_embedding_dim (the populated-DB source of truth) over the
    * default, so an unpinned deploy self-derives its dim instead of refusing the
    * apply. Pinned and fresh-DB paths keep configured_dim → behavior-identical. */
   int recorded_dim = g_embed_dim_pinned ? 0 : db2_embedding_dim_get(conn);
   int effective_dim = db2_effective_dim(g_embed_dim_pinned, configured_dim, recorded_dim);
   if (effective_dim != configured_dim)
   {
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
   pthread_mutex_unlock(&g_init_lock);
}

static sqlite3 *g_eval_temp_store_handle = NULL;

int db2_eval_open_temp_store(void)
{
#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
   return -1;
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
   if (!g_eval_temp_store_handle)
      return;
   /* Restore non-ephemeral when the eval scratch store goes away, so the flag
    * can never outlive the eval that set it. */
   db2_set_ephemeral(0);
#ifdef AIMEE_DISABLE_DB2_SQLITE_SHIM
   db2_maybe_clear_sqlite_cache(g_eval_temp_store_handle);
   db2_register_shared_sqlite(NULL);
   g_eval_temp_store_handle = NULL;
#else
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
