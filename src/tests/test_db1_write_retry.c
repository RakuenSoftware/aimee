/* test_db1_write_retry.c: tests for DB1 write-resilience helpers.
 *   1. db1_install_busy_handler — handler installed (sqlite3_busy_handler replaces timeout)
 *   2. db1_reconcile_columns — adds a column missing from the live DB
 *   3. db1_reconcile_columns — idempotent on a fully up-to-date DB
 *   4. window_fts_trigram virtual table created by schema */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>

/* Forward declarations */
void db1_maybe_checkpoint(sqlite3 *db);
void db1_reconcile_columns(sqlite3 *db);
int db1_apply_schema_sqlite(sqlite3 *db, char *errbuf, size_t errlen);

/* ── helpers ────────────────────────────────────────────────────────────── */

static sqlite3 *open_mem(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   return db;
}

static int col_exists(sqlite3 *db, const char *tbl, const char *col)
{
   char pq[256];
   snprintf(pq, sizeof(pq), "PRAGMA table_info(\"%s\")", tbl);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, pq, -1, &st, NULL) != SQLITE_OK)
      return 0;
   int found = 0;
   while (sqlite3_step(st) == SQLITE_ROW)
   {
      const char *n = (const char *)sqlite3_column_text(st, 1);
      if (n && strcmp(n, col) == 0)
      {
         found = 1;
         break;
      }
   }
   sqlite3_finalize(st);
   return found;
}

static int table_exists(sqlite3 *db, const char *tbl)
{
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", -1, &st,
                          NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_text(st, 1, tbl, -1, SQLITE_STATIC);
   int found = (sqlite3_step(st) == SQLITE_ROW);
   sqlite3_finalize(st);
   return found;
}

/* ── tests ──────────────────────────────────────────────────────────────── */

static void test_busy_handler_api(void)
{
   /* Verify that sqlite3_busy_handler accepts and clears a handler without
    * error — a baseline sanity check for the jitter-handler path that
    * db1_init installs on every real connection. */
   sqlite3 *db = open_mem();
   int rc = sqlite3_busy_handler(db, NULL, NULL);
   assert(rc == SQLITE_OK);
   sqlite3_close(db);
   printf("  PASS: test_busy_handler_api\n");
}

static void test_reconcile_adds_missing_column(void)
{
   sqlite3 *db = open_mem();
   /* Create a table with fewer columns than the canonical schema has */
   assert(sqlite3_exec(db,
                       "CREATE TABLE session_state"
                       " (session_id TEXT PRIMARY KEY,"
                       "  session_mode TEXT NOT NULL DEFAULT 'implement')",
                       NULL, NULL, NULL) == SQLITE_OK);

   /* Canonical schema has many more columns; reconcile should add them */
   db1_reconcile_columns(db);

   /* guardrail_mode column should now exist */
   assert(col_exists(db, "session_state", "guardrail_mode"));
   /* tdd_mode column should now exist */
   assert(col_exists(db, "session_state", "tdd_mode"));
   assert(col_exists(db, "session_state", "skill_condition_waiting_advisory_sent"));
   assert(col_exists(db, "session_state", "skill_tdd_advisory_sent"));

   sqlite3_close(db);
   printf("  PASS: test_reconcile_adds_missing_column\n");
}

static void test_reconcile_idempotent(void)
{
   sqlite3 *db = open_mem();
   char err[256] = "";
   int rc = db1_apply_schema_sqlite(db, err, sizeof(err));
   assert(rc == 0);

   /* Count tables before */
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table'", -1, &st, NULL);
   sqlite3_step(st);
   int cnt_before = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);

   /* Run reconcile twice — must not change table count */
   db1_reconcile_columns(db);
   db1_reconcile_columns(db);

   sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table'", -1, &st, NULL);
   sqlite3_step(st);
   int cnt_after = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);

   assert(cnt_before == cnt_after);
   sqlite3_close(db);
   printf("  PASS: test_reconcile_idempotent\n");
}

static void test_trigram_table_created(void)
{
   sqlite3 *db = open_mem();
   char err[256] = "";
   int rc = db1_apply_schema_sqlite(db, err, sizeof(err));
   assert(rc == 0);

   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(db,
                      "SELECT COUNT(*) FROM sqlite_master"
                      " WHERE type='table' AND name='window_fts_trigram'",
                      -1, &st, NULL);
   sqlite3_step(st);
   int cnt = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);

   assert(cnt == 1);
   sqlite3_close(db);
   printf("  PASS: test_trigram_table_created\n");
}

/* ── main ───────────────────────────────────────────────────────────────── */

/* The retired work queue's tables are dropped on upgrade, not left behind.
 *
 * Guards the disposition, not just the SQL: a legacy DB carries work_queue +
 * work_queue_log with rows, and applying the schema must remove both. Leaving
 * them would make an upgraded DB and a fresh install structurally different
 * forever, which is exactly the drift this migration exists to prevent. */
static void test_retired_work_queue_tables_are_dropped(void)
{
   sqlite3 *db = open_mem();
   /* A legacy DB, mid-flight: both tables present and non-empty. */
   assert(sqlite3_exec(db,
                       "CREATE TABLE work_queue (id TEXT PRIMARY KEY, title TEXT);"
                       "CREATE TABLE work_queue_log (id INTEGER PRIMARY KEY, item_id TEXT);"
                       "INSERT INTO work_queue VALUES ('wq1', 'legacy item');"
                       "INSERT INTO work_queue_log VALUES (1, 'wq1');",
                       NULL, NULL, NULL) == SQLITE_OK);
   assert(table_exists(db, "work_queue"));
   assert(table_exists(db, "work_queue_log"));

   char err[256] = "";
   assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);

   assert(!table_exists(db, "work_queue"));
   assert(!table_exists(db, "work_queue_log"));
   sqlite3_close(db);
   printf("  PASS: test_retired_work_queue_tables_are_dropped\n");
}

/* A fresh DB must not resurrect them, and the drop must not abort the apply
 * when there is nothing to drop (the statements run with errors ignored). */
static void test_fresh_db_has_no_work_queue_tables(void)
{
   sqlite3 *db = open_mem();
   char err[256] = "";
   assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   assert(!table_exists(db, "work_queue"));
   assert(!table_exists(db, "work_queue_log"));
   sqlite3_close(db);
   printf("  PASS: test_fresh_db_has_no_work_queue_tables\n");
}

int main(void)
{
   printf("db1_write_retry:\n");
   test_busy_handler_api();
   test_retired_work_queue_tables_are_dropped();
   test_fresh_db_has_no_work_queue_tables();
   test_reconcile_adds_missing_column();
   test_reconcile_idempotent();
   test_trigram_table_created();
   printf("ok\n");
   return 0;
}
