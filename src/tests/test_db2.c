#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "db_postgres.h"
#include "db2.h"
#include "db2_internal.h"

struct aimee_pg_stmt
{
   int kind;
};

enum
{
   STMT_SCHEMA = 1,
   STMT_EXT = 2,
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

void *aimee_pg_open(const char *conninfo, char *errbuf, size_t errlen)
{
   g_open_calls++;
   assert(conninfo != NULL);
   assert(strcmp(conninfo, "postgres://db2.test/aimee") == 0);
   if (g_fail_open)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "%s", "open failed");
      return NULL;
   }
   return &g_fake_conn;
}

void aimee_pg_close(void *pg_conn)
{
   g_close_calls++;
   assert(pg_conn == &g_fake_conn);
}

int db_apply_schema_postgres(void *pg_conn, int embed_dim, char *errbuf, size_t errlen)
{
   g_schema_calls++;
   (void)embed_dim;
   assert(pg_conn == &g_fake_conn);
   if (g_fail_schema)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "%s", "schema failed");
      return -1;
   }
   return 0;
}

int aimee_pg_exec(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   g_exec_calls++;
   assert(pg_conn == &g_fake_conn);
   assert(strcmp(sql, "SELECT 1") == 0);
   if (g_fail_exec)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "%s", "exec failed");
      return -1;
   }
   return 0;
}

/* member_reset_real (in db2_pool.o, not exercised by these pool tests — they shim
 * g_reset) references this; provide a stub so the object links. */
int aimee_pg_in_transaction(void *pg_conn)
{
   (void)pg_conn;
   return 0;
}

aimee_pg_stmt_t *aimee_pg_prepare(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   static aimee_pg_stmt_t schema_stmt = {.kind = STMT_SCHEMA};
   static aimee_pg_stmt_t ext_stmt = {.kind = STMT_EXT};

   g_prepare_calls++;
   assert(pg_conn == &g_fake_conn);
   if (g_fail_prepare)
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "%s", "prepare failed");
      return NULL;
   }
   if (strstr(sql, "information_schema.tables") != NULL)
      return &schema_stmt;
   /* Both db2_init's pg_trgm enforcement and db2_health_probe's
    * extension report query against pg_extension. Tests treat them
    * uniformly through STMT_EXT and the g_extension_present knob. */
   if (strstr(sql, "pg_extension") != NULL || strstr(sql, "pg_available_extensions") != NULL)
      return &ext_stmt;
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
   assert(!"unexpected statement kind");
   return AIMEE_PG_ERR;
}

int aimee_pg_bind_text(aimee_pg_stmt_t *stmt, const char *name, const char *value)
{
   assert(stmt != NULL);
   assert(stmt->kind == STMT_SCHEMA);
   assert(strcmp(name, "t") == 0);
   assert(strcmp(value, "memories") == 0);
   return 0;
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
   db2_shutdown();
}

static void test_init_shutdown_roundtrip(void)
{
   reset_mocks();
   char conn_url[128] = "stale";

   assert(db2_init("postgres://db2.test/aimee") == 0);
   assert(db2_conn() == &g_fake_conn);
   assert(db2_fork_conn_url(conn_url, sizeof(conn_url)) == 1);
   assert(strcmp(conn_url, "postgres://db2.test/aimee") == 0);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 1);
   assert(g_close_calls == 0);

   assert(db2_init("postgres://db2.test/aimee") == 0);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 1);

   db2_shutdown();
   assert(db2_conn() == NULL);
   assert(db2_fork_conn_url(conn_url, sizeof(conn_url)) == 0);
   assert(conn_url[0] == '\0');
   assert(g_close_calls == 1);

   db2_shutdown();
   assert(g_close_calls == 1);
}

static void test_init_rejects_url_change_without_shutdown(void)
{
   reset_mocks();

   assert(db2_init("postgres://db2.test/aimee") == 0);
   assert(db2_conn() == &g_fake_conn);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 1);

   assert(db2_init("postgres://db2.test/other") == -1);
   assert(db2_conn() == &g_fake_conn);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 1);
   assert(g_close_calls == 0);

   db2_shutdown();
   assert(db2_conn() == NULL);
   assert(g_close_calls == 1);
}

static void test_init_rejects_empty_url(void)
{
   reset_mocks();

   assert(db2_init(NULL) == -1);
   assert(db2_init("") == -1);
   assert(g_open_calls == 0);
   assert(g_schema_calls == 0);
   assert(g_close_calls == 0);
   assert(db2_conn() == NULL);
}

static void test_open_failure_leaves_db2_closed(void)
{
   reset_mocks();
   g_fail_open = 1;

   assert(db2_init("postgres://db2.test/aimee") == -1);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 0);
   assert(g_close_calls == 0);
   assert(db2_conn() == NULL);
}

static void test_schema_failure_closes_connection(void)
{
   reset_mocks();
   g_fail_schema = 1;

   assert(db2_init("postgres://db2.test/aimee") == -1);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 1);
   assert(g_close_calls == 1);
   assert(db2_conn() == NULL);
}

static void test_health_probe_reports_schema_and_extension(void)
{
   reset_mocks();

   assert(db2_init("postgres://db2.test/aimee") == 0);
   /* db2_init does one prepare/finalize for the pg_trgm enforcement
    * check; the per-init exec is the SELECT 1 schema-apply path
    * exercised through the schema mock. */
   const int init_prepares = g_prepare_calls;
   const int init_finalizes = g_finalize_calls;

   int schema_ok = 0;
   int have_pg_trgm = 0;
   assert(db2_health_probe(&schema_ok, &have_pg_trgm) == 0);
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
   /* db2_init now requires pg_trgm to be installed; absence is a hard
    * failure rather than the warn-and-continue contract that
    * db2_health_probe used to use. */
   reset_mocks();
   /* reset_mocks zeros counters then calls db2_shutdown; if g_conn was
    * left set by a prior test, that shutdown closes the fake conn and
    * bumps g_close_calls. Snapshot the post-reset close count so we
    * verify only this test's contribution. */
   const int close_baseline = g_close_calls;
   g_extension_present = 0;

   assert(db2_init("postgres://db2.test/aimee") == -1);
   assert(g_open_calls == 1);
   assert(g_schema_calls == 1);
   assert(g_close_calls - close_baseline == 1);
   assert(db2_conn() == NULL);
}

static void test_health_probe_fails_without_init_or_query_failure(void)
{
   reset_mocks();

   int schema_ok = 0;
   int have_pg_trgm = 0;
   assert(db2_health_probe(&schema_ok, &have_pg_trgm) == -1);

   assert(db2_init("postgres://db2.test/aimee") == 0);
   g_fail_exec = 1;
   assert(db2_health_probe(&schema_ok, &have_pg_trgm) == -1);

   reset_mocks();
   assert(db2_init("postgres://db2.test/aimee") == 0);
   g_fail_prepare = 1;
   assert(db2_health_probe(&schema_ok, &have_pg_trgm) == -1);
}

int main(void)
{
   test_init_shutdown_roundtrip();
   test_init_rejects_url_change_without_shutdown();
   test_init_rejects_empty_url();
   test_open_failure_leaves_db2_closed();
   test_schema_failure_closes_connection();
   test_health_probe_reports_schema_and_extension();
   test_init_fails_without_pg_trgm();
   test_health_probe_fails_without_init_or_query_failure();
   printf("db2: all tests passed\n");
   return 0;
}
