/* Knowledge schema and lease lifecycle. PostgreSQL owns every connection.
 * The bootstrap thread holds an exclusive migration capability; runtime
 * workers lease bounded sessions through the supervised PostgreSQL module.
 */

#include "kb_store.h"
#include "kb_store_hardening.h"
#include "kb_store_internal.h"

#include "db_postgres.h"
#include "db_schema.h"
#include "../support/kb_store_log.h" /* LOG_WARN */
#include "entity_edges.h"

#include <pthread.h>
#include <stdlib.h>
#ifdef AIMEE_DISABLE_KB_STORE_SQLITE_SHIM
typedef struct sqlite3 sqlite3;
#else
#include <sqlite3.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h> /* §2b: CLOCK_MONOTONIC poll budget + nanosleep between lock polls */

#if !defined(AIMEE_DISABLE_KB_STORE_SQLITE_SHIM) && (defined(__GNUC__) || defined(__clang__))
#pragma weak sqlite3_exec
#endif

static void *g_conn = NULL;
static int g_runtime_provider;
static int g_migration_provider;
static int g_local_runtime_provider;
static int g_initialized;
static char g_pg_url[512] = "";
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
/* Embedding dimension for the KB_STORE vector columns (one embedder per deployment).
 * Set from the loaded config by the server / aimee-kb startup via
 * kb_store_set_embedding_dim() before kb_store_init(), so this layer needs no config
 * dependency. 0 = unset, which lets the §2a precedence (pinned > recorded >
 * probed > default) fall through to the injected default below. A deployment that
 * predates an embedder change has its old dim RECORDED in
 * kb_meta.schema_embedding_dim, and the recorded value outranks the default, so an
 * existing corpus keeps working and is migrated deliberately via `aimee kb reembed`. */
static int g_embed_dim = 0;

/* The default width, INJECTED from config (config_embedder_dims_default) at the
 * same startup site that sets g_embed_dim. This layer deliberately holds no
 * literal of its own: the width is declared once, in config, and a copy here
 * could disagree with the embedder that is actually running. 0 = never injected,
 * which kb_store_embedding_dim() reports as 0 so callers fail loudly instead of
 * sizing columns from a guess. */
static int g_embed_dim_default = 0;

void kb_store_set_embedding_dim_default(int dim)
{
   g_embed_dim_default = dim > 0 ? dim : 0;
}

/* §2a: whether the operator pinned the dim. When 0 (default) and nothing was
 * pinned, kb_store_init prefers a recorded kb_meta.schema_embedding_dim over the
 * default. Reset in kb_store_shutdown so a reopen / a later test never inherits it. */
static int g_embed_dim_pinned = 0;

void kb_store_set_embedding_dim(int dim)
{
   g_embed_dim = dim;
}

/* §2c: expose the init mutex so the dim-change reset (kb_store_reembed.c) serializes its
 * destructive execute + in-memory dim swap against kb_store_init and any concurrent reset
 * — the two paths that mutate the schema + recorded/in-memory dim together. The
 * accessors only touch g_init_lock; kb_store_set_embedding_dim/kb_store_embedding_dim take no
 * lock, so holding it across the swap cannot self-deadlock. */
void kb_store_init_lock(void)
{
   pthread_mutex_lock(&g_init_lock);
}
void kb_store_init_unlock(void)
{
   pthread_mutex_unlock(&g_init_lock);
}

int kb_store_embedding_dim(void)
{
   return g_embed_dim > 0 ? g_embed_dim : g_embed_dim_default;
}

void kb_store_set_embedding_dim_pinned(int pinned)
{
   g_embed_dim_pinned = pinned ? 1 : 0;
}

/* §2b: the embedder /health probe seam + its wall-clock budget. NULL/0 by default
 * (the §2b path is skipped → behavior identical to §2a). Both reset in
 * kb_store_shutdown so a reopen / a later test never inherits them. */
static kb_store_embedder_probe_fn g_embedder_probe = NULL;
static int g_dim_probe_budget_ms = 120000;

void kb_store_set_embedder_probe(kb_store_embedder_probe_fn fn)
{
   g_embedder_probe = fn;
}

int kb_store_embedder_probe_registered(void)
{
   return g_embedder_probe != NULL;
}

void kb_store_set_dim_probe_budget_ms(int ms)
{
   if (ms > 0)
      g_dim_probe_budget_ms = ms;
}

/* §2c: probe the running embedder for its CURRENT output dim via the registered
 * §2b probe. The dim-change reset needs this as its target (after a model swap the
 * running kb_store_embedding_dim() is still the old/recorded value). Returns 0 + *out on
 * success; -1 if no probe is registered or it fails. */
int kb_store_probe_embedder_dim(int budget_ms, int *out)
{
   if (out)
      *out = 0;
   if (!g_embedder_probe)
      return -1;
   int dim = 0;
   char err[KB_STORE_PROBE_ERR_LEN] = "";
   if (g_embedder_probe(&dim, budget_ms > 0 ? budget_ms : 8000, err, sizeof(err)) != 0 || dim <= 0)
      return -1;
   if (out)
      *out = dim;
   return 0;
}

/* Model-identity drift guard. A dim-only guard is insufficient: two different
 * models can share a dimension, so a same-dim swap would silently mix
 * incompatible vector spaces. These globals carry the configured embedder model
 * identity (repo@sha) and the compat-list of admitted transitions, set from
 * config before kb_store_init like the
 * dim above. INVARIANT: set at exactly the sites that call kb_store_set_embedding_dim()
 * — the serving config-load paths (cmd_core bootstrap_kb_store, kb_main, cmd_doctor);
 * the connectivity-probe path (bootstrap_kb_store_try_url) and tests deliberately set
 * neither (a probe shuts down before any serving schema applies). ALL DEFAULT
 * EMPTY: an empty embedder model_id makes the guard a no-op, so a deployment that
 * has not yet adopted the unified container (the live torch embedder reports no
 * identity) is unaffected. */
static char g_embedder_model_id[160] = "";
/* The serving endpoint's vector-space identity, probed (not configured) — see
 * kb_store_set_embedder_serving_id. Empty leaves the guard a no-op. */
static char g_embedder_serving_id[160] = "";
static kb_store_embedder_serving_probe_fn g_embedder_serving_probe = NULL;

void kb_store_set_embedder_serving_probe(kb_store_embedder_serving_probe_fn fn)
{
   g_embedder_serving_probe = fn;
}

int kb_store_embedder_serving_probe_registered(void)
{
   return g_embedder_serving_probe != NULL;
}
static char g_embedding_compat[1024] = ""; /* CSV of "old_id->new_id" transitions */

void kb_store_set_embedder_model_id(const char *model_id)
{
   snprintf(g_embedder_model_id, sizeof(g_embedder_model_id), "%s", model_id ? model_id : "");
}

const char *kb_store_embedder_model_id(void)
{
   return g_embedder_model_id;
}

void kb_store_set_embedder_serving_id(const char *serving_id)
{
   snprintf(g_embedder_serving_id, sizeof(g_embedder_serving_id), "%s",
            serving_id ? serving_id : "");
}

const char *kb_store_embedder_serving_id(void)
{
   return g_embedder_serving_id;
}

void kb_store_set_embedding_compat(const char *compat_csv)
{
   snprintf(g_embedding_compat, sizeof(g_embedding_compat), "%s", compat_csv ? compat_csv : "");
}

const char *kb_store_embedding_compat(void)
{
   return g_embedding_compat;
}

int kb_store_effective_dim(int pinned, int configured, int recorded)
{
   if (pinned)
      return configured; /* operator pin is authoritative */
   if (recorded > 0)
      return recorded; /* recorded wins over the configured default */
   return configured;  /* fresh DB / nothing recorded: the default */
}

/* Should a start be refused because the embedder cannot produce the corpus's recorded
 * width? Pure, so the rule is testable without a database.
 *
 * REFUSE ONLY ON EVIDENCE. probe_rc != 0 means the embedder did not answer, which is
 * not the same as disagreeing: an embedder that is slow, or that reports no width, must
 * keep starting exactly as it did before. Guessing in that direction takes down working
 * deployments; guessing in the other lets a kb come up healthy against a corpus every
 * write will bounce off. */
int kb_store_dim_drift_refuses(int probe_rc, int probed_dim, int recorded_dim)
{
   if (probe_rc != 0 || probed_dim <= 0 || recorded_dim <= 0)
      return 0; /* no answer, or nothing recorded: not evidence of drift */
   return probed_dim != recorded_dim;
}

/* §2b precedence (pure): pin > recorded > probe > default. */
kb_store_dim_source_t kb_store_dim_source(int pinned, int recorded_present, int probe_available)
{
   if (pinned)
      return KB_STORE_DIM_SRC_PIN;
   if (recorded_present)
      return KB_STORE_DIM_SRC_RECORDED;
   if (probe_available)
      return KB_STORE_DIM_SRC_PROBE;
   return KB_STORE_DIM_SRC_DEFAULT;
}

/* §2b: a Postgres advisory lock serialises fresh-DB dim bootstrap across racing kb
 * starts. Keyed by hashtext of this string (the mining.c pattern). Bump the _vN
 * suffix only on a semantics-changing lock change (added waiters / xact-scoped),
 * never on a code refactor — the value is a wire contract between concurrent kbs. */
#define KB_STORE_DIM_LOCK_KEY "aimee_dim_bootstrap_v1"

static long kb_store_mono_ms(void)
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
static int kb_store_dim_lock_acquire(void *conn, int budget_ms, int *elapsed_ms)
{
   long start = kb_store_mono_ms();
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
         aimee_pg_bind_text(st, "?1", KB_STORE_DIM_LOCK_KEY);
         if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            ok = 1;
            got = aimee_pg_column_int(st, 0) ? 1 : 0;
         }
         aimee_pg_finalize(st);
      }
      int spent = (int)(kb_store_mono_ms() - start);
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

static void kb_store_dim_lock_release(void *conn)
{
   if (aimee_pg_is_shim())
      return;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT pg_advisory_unlock(hashtext(?1))", err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", KB_STORE_DIM_LOCK_KEY);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

static pthread_key_t g_thread_conn_key;
static pthread_once_t g_thread_conn_key_once = PTHREAD_ONCE_INIT;
/* The thread that ran kb_store_init() owns g_conn and uses it directly; every other
 * thread gets its own libpq connection via kb_store_conn() (see the comment there). */
static pthread_t g_init_thread;
static int g_init_thread_set = 0;

/* A non-init thread's current connection: a pool lease (pooled=1) or, when the
 * pool is exhausted/unavailable, a private overflow connection (pooled=0).
 * Stored in g_thread_conn_key; the destructor returns/closes it on thread exit
 * (this is the reliable reclaim-on-thread-death the pool reaper deliberately
 * leaves to us). A failed acquisition never falls back to g_conn: libpq forbids
 * concurrent use of one PGconn, and g_conn belongs exclusively to the init
 * thread. g_lease_depth refcounts kb_store_lease_begin/_end so nested units
 * reuse one lease. */
typedef struct
{
   void *conn;
} kb_store_thread_lease_t;

static __thread int g_lease_depth = 0;
/* Call site of the OUTERMOST kb_store_lease_begin on this thread, so a lease held
 * past the pool's ceiling can be reported as the code that took it. Always a
 * string literal from the macro in kb_store.h; never freed. */
static __thread const char *g_lease_site = NULL;

static void thread_conn_destructor(void *p)
{
   kb_store_thread_lease_t *L = (kb_store_thread_lease_t *)p;
   if (!L)
      return;
   if (L->conn)
   {
      aimee_pg_close(L->conn);
   }
   free(L);
}

static void thread_conn_key_init(void)
{
   pthread_key_create(&g_thread_conn_key, thread_conn_destructor);
}
/* Test-shim sqlite handle. Set by kb_store_register_shared_sqlite (tests
 *). Does not own the handle; callers are
 * responsible for lifetime. */
static sqlite3 *g_shared_sqlite = NULL;
static int g_shared_ephemeral = 0;
static pthread_mutex_t g_shared_sqlite_lock = PTHREAD_MUTEX_INITIALIZER;

void kb_store_register_shared_sqlite(sqlite3 *h)
{
   pthread_mutex_lock(&g_shared_sqlite_lock);
   g_shared_sqlite = h;
   /* Fresh registration: assume non-ephemeral unless the caller opts in
    * via kb_store_set_ephemeral.  Clearing on re-register avoids leaking a
    * stale flag from a prior test. */
   g_shared_ephemeral = 0;
   pthread_mutex_unlock(&g_shared_sqlite_lock);
}

sqlite3 *kb_store_shared_sqlite(void)
{
   pthread_mutex_lock(&g_shared_sqlite_lock);
   sqlite3 *h = g_shared_sqlite;
   pthread_mutex_unlock(&g_shared_sqlite_lock);
   return h;
}

void kb_store_set_ephemeral(int ephemeral)
{
   /* Ephemeral is a TEST/EVAL-only mode (the in-memory sqlite shim). A real
    * libpq instance must never enter it: ephemeral suppresses durable vector
    * writes (kb_store_vector_index_sync_suppressed), so flipping it on in production
    * would silently stop memory embeddings from persisting. Refuse, fail-safe. */
   if (ephemeral && !aimee_pg_is_shim())
      return;
   pthread_mutex_lock(&g_shared_sqlite_lock);
   g_shared_ephemeral = ephemeral ? 1 : 0;
   pthread_mutex_unlock(&g_shared_sqlite_lock);
}

int kb_store_is_ephemeral(void)
{
   /* Belt-and-suspenders with kb_store_set_ephemeral: a real libpq (production)
    * instance is NEVER ephemeral, regardless of the stored flag. Ephemeral
    * only has meaning under the sqlite shim used by tests/evals. */
   if (!aimee_pg_is_shim())
      return 0;
   pthread_mutex_lock(&g_shared_sqlite_lock);
   int v = g_shared_ephemeral;
   pthread_mutex_unlock(&g_shared_sqlite_lock);
   return v;
}

static int kb_store_query_flag(void *conn, const char *sql, const char *param_name,
                               const char *param_value)
{
   char errbuf[256] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(conn, sql, errbuf, sizeof(errbuf));
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

/* Hardened tier (P7): the kb connects as a non-owner runtime role that CANNOT apply
 * owner-only DDL. The schema is applied by a separate migrate/owner step; here we
 * VERIFY (read-only) that the migrated schema is present, current, and dim-
 * compatible, and fail closed otherwise — never attempting the apply. Every query
 * is a plain SELECT / catalog lookup a non-owner runtime role may run. Called only
 * when kb_store_hardening_enabled(); the dev/owner path still auto-applies as before. */
static int kb_store_verify_pre_provisioned(void *conn, int expected_dim, char *err, size_t errlen)
{
   /* 1. Embedding dim: recorded (proves the migrate ran) and equal to what this kb
    *    will size its vector columns / readers to. */
   int recorded_dim = 0;
   kb_store_dim_read_t rd = kb_store_embedding_dim_read(conn, &recorded_dim);
   if (rd == KB_STORE_DIM_ERROR)
   {
      snprintf(err, errlen, "hardened tier: could not read the recorded embedding dim");
      return -1;
   }
   if (rd != KB_STORE_DIM_FOUND)
   {
      snprintf(err, errlen,
               "hardened tier: schema is not migrated (no recorded embedding dim). A "
               "runtime-role kb cannot apply the schema — run the owner/migrate step first.");
      return -1;
   }
   if (recorded_dim != expected_dim)
   {
      snprintf(err, errlen,
               "hardened tier: embedding dim mismatch (kb expects %d, schema built for %d)",
               expected_dim, recorded_dim);
      return -1;
   }
   /* 2. Schema version: recorded and not older than what this kb depends on. */
   {
      char e2[256] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "SELECT value FROM kb_meta WHERE key='schema_version'", e2, sizeof e2);
      long ver = -1;
      if (st && aimee_pg_step(st, e2, sizeof e2) == AIMEE_PG_ROW)
      {
         const char *v = aimee_pg_column_text(st, 0);
         ver = v ? strtol(v, NULL, 10) : -1;
      }
      if (st)
         aimee_pg_finalize(st);
      if (ver < 0)
      {
         snprintf(err, errlen, "hardened tier: no recorded schema_version — schema not migrated");
         return -1;
      }
      if (ver < AIMEE_KB_STORE_SCHEMA_VERSION)
      {
         snprintf(err, errlen,
                  "hardened tier: schema_version %ld is older than the required %d — run the "
                  "migration before starting a runtime kb",
                  ver, AIMEE_KB_STORE_SCHEMA_VERSION);
         return -1;
      }
   }
   /* 3. Representative object presence: a core table, a recent table, and the newest
    *    function. to_regclass/to_regprocedure are catalog lookups any role may run;
    *    they return NULL for absent objects (never error). */
   static const char *const present_checks[] = {
       "SELECT (to_regclass('public.kb_documents') IS NOT NULL)::text",
       "SELECT (to_regclass('public.kb_vault_witness_checkpoint') IS NOT NULL)::text",
       "SELECT (to_regprocedure('public.org_vault_witness_control_fence()') IS NOT NULL)::text",
   };
   static const char *const present_names[] = {"kb_documents", "kb_vault_witness_checkpoint",
                                               "org_vault_witness_control_fence"};
   for (size_t i = 0; i < sizeof present_checks / sizeof present_checks[0]; i++)
   {
      char e2[256] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, present_checks[i], e2, sizeof e2);
      int present = 0;
      if (st && aimee_pg_step(st, e2, sizeof e2) == AIMEE_PG_ROW)
      {
         const char *v = aimee_pg_column_text(st, 0);
         present = (v && v[0] == 't');
      }
      if (st)
         aimee_pg_finalize(st);
      if (!present)
      {
         snprintf(err, errlen,
                  "hardened tier: required object %s is absent — schema not migrated or stale",
                  present_names[i]);
         return -1;
      }
   }
   return 0;
}

/* Native callers retain only a scoped PostgreSQL capability. The provider
 * owns admission and cleanup; exhaustion never opens an overflow connection. */
void *kb_store_scope_connection_open(char *error, size_t capacity)
{
   if (!g_initialized && !g_conn)
      return NULL;
   /* Migration authority belongs only to the bootstrap thread's existing
    * connection. Background audit work must wait for runtime initialization. */
   if (g_migration_provider)
      return NULL;
   return g_local_runtime_provider ? aimee_pg_open_configured(error, capacity)
          : g_runtime_provider     ? aimee_pg_open_runtime(error, capacity)
                                   : aimee_pg_open(g_pg_url, error, capacity);
}
static void *kb_store_thread_acquire(void)
{
   char error[256] = "";
   void *conn = kb_store_scope_connection_open(error, sizeof(error));
   if (!conn)
      return NULL;
   kb_store_thread_lease_t *L = (kb_store_thread_lease_t *)pthread_getspecific(g_thread_conn_key);
   if (!L)
   {
      L = (kb_store_thread_lease_t *)calloc(1, sizeof(*L));
      if (!L)
      {
         aimee_pg_close(conn);
         return NULL;
      }
      if (pthread_setspecific(g_thread_conn_key, L) != 0)
      {
         free(L);
         aimee_pg_close(conn);
         return NULL;
      }
   }
   L->conn = conn;

   return conn;
}

void *kb_store_conn_at(const char *site)
{
   pthread_once(&g_thread_conn_key_once, thread_conn_key_init);
   kb_store_thread_lease_t *L = (kb_store_thread_lease_t *)pthread_getspecific(g_thread_conn_key);
   if (L && L->conn)
      return L->conn; /* the thread's current lease (re-entrant) */
   /* The kb_store_init() owner thread uses its dedicated g_conn directly. */
   if (!g_runtime_provider && (!g_init_thread_set || pthread_equal(pthread_self(), g_init_thread)))
      return g_conn;
   /* Attribute a LAZY acquire (depth 0, outside any kb_store_lease_begin scope). That
    * is the shape that leaks: a long-lived worker takes a connection here and
    * never calls kb_store_lease_release_idle, pinning a pool member for its lifetime.
    * Only kb_store_lease_begin used to record a site, so precisely this case reached
    * the reaper "unattributed". Set only when no scope owns the thread, so an
    * explicit begin keeps its own attribution. */
   if (!g_lease_site && site)
      g_lease_site = site;
   /* Every other thread leases from the pool (lazily; returned on thread exit
    * by the destructor, or sooner via kb_store_lease_end at a job boundary). */
   return kb_store_thread_acquire();
}

/* Kept for any translation unit that does not see the kb_store_conn() macro. */
void *(kb_store_conn)(void)
{
   return kb_store_conn_at(NULL);
}

void kb_store_lease_begin_at(const char *site)
{
   pthread_once(&g_thread_conn_key_once, thread_conn_key_init);
   /* The init thread is never pooled. */
   if (!g_runtime_provider && (!g_init_thread_set || pthread_equal(pthread_self(), g_init_thread)))
      return;
   if (g_lease_depth++ == 0)
   {
      /* Outermost scope owns the attribution: a nested begin is served by the
       * same connection, so the first caller is the one that must release it. */
      g_lease_site = site;
      kb_store_thread_lease_t *L =
          (kb_store_thread_lease_t *)pthread_getspecific(g_thread_conn_key);
      if (!L || !L->conn)
         (void)kb_store_thread_acquire(); /* eager lease for the unit of work */
   }
}

void kb_store_lease_end(void)
{
   if (!g_runtime_provider && (!g_init_thread_set || pthread_equal(pthread_self(), g_init_thread)))
      return;
   if (g_lease_depth > 0 && --g_lease_depth == 0)
   {
      kb_store_thread_lease_t *L =
          (kb_store_thread_lease_t *)pthread_getspecific(g_thread_conn_key);
      if (L && L->conn)
      {
         aimee_pg_close(L->conn);
         L->conn = NULL;
      }
      g_lease_site = NULL;
   }
}

void kb_store_lease_release_idle(void)
{
   /* Release a connection acquired lazily by kb_store_conn() OUTSIDE any
    * kb_store_lease_begin/_end scope (depth 0). Long-lived periodic workers (curator
    * drain, maintenance timer) otherwise pin a pool connection for their whole
    * lifetime — the reaper flags it as a stuck lease and it permanently shrinks
    * the pool. They call this at a job boundary (between cycles) to return the
    * connection while idle. No-op inside a lease scope or on the init thread. */
   if (!g_runtime_provider && (!g_init_thread_set || pthread_equal(pthread_self(), g_init_thread)))
      return;
   if (g_lease_depth != 0)
      return; /* inside an explicit begin/end unit — leave it owned */
   kb_store_thread_lease_t *L = (kb_store_thread_lease_t *)pthread_getspecific(g_thread_conn_key);
   if (L && L->conn)
   {
      aimee_pg_close(L->conn);
      L->conn = NULL;
   }
   g_lease_site = NULL;
}

void *kb_store_thread_conn_open(char *errbuf, size_t errlen)
{
   pthread_once(&g_thread_conn_key_once, thread_conn_key_init);
   if (!g_initialized)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "kb_store not initialized");
      return NULL;
   }
   void *conn = kb_store_scope_connection_open(errbuf, errlen);
   if (!conn)
      return NULL;
   kb_store_thread_lease_t *L = (kb_store_thread_lease_t *)pthread_getspecific(g_thread_conn_key);
   if (!L)
   {
      L = (kb_store_thread_lease_t *)calloc(1, sizeof(*L));
      if (!L)
      {
         aimee_pg_close(conn);
         return NULL;
      }
      if (pthread_setspecific(g_thread_conn_key, L) != 0)
      {
         free(L);
         aimee_pg_close(conn);
         return NULL;
      }
   }
   if (L->conn)
   {
      aimee_pg_close(L->conn);
   }
   L->conn = conn;

   return conn;
}

void kb_store_thread_conn_close(void)
{
   pthread_once(&g_thread_conn_key_once, thread_conn_key_init);
   kb_store_thread_lease_t *L = (kb_store_thread_lease_t *)pthread_getspecific(g_thread_conn_key);
   if (L && L->conn)
   {
      aimee_pg_close(L->conn);
      L->conn = NULL;
   }
}

/* See kb_store_set_schema_readonly in lifecycle.h. */
static int g_schema_readonly;
void kb_store_set_schema_readonly(int on)
{
   g_schema_readonly = on ? 1 : 0;
}

int kb_store_is_initialized(void)
{
   return g_initialized || g_conn ? 1 : 0;
}

static int kb_store_init_mode(const char *libpq_url, int mode)
{
   int runtime = mode == 1 || mode == 3;
   int migration = mode == 2;
   char errbuf[256] = "";

   if (!runtime && !migration && (!libpq_url || !libpq_url[0]))
      return -1;

   pthread_mutex_lock(&g_init_lock);

   if (g_initialized || g_conn)
   {
      int same_url = runtime == g_runtime_provider && migration == g_migration_provider &&
                     (mode == 3) == g_local_runtime_provider &&
                     (runtime || migration || strcmp(g_pg_url, libpq_url) == 0);
      pthread_mutex_unlock(&g_init_lock);
      return same_url ? 0 : -1;
   }

   void *conn = mode == 3   ? aimee_pg_open_configured(errbuf, sizeof(errbuf))
                : runtime   ? aimee_pg_open_runtime(errbuf, sizeof(errbuf))
                : migration ? aimee_pg_open_migration(errbuf, sizeof(errbuf))
                            : aimee_pg_open(libpq_url, errbuf, sizeof(errbuf));
   if (!conn)
   {
      pthread_mutex_unlock(&g_init_lock);
      return -1;
   }

   /* Hardened multi-tenant tier: fail closed at boot unless kb↔Postgres is
    * verify-full TLS (I1) and the connected runtime role is non-owner, non-super,
    * NOBYPASSRLS, no-CREATE (B4) — otherwise the team-scoped RLS is defeated. The
    * dev/personal profile (AIMEE_KB_HARDENED unset) skips these, like the file
    * vault-custody dev mode that holds no live secrets. */
   if (!migration && kb_store_hardening_enabled() && !aimee_pg_is_shim())
   {
      char herr[256] = "";
      /* The PostgreSQL provider already enforced verified TLS at acquisition. */
      if (kb_store_hardening_assert_runtime_role(conn, herr, sizeof(herr)) != 0)
      {
         fprintf(stderr, "aimee-kb: hardened tier runtime-role check failed: %s\n", herr);
         aimee_pg_close(conn);
         pthread_mutex_unlock(&g_init_lock);
         return -1;
      }
   }

   /* The deployment runs a single embedder (0.6b=1024 / 4b=2560); the configured
    * embedding_dim drives the dimension of the KB_STORE vector embedding columns.
    * The dimension is supplied by kb_store_set_embedding_dim() at startup (the server
    * and aimee-kb, which hold the loaded config) so this low-level layer stays
    * config-free; it defaults to 1024 when unset. */
   int configured_dim = kb_store_embedding_dim();
   /* §2a/§2b precedence: pin > recorded > probe > default. When the operator did
    * NOT pin, prefer the recorded kb_meta.schema_embedding_dim (the populated-DB
    * source of truth); on a FRESH DB (nothing recorded) §2b derives the dim from
    * the embedder /health probe under an advisory lock. Pinned + recorded paths
    * keep §2a behavior. A DB read ERROR fails fast (never guess a default). */
   int recorded_dim = 0;
   kb_store_dim_read_t rd =
       g_embed_dim_pinned ? KB_STORE_DIM_ABSENT : kb_store_embedding_dim_read(conn, &recorded_dim);
   if (rd == KB_STORE_DIM_ERROR)
   {
      fprintf(stderr, "aimee: kb_store_init: reading recorded embedding dim failed\n");
      aimee_pg_close(conn);
      pthread_mutex_unlock(&g_init_lock);
      return -1;
   }
   int effective_dim = kb_store_effective_dim(g_embed_dim_pinned, configured_dim,
                                              rd == KB_STORE_DIM_FOUND ? recorded_dim : 0);

   /* Hardened tier: the runtime role cannot apply owner-only DDL. The schema is
    * applied by a separate migrate/owner step; here we VERIFY it (read-only) and
    * skip both the probe leg (which would write a fresh dim) and the apply. The
    * dev/owner path below is unchanged. */
   int pre_provisioned =
       (runtime || (!migration && kb_store_hardening_enabled()) || g_schema_readonly) &&
       !aimee_pg_is_shim();

   /* §2b fresh-DB probe leg: unpinned + nothing recorded + a probe registered. The
    * advisory lock serialises racing kb starts; FAIL-FAST on any probe/lock/read
    * failure and record NOTHING (kb_main retries; never poison the recorded dim). */
   int dim_lock_held = 0, derived_via_probe = 0;
   if (!pre_provisioned && !g_embed_dim_pinned && rd == KB_STORE_DIM_ABSENT && g_embedder_probe)
   {
      /* Wait up to the FULL budget for the lock: a peer mid-bootstrap can take the
       * whole budget (cold embedder), so the waiter must be at least as patient —
       * by the time it acquires (or the peer releases) the dim is recorded and the
       * waiter's double-check below sees it. A genuine budget-long timeout means a
       * stuck/dead holder → fail fast; kb_main.c's bounded retry (24×5s) is the
       * outer safety net so a transient stall self-heals without thrash. */
      int total = g_dim_probe_budget_ms;
      int elapsed = 0;
      int lrc = kb_store_dim_lock_acquire(conn, total, &elapsed);
      if (lrc != 0)
      {
         /* Timed out or a lock query error: another starter may have bootstrapped
          * in the meantime. Re-read; use a now-recorded dim, else fail fast. */
         int rec2 = 0;
         if (kb_store_embedding_dim_read(conn, &rec2) == KB_STORE_DIM_FOUND)
            effective_dim = rec2;
         else
         {
            LOG_WARN("kb_store",
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
         kb_store_dim_read_t rd2 =
             kb_store_embedding_dim_read(conn, &rec2); /* double-check under the lock */
         if (rd2 == KB_STORE_DIM_ERROR)
         {
            kb_store_dim_lock_release(conn);
            aimee_pg_close(conn);
            pthread_mutex_unlock(&g_init_lock);
            return -1;
         }
         if (rd2 == KB_STORE_DIM_FOUND)
         {
            effective_dim = rec2; /* another starter bootstrapped between our reads */
         }
         else
         {
            int probed = 0;
            char perr[KB_STORE_PROBE_ERR_LEN] = "";
            /* Remaining budget after the lock wait, floored so a late acquirer
             * (rare: a crashed peer that never recorded) still gets one attempt. */
            int pbudget = total - elapsed;
            if (pbudget < 1000)
               pbudget = 1000;
            int prc = g_embedder_probe(&probed, pbudget, perr, sizeof(perr));
            /* Bound to a valid vector width: an out-of-range value must NOT fall
             * through to db_apply's clamp-to-1024 (that would record a wrong dim). */
            if (prc != 0 || probed <= 0 || probed > EMBED_MAX_DIM)
            {
               LOG_WARN("kb_store", "embedder dim probe failed/out-of-range (got %d, max %d): %s",
                        probed, EMBED_MAX_DIM, perr[0] ? perr : "(no detail)");
               kb_store_dim_lock_release(conn);
               aimee_pg_close(conn);
               pthread_mutex_unlock(&g_init_lock);
               return -1;
            }
            effective_dim = probed;
            derived_via_probe = 1;
         }
      }
   }

   /* A RECORDED dim is adopted without asking the embedder whether it can produce that
    * width, and that gap is reachable by the documented 0.2 -> 0.3 upgrade: a v0.2.192
    * corpus recorded at 1024 came up under the 384-dim bundled embedder reporting
    * healthy, embed_ok:true and embedding_dim_refused:0, and recorded that embedder's
    * serving identity over the corpus. Postgres then refuses every write ("expected
    * 1024 dimensions, not 384"), so the data is safe and the deployment is inert while
    * claiming to be well — which is the failure this release exists to stop, and which
    * UPGRADING.md already promises does not happen ("refuses to start on drift").
    *
    * Refuse only when the embedder ANSWERS and disagrees. A probe that fails is not
    * evidence of drift: an embedder that is merely slow, or one that reports no width,
    * must keep starting exactly as before. Knowing beats guessing in both directions. */
   if (!derived_via_probe && effective_dim > 0 && g_embedder_probe)
   {
      int probed = 0;
      char perr[KB_STORE_PROBE_ERR_LEN] = "";
      int prc = g_embedder_probe(&probed, g_dim_probe_budget_ms, perr, sizeof(perr));
      if (kb_store_dim_drift_refuses(prc, probed, effective_dim))
      {
         snprintf(errbuf, sizeof(errbuf),
                  "embedder serves %d-dimension vectors but this corpus is recorded at %d. "
                  "Every write would be refused by the vector columns. Point EMBEDDER_URL at "
                  "a %d-dimension embedder, or re-embed the corpus at %d "
                  "(docs/runbooks/change-embedder.md).",
                  probed, effective_dim, effective_dim, probed);
         LOG_ERROR("kb_store", "%s", errbuf);
         if (dim_lock_held)
            kb_store_dim_lock_release(conn);
         aimee_pg_close(conn);
         pthread_mutex_unlock(&g_init_lock);
         return -1;
      }
   }

   if (effective_dim != configured_dim)
   {
      if (derived_via_probe)
         LOG_WARN("kb_store", "fresh DB: derived embedding dim %d from the embedder /health probe",
                  effective_dim);
      else
         LOG_WARN("kb_store",
                  "using recorded embedding dim %d (no operator pin; configured default was %d)",
                  effective_dim, configured_dim);
      kb_store_set_embedding_dim(effective_dim); /* vector columns + all readers agree */
   }
   int apply_rc;
   if (pre_provisioned)
   {
      /* Verify the owner-migrated schema is present + compatible; never apply DDL. */
      char verr[256] = "";
      apply_rc = kb_store_verify_pre_provisioned(conn, effective_dim, verr, sizeof(verr));
      if (apply_rc != 0)
         snprintf(errbuf, sizeof(errbuf), "%s", verr);
   }
   else
   {
      apply_rc = db_apply_schema_postgres(conn, effective_dim, errbuf, sizeof(errbuf));
   }
   if (apply_rc != 0)
   {
      /* Surface the postgres error so callers see WHICH statement failed (apply
       * path), or the fail-closed reason (hardened verify path — no DDL was run).
       * Silently returning -1 hid bugs like a stale CREATE INDEX referencing a
       * table that lives in a different tier's schema. */
      fprintf(stderr, "aimee: kb_store_init: %s: %s\n",
              pre_provisioned ? "hardened schema verification failed" : "schema apply failed",
              errbuf);
      if (dim_lock_held)
         kb_store_dim_lock_release(conn);
      aimee_pg_close(conn);
      pthread_mutex_unlock(&g_init_lock);
      return -1;
   }
   /* Schema (incl. the vector columns) is now applied AND the dim recorded last
    * (record-after-DDL), so the lock can release: a later starter reads the dim. */
   if (dim_lock_held)
      kb_store_dim_lock_release(conn);

   /* unified-llm-container §2: model-identity drift guard, applied here (where the
    * configured identity globals live) rather than in the lower db_schema layer.
    * Record/check the EMBEDDER model identity alongside the dim — closing the
    * same-dim different-model footgun (two models can share a dim). A no-op when
    * the identity is unset (the legacy torch embedder reports none), so existing
    * deployments are unaffected; it activates when the unified container supplies
    * the identity via the setter. */
   /* Ask for the serving identity here, not at startup: the embedder runs beside the kb
    * and is not up yet when the kb boots. An unreachable probe leaves the identity empty,
    * which makes the guard a no-op for this start rather than blocking the boot. */
   if (!g_embedder_serving_id[0] && g_embedder_serving_probe)
   {
      char sid[160] = "";
      char perr[192] = "";
      if (g_embedder_serving_probe(sid, sizeof(sid), perr, sizeof(perr)) == 0)
         kb_store_set_embedder_serving_id(sid);
      else
         fprintf(stderr,
                 "aimee: embedder serving-identity probe failed (%s); vector-space "
                 "guard inactive for this start\n",
                 perr[0] ? perr : "unreachable");
   }
   if (kb_store_embedding_model_record_or_check(conn, g_embedder_model_id, g_embedding_compat,
                                                errbuf, sizeof(errbuf)) != 0 ||
       kb_store_embedder_serving_record_or_check(conn, g_embedder_serving_id, errbuf,
                                                 sizeof(errbuf)) != 0)
   {
      fprintf(stderr, "aimee: kb_store_init: model-identity guard failed: %s\n", errbuf);
      aimee_pg_close(conn);
      pthread_mutex_unlock(&g_init_lock);
      return -1;
   }

   /* pg_trgm is required: kb_store/schema.sql creates a GIN index on
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
         fprintf(stderr,
                 "aimee: kb_store_init: pg_trgm extension is not installed in this database. "
                 "Enable it with: CREATE EXTENSION pg_trgm;  (requires a role with the "
                 "appropriate privileges)\n");
         return -1;
      }
   }

   g_conn = conn;
   /* Record the owning thread: kb_store_conn() hands g_conn to this thread and a
    * private per-thread connection to every other thread. */
   g_init_thread = pthread_self();
   g_init_thread_set = 1;
   snprintf(g_pg_url, sizeof(g_pg_url), "%s", libpq_url ? libpq_url : "");

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
    * reaches Postgres through kb_store_conn(), which returns g_conn only once the init
    * thread is set. Running it earlier (the original site, before g_conn was
    * assigned) made kb_store_conn() return NULL, so the build silently failed and the
    * index was never created on any instance — graph-code fusion's whole
    * substrate produced zero edges. */
   if (!aimee_pg_is_shim())
   {
      int ee_idx_existed = 0;
      if (kb_store_entity_edge_build_unique_index(&ee_idx_existed) != 0)
         fprintf(stderr, "aimee: kb_store_init: entity_edges unique index not built; code-graph "
                         "projection ON CONFLICT will no-op until duplicate triples are deduped\n");
   }
   if (runtime)
   {
      pthread_once(&g_thread_conn_key_once, thread_conn_key_init);
      kb_store_thread_lease_t *lease = calloc(1, sizeof(*lease));
      if (!lease || pthread_setspecific(g_thread_conn_key, lease))
      {
         free(lease);
         aimee_pg_close(g_conn);
         g_conn = NULL;
         pthread_mutex_unlock(&g_init_lock);
         return -1;
      }
      lease->conn = g_conn;
      g_conn = NULL;
   }
   g_runtime_provider = runtime;
   g_migration_provider = migration;
   g_local_runtime_provider = mode == 3;
   g_initialized = 1;
   pthread_mutex_unlock(&g_init_lock);
   return 0;
}
int kb_store_init(const char *authority)
{
   return kb_store_init_mode(authority, 0);
}
int kb_store_init_configured(void)
{
   return kb_store_init_mode(NULL, 3);
}
int kb_store_init_migration(void)
{
   return kb_store_init_mode(NULL, 2);
}
int kb_store_init_runtime(void)
{
   return kb_store_init_mode(NULL, 1);
}

int kb_store_health_probe(int *schema_ok, int *have_pg_trgm)
{
   char errbuf[256] = "";

   if (schema_ok)
      *schema_ok = 0;
   if (have_pg_trgm)
      *have_pg_trgm = 0;

   /* Health endpoints run on worker threads while the init thread performs
    * periodic maintenance. Never bypass kb_store_conn() here: sharing g_conn with
    * the checkpoint transaction lets concurrent libpq calls exchange results
    * and can leave the owner connection transaction-aborted. */
   void *conn = kb_store_conn();
   if (!conn)
      return -1;

   if (aimee_pg_exec(conn, "SELECT 1", errbuf, sizeof(errbuf)) != 0)
      return -1;

   int schema_present =
       kb_store_query_flag(conn,
                           "SELECT 1 FROM information_schema.tables "
                           "WHERE table_schema = current_schema() AND table_name = :t",
                           "t", "memories");
   if (schema_present < 0)
      return -1;
   if (schema_ok)
      *schema_ok = schema_present;

   /* pg_trgm is required by kb_store_init; reporting its presence here for
    * doctor/diagnostics. The shim has no pg_extension catalog, so
    * report installed=1 unconditionally under the shim (doctor path
    * never runs against the shim in production but tests can call
    * health_probe). */
   int ext_present;
   if (aimee_pg_is_shim())
      ext_present = 1;
   else
      ext_present = kb_store_query_flag(
          conn, "SELECT 1 FROM pg_extension WHERE extname = 'pg_trgm'", NULL, NULL);
   if (ext_present < 0)
      return -1;
   if (have_pg_trgm)
      *have_pg_trgm = ext_present;

   return 0;
}

int kb_store_kb_health_probe(int *kb_tables_ok)
{
   if (kb_tables_ok)
      *kb_tables_ok = 0;
   void *conn = kb_store_conn();
   if (!conn)
      return -1;
   int docs_ok = kb_store_query_flag(conn,
                                     "SELECT 1 FROM information_schema.tables "
                                     "WHERE table_schema = current_schema() AND table_name = :t",
                                     "t", "kb_documents");
   int jobs_ok = kb_store_query_flag(conn,
                                     "SELECT 1 FROM information_schema.tables "
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
int kb_store_pg_stat_summary(int *active_conns, int *max_conns, int *is_replica,
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

   void *conn = kb_store_conn();
   if (!conn)
      return -1;

   if (aimee_pg_is_shim())
      return 0;

   char errbuf[256] = "";
   aimee_pg_stmt_t *st = NULL;

   if (active_conns)
   {
      st = aimee_pg_prepare(conn,
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
      st = aimee_pg_prepare(conn, "SELECT current_setting('max_connections')::int", errbuf,
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
      st = aimee_pg_prepare(conn, "SELECT pg_is_in_recovery()::int", errbuf, sizeof(errbuf));
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
          conn,
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

void kb_store_shutdown(void)
{
   kb_store_thread_conn_close();
   pthread_mutex_lock(&g_init_lock);
   if (g_conn)
   {
      aimee_pg_close(g_conn);
      g_conn = NULL;
   }
   g_pg_url[0] = '\0';
   g_initialized = 0;
   g_runtime_provider = 0;
   g_migration_provider = 0;
   g_local_runtime_provider = 0;
   /* §2a: clear the dim + pinned flag so a reopen / a later unit test starts from
    * the unpinned default rather than inheriting this run's state. kb_store_init may
    * have re-set g_embed_dim to a recorded value, so reset it for symmetry — every
    * real caller sets it again via kb_store_set_embedding_dim before the next init. */
   g_embed_dim = 0;
   g_embed_dim_pinned = 0;
   /* §2b: defensively clear the probe seam + budget (the caller SHOULD deregister
    * before kb_store_shutdown, but back it up here, mirroring g_embed_dim_pinned). */
   g_embedder_probe = NULL;
   g_dim_probe_budget_ms = 120000;
   g_embedder_model_id[0] = '\0';
   g_embedder_serving_id[0] = '\0';
   g_embedder_serving_probe = NULL;
   g_embedding_compat[0] = '\0';
   pthread_mutex_unlock(&g_init_lock);
}

/* --- Generic scalar/exec convenience helpers ------------------------------
 * See kb_store_internal.h for contract. These collapse the prepare/bind/step/
 * finalize boilerplate that recurred verbatim across the per-column getters,
 * counters, and fire-and-forget setters in the kb_store sources. */

#define KB_STOREQ_ERRBUF 256

int kb_store_scalar_int(const char *sql, int dflt)
{
   void *conn = kb_store_conn();
   if (!conn)
      return dflt;
   char err[KB_STOREQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return dflt;
   int value = dflt;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      value = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return value;
}

int kb_store_scalar_int_text(const char *sql, const char *a1, int dflt)
{
   void *conn = kb_store_conn();
   if (!conn)
      return dflt;
   char err[KB_STOREQ_ERRBUF] = "";
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

double kb_store_scalar_double_id(const char *sql, int64_t id, double dflt)
{
   void *conn = kb_store_conn();
   if (!conn)
      return dflt;
   char err[KB_STOREQ_ERRBUF] = "";
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

void kb_store_exec_id(const char *sql, int64_t id)
{
   void *conn = kb_store_conn();
   if (!conn)
      return;
   char err[KB_STOREQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int kb_store_exec_conn_int64(void *conn, const char *sql, int64_t arg)
{
   if (!conn)
      return -1;
   char err[KB_STOREQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", arg);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

void kb_store_exec_text(const char *sql, const char *a1)
{
   void *conn = kb_store_conn();
   if (!conn)
      return;
   char err[KB_STOREQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", a1);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void kb_store_exec_text_id(const char *sql, const char *a1, int64_t id)
{
   void *conn = kb_store_conn();
   if (!conn)
      return;
   char err[KB_STOREQ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", a1);
   aimee_pg_bind_int64(st, "?2", id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void kb_store_copy_text(char *dst, size_t cap, const char *src)
{
   if (!dst || cap == 0)
      return;
   snprintf(dst, cap, "%s", src ? src : "");
}

void kb_store_copy_col_text(char *dst, size_t cap, aimee_pg_stmt_t *st, int col)
{
   const char *value = aimee_pg_column_text(st, col);
   snprintf(dst, cap, "%s", value ? value : "");
}
