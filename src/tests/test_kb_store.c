#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "db_postgres.h"
#include "modules/kb/c/kb_store.h"
#include "modules/kb/c/kb_store_internal.h"
#include "modules/kb/c/db_schema.h" /* §2b: kb_store_dim_read_t / KB_STORE_DIM_* for the dim-read stub */
#include "../headers/log.h"         /* log_level_t for the aimee_log stub below */
#include <stdarg.h>

struct aimee_pg_stmt
{
   int kind;
};

enum
{
   STMT_SCHEMA = 1,
   STMT_EXT = 2,
   STMT_INDEX = 3,
};

static int g_open_calls = 0;
static int g_close_calls = 0;
static int g_schema_calls = 0;
static int g_exec_calls = 0;
static int g_prepare_calls = 0;
static int g_finalize_calls = 0;
static int g_fail_open = 0;
static int g_fail_schema = 0;
static int g_fail_exec = 0;
static int g_fail_prepare = 0;
static int g_schema_present = 1;
static int g_extension_present = 1;
static int g_fake_conn = 0;
static int g_worker_conn = 0;
static int g_distinct_worker_conn = 0;
static void *g_last_exec_conn = NULL;
/* §2a: knob for the kb_store_embedding_dim_get stub (the recorded kb_meta dim kb_store_init
 * reads) and a capture of the embed_dim db_apply_schema_postgres actually received
 * — together they exercise the recorded-dim precedence wiring through kb_store_init. */
static int g_recorded_dim = 0;
static int g_schema_dim = -1;

void *aimee_pg_open(const char *conninfo, char *errbuf, size_t errlen)
{
   g_open_calls++;
   assert(conninfo != NULL);
   assert(strcmp(conninfo, "postgres://kb_store.test/aimee") == 0);
   if (g_fail_open)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "%s", "open failed");
      return NULL;
   }
   return (g_distinct_worker_conn && g_open_calls > 1) ? (void *)&g_worker_conn
                                                       : (void *)&g_fake_conn;
}

void *aimee_pg_open_runtime(char *error, size_t capacity)
{
   return aimee_pg_open("postgres://kb_store.test/aimee", error, capacity);
}
void *aimee_pg_open_configured(char *error, size_t capacity)
{
   return aimee_pg_open_runtime(error, capacity);
}
void *aimee_pg_open_migration(char *error, size_t capacity)
{
   return aimee_pg_open_runtime(error, capacity);
}
void aimee_pg_close(void *pg_conn)
{
   g_close_calls++;
   assert(pg_conn == &g_fake_conn || pg_conn == &g_worker_conn);
}

int db_apply_schema_postgres(void *pg_conn, int embed_dim, char *errbuf, size_t errlen)
{
   g_schema_calls++;
   g_schema_dim = embed_dim; /* §2a: capture the effective dim kb_store_init resolved */
   assert(pg_conn == &g_fake_conn);
   if (g_fail_schema)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "%s", "schema failed");
      return -1;
   }
   return 0;
}

/* §2a: kb_store_init now reads the recorded dim (db_schema.o, not linked here — the
 * real db_apply_schema_postgres is stubbed above) and logs via aimee_log (log.o,
 * not linked). Stub both so the object links; returning 0 (no recorded dim) keeps
 * the unpinned path's effective dim == configured, so these tests are unchanged. */
int kb_store_embedding_dim_get(void *pg_conn)
{
   assert(pg_conn == &g_fake_conn);
   return g_recorded_dim;
}

/* §2b: kb_store_init now reads the recorded dim via the tri-state reader. Mirror the
 * get stub off the same g_recorded_dim knob: >0 → FOUND (recorded wins), else
 * ABSENT (fresh DB). With no probe registered (the default here) kb_store_init's §2b
 * block is skipped, so these tests stay on the §2a path — unchanged. */
kb_store_dim_read_t kb_store_embedding_dim_read(void *pg_conn, int *out)
{
   assert(pg_conn == &g_fake_conn);
   if (g_recorded_dim > 0)
   {
      if (out)
         *out = g_recorded_dim;
      return KB_STORE_DIM_FOUND;
   }
   return KB_STORE_DIM_ABSENT;
}

/* unified-llm-container §2: kb_store_init now also calls the model-identity guards
 * (db_schema.o, not linked here). Stub them as no-ops (the real guards likewise
 * no-op on the empty identity these tests run with), so kb_store_init's apply path is
 * unchanged. */
int kb_store_embedding_model_record_or_check(void *pg_conn, const char *model_id,
                                             const char *compat_csv, char *errbuf, size_t errlen)
{
   (void)model_id;
   (void)compat_csv;
   (void)errbuf;
   (void)errlen;
   assert(pg_conn == &g_fake_conn);
   return 0;
}

int kb_store_embedder_serving_record_or_check(void *pg_conn, const char *serving_id, char *errbuf,
                                              size_t errlen)
{
   (void)serving_id;
   (void)errbuf;
   (void)errlen;
   assert(pg_conn == &g_fake_conn);
   return 0;
}

void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}

int aimee_pg_exec(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   g_exec_calls++;
   assert(pg_conn == &g_fake_conn || pg_conn == &g_worker_conn);
   g_last_exec_conn = pg_conn;
   assert(strcmp(sql, "SELECT 1") == 0);
   if (g_fail_exec)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "%s", "exec failed");
      return -1;
   }
   return 0;
}

/* Session transaction state for the lifecycle fixture. */
int aimee_pg_in_transaction(void *pg_conn)
{
   (void)pg_conn;
   return 0;
}

aimee_pg_stmt_t *aimee_pg_prepare(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   static aimee_pg_stmt_t schema_stmt = {.kind = STMT_SCHEMA};
   static aimee_pg_stmt_t ext_stmt = {.kind = STMT_EXT};
   static aimee_pg_stmt_t index_stmt = {.kind = STMT_INDEX};

   g_prepare_calls++;
   assert(pg_conn == &g_fake_conn || pg_conn == &g_worker_conn);
   if (g_fail_prepare)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "%s", "prepare failed");
      return NULL;
   }
   if (strstr(sql, "information_schema.tables") != NULL)
      return &schema_stmt;
   /* Both kb_store_init's pg_trgm enforcement and kb_store_health_probe's
    * extension report query against pg_extension. Tests treat them
    * uniformly through STMT_EXT and the g_extension_present knob. */
   if (strstr(sql, "pg_extension") != NULL || strstr(sql, "pg_available_extensions") != NULL)
      return &ext_stmt;
   /* kb_store_init builds the entity_edges (source,relation,target) unique index
    * (needed by the code-graph projection's ON CONFLICT). It first probes
    * pg_indexes for idx_ee_unique_triple; this mock reports the index already
    * present so the build short-circuits (no CREATE exec). */
   if (strstr(sql, "pg_indexes") != NULL)
      return &index_stmt;
   assert(!"unexpected SQL");
   return NULL;
}

int aimee_pg_is_shim(void)
{
   /* Tests exercise the production pg_extension enforcement path; the
    * shim short-circuit is covered separately by the in-memory shim
    * unit tests. */
   return 0;
}

void aimee_pg_finalize(aimee_pg_stmt_t *stmt)
{
   assert(stmt != NULL);
   g_finalize_calls++;
}

aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *stmt, char *errbuf, size_t errlen)
{
   (void)errbuf;
   (void)errlen;
   assert(stmt != NULL);
   if (stmt->kind == STMT_SCHEMA)
      return g_schema_present ? AIMEE_PG_ROW : AIMEE_PG_DONE;
   if (stmt->kind == STMT_EXT)
      return g_extension_present ? AIMEE_PG_ROW : AIMEE_PG_DONE;
   if (stmt->kind == STMT_INDEX)
      return AIMEE_PG_ROW; /* idx_ee_unique_triple already present */
   assert(!"unexpected statement kind");
   return AIMEE_PG_ERR;
}

int aimee_pg_bind_text(aimee_pg_stmt_t *stmt, const char *name, const char *value)
{
   assert(stmt != NULL);
   assert(stmt->kind == STMT_SCHEMA);
   assert(strcmp(name, "t") == 0);
   assert(strcmp(value, "memories") == 0 || strcmp(value, "kb_documents") == 0 ||
          strcmp(value, "kb_async_jobs") == 0);
   return 0;
}

/* §2b: kb_store_init's advisory-lock + probe path pulls these aimee_pg accessors into
 * kb_store_init.o's kept symbol set. They are NEVER called on this test's path (no
 * embedder probe is registered, so the §2b block is skipped), so trivial stubs
 * suffice for linking. */
int aimee_pg_bind_int64(aimee_pg_stmt_t *stmt, const char *name, int64_t value)
{
   (void)stmt;
   (void)name;
   (void)value;
   return 0;
}
int aimee_pg_column_int(aimee_pg_stmt_t *stmt, int col)
{
   (void)stmt;
   (void)col;
   return 0;
}
int64_t aimee_pg_column_int64(aimee_pg_stmt_t *stmt, int col)
{
   (void)stmt;
   (void)col;
   return 0;
}
double aimee_pg_column_double(aimee_pg_stmt_t *stmt, int col)
{
   (void)stmt;
   (void)col;
   return 0.0;
}
const char *aimee_pg_column_text(aimee_pg_stmt_t *stmt, int col)
{
   (void)stmt;
   (void)col;
   return "";
}

static void reset_mocks(void)
{
   g_open_calls = 0;
   g_close_calls = 0;
   g_schema_calls = 0;
   g_exec_calls = 0;
   g_prepare_calls = 0;
   g_finalize_calls = 0;
   g_fail_open = 0;
   g_fail_schema = 0;
   g_fail_exec = 0;
   g_fail_prepare = 0;
   g_schema_present = 1;
   g_extension_present = 1;
   g_distinct_worker_conn = 0;
   g_last_exec_conn = NULL;
   g_recorded_dim = 0;
   g_schema_dim = -1;
   kb_store_shutdown();
   kb_store_set_embedding_dim(0);
   kb_store_set_embedding_dim_pinned(0);
}

static void test_init_shutdown_roundtrip(void)
{
   reset_mocks();

   assert(kb_store_init("postgres://kb_store.test/aimee") == 0);
   assert(kb_store_conn() == &g_fake_conn);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 1);
   assert(g_close_calls == 0);

   assert(kb_store_init("postgres://kb_store.test/aimee") == 0);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 1);

   kb_store_shutdown();
   assert(kb_store_conn() == NULL);
   assert(g_close_calls == 1);

   kb_store_shutdown();
   assert(g_close_calls == 1);
}

/* §2a: drive the recorded-dim precedence through kb_store_init end-to-end (the wiring
 * the shim/unit helpers don't exercise). g_schema_dim captures the effective dim
 * that reached db_apply_schema_postgres; g_recorded_dim mocks kb_meta. */
static void test_recorded_dim_precedence(void)
{
   /* Unpinned + a recorded dim that differs from the configured default: the
    * recorded dim wins and the global is updated so later readers agree. */
   reset_mocks();
   kb_store_set_embedding_dim(1024); /* configured default */
   g_recorded_dim = 2560;            /* populated DB recorded 2560 */
   assert(kb_store_init("postgres://kb_store.test/aimee") == 0);
   assert(g_schema_dim == 2560);             /* effective dim, not the 1024 default */
   assert(kb_store_embedding_dim() == 2560); /* global re-set so halfvec + readers agree */

   /* Pinned: the operator value is authoritative; the recorded dim is ignored. */
   reset_mocks();
   kb_store_set_embedding_dim(1024);
   kb_store_set_embedding_dim_pinned(1);
   g_recorded_dim = 2560;
   assert(kb_store_init("postgres://kb_store.test/aimee") == 0);
   assert(g_schema_dim == 1024); /* pin wins over recorded */
   assert(kb_store_embedding_dim() == 1024);

   /* Unpinned, nothing recorded: the configured default stands (fresh-DB path). */
   reset_mocks();
   kb_store_set_embedding_dim(1024);
   g_recorded_dim = 0;
   assert(kb_store_init("postgres://kb_store.test/aimee") == 0);
   assert(g_schema_dim == 1024);

   /* kb_store_shutdown clears the pinned flag: a pin set before shutdown must NOT leak
    * into the next init, or an unpinned deploy would wrongly refuse to self-derive. */
   reset_mocks();
   kb_store_set_embedding_dim_pinned(1);
   kb_store_shutdown(); /* resets g_embed_dim_pinned (and g_embed_dim) */
   kb_store_set_embedding_dim(1024);
   g_recorded_dim = 2560;
   assert(kb_store_init("postgres://kb_store.test/aimee") == 0);
   assert(g_schema_dim == 2560); /* pinned state did not survive shutdown */
   kb_store_shutdown();
}

static void test_init_rejects_url_change_without_shutdown(void)
{
   reset_mocks();

   assert(kb_store_init("postgres://kb_store.test/aimee") == 0);
   assert(kb_store_conn() == &g_fake_conn);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 1);

   assert(kb_store_init("postgres://kb_store.test/other") == -1);
   assert(kb_store_conn() == &g_fake_conn);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 1);
   assert(g_close_calls == 0);

   kb_store_shutdown();
   assert(kb_store_conn() == NULL);
   assert(g_close_calls == 1);
}

static void test_init_rejects_empty_url(void)
{
   reset_mocks();

   assert(kb_store_init(NULL) == -1);
   assert(kb_store_init("") == -1);
   assert(g_open_calls == 0);
   assert(g_schema_calls == 0);
   assert(g_close_calls == 0);
   assert(kb_store_conn() == NULL);
}

static void test_open_failure_leaves_kb_store_closed(void)
{
   reset_mocks();
   g_fail_open = 1;

   assert(kb_store_init("postgres://kb_store.test/aimee") == -1);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 0);
   assert(g_close_calls == 0);
   assert(kb_store_conn() == NULL);
}

static void test_schema_failure_closes_connection(void)
{
   reset_mocks();
   g_fail_schema = 1;

   assert(kb_store_init("postgres://kb_store.test/aimee") == -1);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 1);
   assert(g_close_calls == 1);
   assert(kb_store_conn() == NULL);
}

static void test_health_probe_reports_schema_and_extension(void)
{
   reset_mocks();

   assert(kb_store_init("postgres://kb_store.test/aimee") == 0);
   /* kb_store_init does one prepare/finalize for the pg_trgm enforcement
    * check; the per-init exec is the SELECT 1 schema-apply path
    * exercised through the schema mock. */
   const int init_prepares = g_prepare_calls;
   const int init_finalizes = g_finalize_calls;

   int schema_ok = 0;
   int have_pg_trgm = 0;
   assert(kb_store_health_probe(&schema_ok, &have_pg_trgm) == 0);
   assert(schema_ok == 1);
   assert(have_pg_trgm == 1);
   /* health_probe issues SELECT 1 (exec) + 2 prepare/finalize cycles
    * (schema-present check, pg_trgm presence check). */
   assert(g_exec_calls == 1);
   assert(g_prepare_calls - init_prepares == 2);
   assert(g_finalize_calls - init_finalizes == 2);
}

static void test_init_fails_without_pg_trgm(void)
{
   /* kb_store_init now requires pg_trgm to be installed; absence is a hard
    * failure rather than the warn-and-continue contract that
    * kb_store_health_probe used to use. */
   reset_mocks();
   /* reset_mocks zeros counters then calls kb_store_shutdown; if g_conn was
    * left set by a prior test, that shutdown closes the fake conn and
    * bumps g_close_calls. Snapshot the post-reset close count so we
    * verify only this test's contribution. */
   const int close_baseline = g_close_calls;
   g_extension_present = 0;

   assert(kb_store_init("postgres://kb_store.test/aimee") == -1);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 1);
   assert(g_close_calls - close_baseline == 1);
   assert(kb_store_conn() == NULL);
}

static void test_health_probe_fails_without_init_or_query_failure(void)
{
   reset_mocks();

   int schema_ok = 0;
   int have_pg_trgm = 0;
   assert(kb_store_health_probe(&schema_ok, &have_pg_trgm) == -1);

   assert(kb_store_init("postgres://kb_store.test/aimee") == 0);
   g_fail_exec = 1;
   assert(kb_store_health_probe(&schema_ok, &have_pg_trgm) == -1);

   reset_mocks();
   assert(kb_store_init("postgres://kb_store.test/aimee") == 0);
   g_fail_prepare = 1;
   assert(kb_store_health_probe(&schema_ok, &have_pg_trgm) == -1);
}

static void *acquire_from_worker(void *arg)
{
   *(void **)arg = kb_store_conn();
   return NULL;
}

typedef struct
{
   int rc;
   int schema_ok;
   int tables_ok;
} worker_health_result_t;

static void *probe_from_worker(void *arg)
{
   worker_health_result_t *result = arg;
   int have_pg_trgm = 0;
   result->rc = kb_store_health_probe(&result->schema_ok, &have_pg_trgm);
   if (result->rc == 0 && have_pg_trgm)
      result->rc = kb_store_kb_health_probe(&result->tables_ok);
   return NULL;
}

static void test_worker_health_probe_never_shares_init_connection(void)
{
   reset_mocks();
   g_distinct_worker_conn = 1;
   assert(kb_store_init("postgres://kb_store.test/aimee") == 0);

   worker_health_result_t result = {0};
   pthread_t worker;
   assert(pthread_create(&worker, NULL, probe_from_worker, &result) == 0);
   assert(pthread_join(worker, NULL) == 0);

   assert(result.rc == 0);
   assert(result.schema_ok == 1);
   assert(result.tables_ok == 1);
   assert(g_open_calls == 2); /* owner plus the worker's lazy pool member */
   assert(g_last_exec_conn == &g_worker_conn);

   kb_store_shutdown();
}

static void test_worker_acquire_failure_never_shares_init_connection(void)
{
   reset_mocks();
   assert(kb_store_init("postgres://kb_store.test/aimee") == 0);

   /* Simulate an unavailable pool and a failed overflow connection. Sharing the
    * init thread's PGconn here corrupts libpq under concurrent load. */
   g_fail_open = 1;
   void *worker_conn = &g_fake_conn;
   pthread_t worker;
   assert(pthread_create(&worker, NULL, acquire_from_worker, &worker_conn) == 0);
   pthread_join(worker, NULL);
   assert(worker_conn == NULL);
   assert(kb_store_conn() == &g_fake_conn); /* the owning init thread keeps its handle */
   kb_store_shutdown();
}

static void test_migration_authority_is_not_leased_to_workers(void)
{
   reset_mocks();
   assert(kb_store_init_migration() == 0);
   assert(kb_store_conn() == &g_fake_conn);
   int opens = g_open_calls;
   void *worker_conn = &g_fake_conn;
   pthread_t worker;
   assert(pthread_create(&worker, NULL, acquire_from_worker, &worker_conn) == 0);
   assert(pthread_join(worker, NULL) == 0);
   assert(worker_conn == NULL);
   assert(kb_store_scope_connection_open(NULL, 0) == NULL);
   assert(g_open_calls == opens);
   kb_store_shutdown();
}

int main(void)
{
   test_migration_authority_is_not_leased_to_workers();
   test_init_shutdown_roundtrip();
   test_recorded_dim_precedence();
   test_init_rejects_url_change_without_shutdown();
   test_init_rejects_empty_url();
   test_open_failure_leaves_kb_store_closed();
   test_schema_failure_closes_connection();
   test_health_probe_reports_schema_and_extension();
   test_init_fails_without_pg_trgm();
   test_health_probe_fails_without_init_or_query_failure();
   test_worker_health_probe_never_shares_init_connection();
   test_worker_acquire_failure_never_shares_init_connection();
   printf("kb_store: all tests passed\n");
   return 0;
}
