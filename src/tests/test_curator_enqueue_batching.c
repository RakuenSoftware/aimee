/* test_curator_enqueue_batching.c: the code-unit enqueue must issue ONE
 * statement, whatever the symbol count.
 *
 * kb_curator_queue_code_units_for_project used to SELECT the rows to enqueue and
 * then feed them back one INSERT at a time. Each of those inserts is its own
 * autocommit transaction, so the cost was one WAL fsync per symbol. On a
 * 4,018-file repository that is ~183,000 fsyncs: postgres sat in
 * LWLock/WALWrite, and the /v1/code/scan request that triggered it never
 * answered inside the client's 5-minute deadline — `aimee index scan` on a
 * mid-sized repo could not complete at all.
 *
 * A timing test would be a bad guard (it needs a real postgres, and it would be
 * flaky). The defect is structural and countable: N statements instead of 1.
 * So this stubs the pg layer, counts prepares, and asserts the shape of the SQL
 * that goes to the database — which is exactly what regressed.
 *
 * DISTINCT ON is asserted too, because it is load-bearing rather than
 * decorative: postgres refuses an ON CONFLICT DO UPDATE that touches the same
 * row twice in one statement, and `terms` legitimately holds the same symbol
 * name twice for one file. Dropping it turns the fast path into a hard error on
 * any corpus with a duplicate (observed: 328 duplicate pairs in a 45k-term
 * project). */
#include "db_postgres.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int kb_curator_queue_code_units_for_project(const char *project, const char *root_path);

/* ---- recorded pg interactions ---- */
#define MAX_STMTS 64
/* The stub must model a SELECT that actually RETURNS ROWS, or the old
 * row-per-symbol code never performs its N inserts and this test would pass
 * against the very thing it exists to catch. */
#define SYMBOLS_IN_PROJECT 5
static char g_sql[MAX_STMTS][4096];
static int g_prepares;
static int g_changes_to_report;
static int g_is_select[MAX_STMTS];
static int g_rows_left;

static void reset(void)
{
   g_prepares = 0;
   g_changes_to_report = 0;
   g_rows_left = 0;
   memset(g_sql, 0, sizeof(g_sql));
   memset(g_is_select, 0, sizeof(g_is_select));
}

/* ---- pg layer stubs ---- */
struct aimee_pg_stmt
{
   int idx;
};
static struct aimee_pg_stmt g_stmt;

void *db2_conn(void)
{
   static int conn = 1;
   return &conn; /* non-NULL: the function proceeds to prepare */
}

/* A SELECT yields SYMBOLS_IN_PROJECT rows; an INSERT completes. */
aimee_pg_stmt_t *aimee_pg_prepare(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   (void)pg_conn;
   (void)errbuf;
   (void)errlen;
   int select_only = sql && strncmp(sql, "SELECT", 6) == 0;
   if (g_prepares < MAX_STMTS)
   {
      snprintf(g_sql[g_prepares], sizeof(g_sql[0]), "%s", sql ? sql : "");
      g_is_select[g_prepares] = select_only;
   }
   if (select_only)
      g_rows_left = SYMBOLS_IN_PROJECT;
   g_prepares++;
   g_stmt.idx = g_prepares - 1;
   return (aimee_pg_stmt_t *)&g_stmt;
}

aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *stmt, char *errbuf, size_t errlen)
{
   (void)errbuf;
   (void)errlen;
   struct aimee_pg_stmt *s = (struct aimee_pg_stmt *)stmt;
   if (s && s->idx < MAX_STMTS && g_is_select[s->idx] && g_rows_left > 0)
   {
      g_rows_left--;
      return AIMEE_PG_ROW;
   }
   return AIMEE_PG_DONE;
}

int aimee_pg_stmt_changes(aimee_pg_stmt_t *stmt)
{
   (void)stmt;
   return g_changes_to_report;
}

void aimee_pg_finalize(aimee_pg_stmt_t *stmt)
{
   (void)stmt;
}
int aimee_pg_bind_text(aimee_pg_stmt_t *stmt, const char *name, const char *value)
{
   (void)stmt;
   (void)name;
   (void)value;
   return 0;
}
int aimee_pg_bind_int(aimee_pg_stmt_t *stmt, const char *name, int value)
{
   (void)stmt;
   (void)name;
   (void)value;
   return 0;
}
const char *aimee_pg_column_text(aimee_pg_stmt_t *stmt, int col)
{
   (void)stmt;
   (void)col;
   return "";
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
int db2_kb_async_enqueue(const char *kind, int64_t id, const char *project)
{
   (void)kind;
   (void)id;
   (void)project;
   return 0;
}
/* Only reached by the docs path, which these tests do not exercise. */
int index_list_projects(void *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}
void aimee_log(int level, const char *component, const char *fmt, ...)
{
   (void)level;
   (void)component;
   (void)fmt;
}

/* The gate: on for the batching tests, off for the gate test. */
static int g_gate = 1;
int config_kb_curator_extract_code_enabled(void)
{
   return g_gate;
}
int config_kb_curator_extract_docs_enabled(void)
{
   return 0;
}

/* ---- tests ---- */

/* The regression itself: one statement reaches the database, not one per row. */
static void test_enqueue_issues_a_single_statement(void)
{
   reset();
   g_gate = 1;
   g_changes_to_report = 44668;

   int rc = kb_curator_queue_code_units_for_project("proj", "/repo");

   /* Pre-fix this was 1 SELECT + one INSERT per row = 1 + SYMBOLS_IN_PROJECT. */
   assert(g_prepares == 1);
   assert(rc == 44668);     /* rowcount comes from the statement, not a counter */
   printf("  test_enqueue_issues_a_single_statement: ok\n");
}

/* That single statement must be the INSERT, not a SELECT the caller then
 * loops over — otherwise the count above could pass while the N+1 returned. */
static void test_the_statement_is_the_insert(void)
{
   reset();
   g_gate = 1;
   kb_curator_queue_code_units_for_project("proj", "/repo");

   assert(g_prepares == 1);
   const char *sql = g_sql[0];
   assert(strstr(sql, "INSERT INTO kb_code_unit_jobs") != NULL);
   assert(strstr(sql, "SELECT") != NULL);      /* insert-select, one round trip */
   assert(strstr(sql, "NOT EXISTS") != NULL);  /* anti-join preserved */
   assert(strstr(sql, "ON CONFLICT") != NULL); /* idempotent re-run preserved */
   printf("  test_the_statement_is_the_insert: ok\n");
}

/* DISTINCT ON is required for correctness once the loop is folded in: without
 * it postgres rejects the whole statement on any duplicate (file_path, symbol).
 * ORDER BY must accompany it so the surviving row is deterministic. */
static void test_distinct_on_guards_duplicate_symbols(void)
{
   reset();
   g_gate = 1;
   kb_curator_queue_code_units_for_project("proj", "/repo");

   const char *sql = g_sql[0];
   assert(strstr(sql, "DISTINCT ON") != NULL);
   assert(strstr(sql, "ORDER BY") != NULL);
   printf("  test_distinct_on_guards_duplicate_symbols: ok\n");
}

/* Gate off must still short-circuit before touching the database. */
static void test_gate_off_touches_no_database(void)
{
   reset();
   g_gate = 0;

   int rc = kb_curator_queue_code_units_for_project("proj", "/repo");

   assert(rc == 0);
   assert(g_prepares == 0);
   printf("  test_gate_off_touches_no_database: ok\n");
}

int main(void)
{
   printf("test_curator_enqueue_batching:\n");
   test_enqueue_issues_a_single_statement();
   test_the_statement_is_the_insert();
   test_distinct_on_guards_duplicate_symbols();
   test_gate_off_touches_no_database();
   printf("ALL PASS\n");
   return 0;
}
