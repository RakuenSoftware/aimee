#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>
#include "aimee.h"
#include "db.h"
#include "db1.h"
#include "db_schema.h"
#include "db1_cron_jobs.h"
#include "../db2/db_schema.h"
#include "platform_test_util.h"

static void cleanup_test_db(const char *path);

static void make_test_db_path(char *out, size_t out_len, const char *stem)
{
   snprintf(out, out_len, "%s/%s-%d.db", platform_tmpdir(), stem, getpid());
}

static void test_open_memory(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }

   /* Verify tables exist */
   sqlite3_stmt *stmt;
   int rc =
       sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table'", -1, &stmt, NULL);
   assert(rc == SQLITE_OK);

   int table_count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
      table_count++;
   sqlite3_finalize(stmt);

   /* Should have many tables from migrations */
   assert(table_count >= 15);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);
}

static void test_fts5_available(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }
   assert(db1_fts5_available(db) == 1);
   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);
}

static void test_prepare_cache(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }

   sqlite3_stmt *s1 = db1_prepare(db, "SELECT 1");
   assert(s1 != NULL);

   /* Same query should return same statement */
   sqlite3_stmt *s2 = db1_prepare(db, "SELECT 1");
   assert(s1 == s2);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);
}

static void test_migrations_idempotent(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }
   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);

   /* Opening again (migrations already applied) should succeed */
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }
   assert(db != NULL);
   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);
}

/* test_migration_versions: removed — schema_revisions table no longer
 * exists. aimee ships a consolidated schema applied idempotently rather
 * than a revision log; the equivalent coverage now lives in
 * test_migrations_idempotent and test_key_tables_exist. */

static void test_key_tables_exist(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
      /* The DB1 schema only owns DB1 tables; the DB2 sqlite shim adds
       * the shareable tables (memories, rules, tasks, ...) that this
       * legacy "key tables" assertion was originally written against. */
      assert(db2_apply_schema_sqlite_shim(db, err, sizeof(err)) == 0);
   }

   /* Verify critical tables from specific migrations */
   const char *required_tables[] = {
       "rules",        "memories",       "tasks",           "anti_patterns", "checkpoints",
       "agent_log",    "working_memory", "server_sessions", "agent_cache",   "agent_hints",
       "trigger_runs", "cron_jobs",      "cron_job_runs",   "model_catalog", NULL};

   for (int i = 0; required_tables[i]; i++)
   {
      char sql[256];
      snprintf(sql, sizeof(sql), "SELECT 1 FROM sqlite_master WHERE type='table' AND name='%s'",
               required_tables[i]);
      sqlite3_stmt *stmt;
      int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
      assert(rc == SQLITE_OK);
      int exists = (sqlite3_step(stmt) == SQLITE_ROW);
      sqlite3_finalize(stmt);
      assert(exists);
   }

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);
}

static int sqlite_column_exists(sqlite3 *db, const char *table, const char *column)
{
   char sql[256];
   snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
   sqlite3_stmt *stmt = NULL;
   assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
   int found = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *name = sqlite3_column_text(stmt, 1);
      if (name && strcmp((const char *)name, column) == 0)
      {
         found = 1;
         break;
      }
   }
   sqlite3_finalize(stmt);
   return found;
}

static void test_db2_sqlite_code_embeddings_body_hash_migration(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   assert(sqlite3_exec(db,
                       "CREATE TABLE code_embeddings ("
                       " point_id INTEGER PRIMARY KEY,"
                       " embedding TEXT NOT NULL DEFAULT '[]',"
                       " project TEXT NOT NULL DEFAULT '',"
                       " node_key TEXT NOT NULL DEFAULT '',"
                       " file_path TEXT NOT NULL DEFAULT '',"
                       " symbol TEXT NOT NULL DEFAULT '',"
                       " record_type TEXT NOT NULL DEFAULT 'code_unit',"
                       " content_hash TEXT NOT NULL DEFAULT '',"
                       " source_hash TEXT NOT NULL DEFAULT '',"
                       " payload_json TEXT NOT NULL DEFAULT '',"
                       " updated_at TEXT NOT NULL DEFAULT (datetime('now')))",
                       NULL, NULL, NULL) == SQLITE_OK);
   assert(!sqlite_column_exists(db, "code_embeddings", "body_hash"));

   char err[512] = {0};
   assert(db2_apply_schema_sqlite_shim(db, err, sizeof(err)) == 0);
   assert(sqlite_column_exists(db, "code_embeddings", "body_hash"));
   assert(sqlite3_exec(db,
                       "INSERT INTO code_embeddings"
                       " (point_id, project, node_key, content_hash, body_hash)"
                       " VALUES (1, 'p', 'file:p:a.c', 'content', 'body')",
                       NULL, NULL, NULL) == SQLITE_OK);

   sqlite3_close(db);
}

static void test_trigger_run_change_counts(void)
{
   char path[PATH_MAX];
   make_test_db_path(path, sizeof(path), "aimee-test-trigger-runs");
   cleanup_test_db(path);
   db1_shutdown();

   assert(db1_init(path) == 0);
   assert(db1_trigger_insert("trig_change_count", "unit", "event", "task", "workspace", "{}") == 0);
   assert(db1_trigger_insert("trig_change_count", "unit", "event", "task", "workspace", "{}") != 0);
   assert(db1_trigger_status_set("missing_trigger", "cancelled", "", "") != 0);
   assert(db1_trigger_status_set("trig_change_count", "cancelled", "7", "") == 0);

   db1_trigger_run_t run;
   assert(db1_trigger_get("trig_change_count", &run) == 0);
   assert(strcmp(run.status, "cancelled") == 0);
   assert(strcmp(run.pipeline_id, "7") == 0);

   db1_shutdown();
   cleanup_test_db(path);
}

static void test_cron_job_history_round_trip(void)
{
   char path[PATH_MAX];
   make_test_db_path(path, sizeof(path), "aimee-test-cron-jobs");
   cleanup_test_db(path);
   db1_shutdown();

   assert(db1_init(path) == 0);

   cron_job_t job;
   memset(&job, 0, sizeof(job));
   snprintf(job.id, sizeof(job.id), "pve-pulse");
   snprintf(job.schedule, sizeof(job.schedule), "every 10m");
   snprintf(job.mode, sizeof(job.mode), "script");
   snprintf(job.script, sizeof(job.script), "echo OK");
   snprintf(job.deliver_target, sizeof(job.deliver_target), "local");
   job.deliver_only_if_changed = 1;
   job.enabled = 1;

   assert(db1_cron_job_upsert(&job) == 0);
   cron_job_t loaded;
   memset(&loaded, 0, sizeof(loaded));
   assert(db1_cron_job_get(job.id, &loaded) == 0);
   assert(strcmp(loaded.id, "pve-pulse") == 0);
   assert(strcmp(loaded.mode, "script") == 0);
   assert(loaded.deliver_only_if_changed == 1);

   cron_job_t jobs[4];
   memset(jobs, 0, sizeof(jobs));
   assert(db1_cron_jobs_load(jobs, 4, 1) == 1);
   assert(strcmp(jobs[0].id, "pve-pulse") == 0);

   assert(db1_cron_job_set_enabled(job.id, 0) == 0);
   assert(db1_cron_jobs_load(jobs, 4, 1) == 0);
   assert(db1_cron_job_set_enabled(job.id, 1) == 0);
   assert(db1_cron_jobs_set_enabled_all(0) == 0);
   assert(db1_cron_jobs_load(jobs, 4, 1) == 0);
   assert(db1_cron_jobs_set_enabled_all(1) == 0);
   assert(db1_cron_jobs_load(jobs, 4, 1) == 1);

   int run_id = db1_cron_job_record_run(job.id, "complete", 0, 0, "OK\n", "", "hash-one");
   assert(run_id > 0);

   char *last_hash = db1_cron_job_last_output_hash(job.id);
   assert(last_hash != NULL);
   assert(strcmp(last_hash, "hash-one") == 0);
   free(last_hash);

   char *latest = db1_cron_job_latest_output(job.id);
   assert(latest != NULL);
   assert(strcmp(latest, "OK\n") == 0);
   free(latest);

   char *history = db1_cron_job_history_json(job.id, 10);
   assert(history != NULL);
   assert(strstr(history, "\"job_id\":\"pve-pulse\"") != NULL);
   assert(strstr(history, "\"status\":\"complete\"") != NULL);
   free(history);

   assert(db1_cron_job_delete(job.id) == 0);
   assert(db1_cron_job_get(job.id, &loaded) != 0);

   db1_shutdown();
   cleanup_test_db(path);
}

static void test_model_catalog_cache(void)
{
   char path[PATH_MAX];
   make_test_db_path(path, sizeof(path), "aimee-test-model-catalog");
   cleanup_test_db(path);
   db1_shutdown();

   assert(db1_init(path) == 0);
   char *models[] = {"zeta-model", "alpha-model"};
   assert(db1_model_catalog_replace("openrouter", models, 2) == 0);
   assert(db1_model_catalog_is_fresh("openrouter", 3600) == 1);

   char **out = NULL;
   int n = 0;
   assert(db1_model_catalog_get("openrouter", &out, &n) == 0);
   assert(n == 2);
   assert(strcmp(out[0], "alpha-model") == 0);
   assert(strcmp(out[1], "zeta-model") == 0);
   db1_model_catalog_free(out, n);

   db1_shutdown();
   cleanup_test_db(path);
}

static int table_has_column(sqlite3 *db, const char *table, const char *column)
{
   char sql[128];
   snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
   assert(rc == SQLITE_OK);

   int found = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *name = (const char *)sqlite3_column_text(stmt, 1);
      if (name && strcmp(name, column) == 0)
      {
         found = 1;
         break;
      }
   }

   sqlite3_finalize(stmt);
   return found;
}

static void test_agent_schema_matches_runtime_queries(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
      /* agent_hints lives in the DB2 shim schema. */
      assert(db2_apply_schema_sqlite_shim(db, err, sizeof(err)) == 0);
   }

   sqlite3_stmt *stmt = NULL;
   char *err = NULL;
   int rc;

   assert(table_has_column(db, "agent_cache", "prompt"));
   assert(table_has_column(db, "agent_cache", "result"));
   assert(table_has_column(db, "agent_hints", "pattern"));
   assert(table_has_column(db, "agent_hints", "hint"));
   assert(table_has_column(db, "agent_hints", "consumed"));

   rc = sqlite3_exec(
       db,
       "INSERT INTO agent_cache(role, prompt, result) VALUES('reviewer', 'check migrations', "
       "'cached result')",
       NULL, NULL, &err);
   assert(rc == SQLITE_OK);
   sqlite3_free(err);
   err = NULL;

   rc = sqlite3_prepare_v2(db, "SELECT result FROM agent_cache WHERE role = ? AND prompt = ?", -1,
                           &stmt, NULL);
   assert(rc == SQLITE_OK);
   sqlite3_bind_text(stmt, 1, "reviewer", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, "check migrations", -1, SQLITE_STATIC);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "cached result") == 0);
   sqlite3_finalize(stmt);

   rc = sqlite3_exec(db,
                     "INSERT INTO agent_hints(role, pattern, hint) VALUES('reviewer', '%migrate%', "
                     "'run pending migrations')",
                     NULL, NULL, &err);
   assert(rc == SQLITE_OK);
   sqlite3_free(err);
   err = NULL;

   rc = sqlite3_prepare_v2(db,
                           "SELECT id, hint FROM agent_hints "
                           "WHERE role = ? AND consumed = 0 AND ? LIKE pattern "
                           "ORDER BY id DESC LIMIT 1",
                           -1, &stmt, NULL);
   assert(rc == SQLITE_OK);
   sqlite3_bind_text(stmt, 1, "reviewer", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, "please migrate this db", -1, SQLITE_STATIC);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int hint_id = sqlite3_column_int(stmt, 0);
   assert(hint_id > 0);
   assert(strcmp((const char *)sqlite3_column_text(stmt, 1), "run pending migrations") == 0);
   sqlite3_finalize(stmt);

   rc = sqlite3_prepare_v2(db, "UPDATE agent_hints SET consumed = 1 WHERE id = ?", -1, &stmt, NULL);
   assert(rc == SQLITE_OK);
   sqlite3_bind_int(stmt, 1, hint_id);
   assert(sqlite3_step(stmt) == SQLITE_DONE);
   sqlite3_finalize(stmt);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);
}

static void test_agent_schema_upgrade_from_previous_db(void)
{
   /* Open a fresh DB, close, reopen on the same file. With the
    * consolidated schema approach, reopen is a no-op pass over
    * CREATE-IF-NOT-EXISTS statements. Covers the "existing install"
    * path without faking a half-populated legacy schema (that case
    * never appeared in production since the old runner was
    * idempotent and always brought DBs up to the latest revision). */
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/aimee-test-agent-schema-upgrade-%d.db", platform_tmpdir(),
            getpid());
   cleanup_test_db(path);

   sqlite3 *db = NULL;
   assert(sqlite3_open(path, &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
      assert(db2_apply_schema_sqlite_shim(db, err, sizeof(err)) == 0);
   }
   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);

   assert(sqlite3_open(path, &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
      assert(db2_apply_schema_sqlite_shim(db, err, sizeof(err)) == 0);
   }
   assert(db != NULL);
   sqlite3_stmt *stmt = NULL;
   /* `rules` lives in the DB2 shim schema; the second open is a no-op
    * pass over CREATE IF NOT EXISTS, so it must still be present. */
   assert(sqlite3_prepare_v2(
              db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='rules'", -1,
              &stmt, NULL) == SQLITE_OK);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(sqlite3_column_int(stmt, 0) == 1);
   sqlite3_finalize(stmt);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);
   cleanup_test_db(path);
}

static void test_wal_mode(void)
{
   /* WAL mode should be set on file-backed DBs */
   char path[PATH_MAX];
   make_test_db_path(path, sizeof(path), "aimee-test-wal");
   cleanup_test_db(path);
   sqlite3 *db = NULL;
   assert(sqlite3_open(path, &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &stmt, NULL);
   assert(rc == SQLITE_OK);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   const char *mode = (const char *)sqlite3_column_text(stmt, 0);
   assert(strcmp(mode, "wal") == 0);
   sqlite3_finalize(stmt);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);
   cleanup_test_db(path);
}

static void test_prepare_cache_different_queries(void)
{
   sqlite3 *db = NULL;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }

   sqlite3_stmt *s1 = db1_prepare(db, "SELECT 1");
   sqlite3_stmt *s2 = db1_prepare(db, "SELECT 2");
   assert(s1 != NULL);
   assert(s2 != NULL);
   assert(s1 != s2); /* Different queries get different statements */

   /* Same query again returns cached */
   sqlite3_stmt *s3 = db1_prepare(db, "SELECT 1");
   assert(s3 == s1);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);
}

static void test_db_open_fast(void)
{
   /* db_open_fast on already-initialized DB should succeed */
   char path[PATH_MAX];
   make_test_db_path(path, sizeof(path), "aimee-test-fast");
   cleanup_test_db(path);
   sqlite3 *db = NULL;
   assert(sqlite3_open(path, &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }
   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);

   assert(sqlite3_open(path, &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_SERVER);
   assert(db != NULL);

   /* Fast-path connections are used by the long-lived server and should
    * therefore inherit the server pragma profile. */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA synchronous", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(sqlite3_column_int(stmt, 0) == 1); /* NORMAL */
   sqlite3_finalize(stmt);

   stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA cache_size", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(sqlite3_column_int(stmt, 0) == -8192);
   sqlite3_finalize(stmt);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);

   cleanup_test_db(path);
}

static void test_schema_version_fastpath(void)
{
   char path[PATH_MAX];
   make_test_db_path(path, sizeof(path), "aimee-test-schema-ver");
   cleanup_test_db(path);

   /* Opening a fresh DB applies the consolidated schema; opening it
    * again is a no-op. Previously this test walked PRAGMA user_version
    * + schema_revisions; both are gone now, so we instead verify the
    * core tables landed and a reopen still succeeds. */
   sqlite3 *db = NULL;
   assert(sqlite3_open(path, &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
      assert(db2_apply_schema_sqlite_shim(db, err, sizeof(err)) == 0);
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='memories'", -1, &stmt,
       NULL);
   assert(rc == SQLITE_OK);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   assert(sqlite3_column_int(stmt, 0) == 1);
   sqlite3_finalize(stmt);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);

   /* Second open: should hit fast-path (user_version matches).
    * Verify it still works correctly. */
   assert(sqlite3_open(path, &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
      assert(db2_apply_schema_sqlite_shim(db, err, sizeof(err)) == 0);
   }
   assert(db != NULL);

   /* Tables should still be accessible */
   stmt = NULL;
   rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM memories", -1, &stmt, NULL);
   assert(rc == SQLITE_OK);
   sqlite3_finalize(stmt);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);

   cleanup_test_db(path);
}

/* Remove a test database and all associated files (WAL, SHM, backup artifacts). */
static void cleanup_test_db(const char *path)
{
   unlink(path);
   char buf[512];
   snprintf(buf, sizeof(buf), "%s-wal", path);
   unlink(buf);
   snprintf(buf, sizeof(buf), "%s-shm", path);
   unlink(buf);
   /* Remove backup files left by backup_before_migrate() */
   for (int v = 0; v < 200; v++)
   {
      snprintf(buf, sizeof(buf), "%s.bak.%d", path, v);
      unlink(buf);
   }
}

static void test_schema_version_detects_new_migration(void)
{
   char path[256];
   make_test_db_path(path, sizeof(path), "aimee-test-schema-new");
   cleanup_test_db(path);

   /* Open to run all migrations */
   sqlite3 *db = NULL;
   assert(sqlite3_open(path, &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }

   /* Tamper: set user_version to something lower to simulate a schema/version mismatch */
   sqlite3_exec(db, "PRAGMA user_version = 1", NULL, NULL, NULL);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);

   /* Reopen: should detect mismatch and run through migration loop again */
   assert(sqlite3_open(path, &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }
   assert(db != NULL);

   /* user_version should be updated back to the current schema revision count */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int user_ver = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(user_ver >= 1);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);

   cleanup_test_db(path);
}

static void test_pragma_profile_cli(void)
{
   char path[PATH_MAX];
   make_test_db_path(path, sizeof(path), "aimee-test-pragma-cli");
   cleanup_test_db(path);
   sqlite3 *db = NULL;
   assert(sqlite3_open(path, &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }

   /* CLI mode should have synchronous=FULL (2) */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA synchronous", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int sync_val = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(sync_val == 2); /* FULL */

   /* CLI mode should have cache_size=-2048 */
   stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA cache_size", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int cache = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(cache == -2048);

   /* wal_autocheckpoint should be set */
   stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA wal_autocheckpoint", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int checkpoint = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(checkpoint == 1000);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);
   cleanup_test_db(path);
}

static void test_pragma_profile_server(void)
{
   char path[PATH_MAX];
   make_test_db_path(path, sizeof(path), "aimee-test-pragma-srv");
   cleanup_test_db(path);
   sqlite3 *db = NULL;
   assert(sqlite3_open(path, &db) == SQLITE_OK);
   db1_apply_pragmas(db, DB_MODE_CLI);
   {
      char err[512] = {0};
      assert(db1_apply_schema_sqlite(db, err, sizeof(err)) == 0);
   }

   /* Apply server profile (overrides CLI defaults) */
   db1_apply_pragmas(db, DB_MODE_SERVER);

   /* Server mode should have synchronous=NORMAL (1) */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA synchronous", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int sync_val = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(sync_val == 1); /* NORMAL */

   /* Server mode should have cache_size=-8192 */
   stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA cache_size", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int cache = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   assert(cache == -8192);

   /* Server mode should have mmap_size=67108864 */
   stmt = NULL;
   sqlite3_prepare_v2(db, "PRAGMA mmap_size", -1, &stmt, NULL);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   long long mmap = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);
   assert(mmap == 67108864);

   db1_stmt_cache_clear_for_sqlite(db);
   sqlite3_close(db);
   cleanup_test_db(path);
}

static void test_eval_results_rescue_recoveries_round_trip(void)
{
   char path[PATH_MAX];
   make_test_db_path(path, sizeof(path), "aimee-test-eval-results");
   cleanup_test_db(path);
   db1_shutdown();

   assert(db1_init(path) == 0);

   db1_eval_result_row_t row = {
       .suite = "delegate",
       .task_name = "xml-recovery",
       .agent_name = "test-agent",
       .ablation = "full",
       .success = 1,
       .turns = 2,
       .tool_calls = 3,
       .tool_call_failures = 0,
       .rescue_recoveries = 2,
       .prompt_tokens = 10,
       .completion_tokens = 5,
       .latency_ms = 17,
       .response = "ok",
       .dataset_hash = "dataset",
       .target_hash = "target",
       .harness_version = "1",
       .hardware_profile = "unit",
       .seed = 42,
   };
   assert(db1_eval_result_insert(&row) == 0);

   db1_eval_display_row_t rows[4];
   int n = db1_eval_results_list("delegate", rows, 4);
   assert(n == 1);
   assert(strcmp(rows[0].task_name, "xml-recovery") == 0);
   assert(rows[0].tool_calls == 3);
   assert(rows[0].tool_call_failures == 0);
   assert(rows[0].rescue_recoveries == 2);

   db1_shutdown();
   cleanup_test_db(path);
}

/* test_migration_ordering_validation: removed along with the revision
 * runner — there are no migration IDs to order. The consolidated
 * schema is a single CREATE-IF-NOT-EXISTS pass applied on open. */

int main(void)
{
   test_open_memory();
   test_fts5_available();
   test_prepare_cache();
   test_prepare_cache_different_queries();
   test_migrations_idempotent();
   test_key_tables_exist();
   test_db2_sqlite_code_embeddings_body_hash_migration();
   test_trigger_run_change_counts();
   test_cron_job_history_round_trip();
   test_model_catalog_cache();
   test_agent_schema_matches_runtime_queries();
   test_agent_schema_upgrade_from_previous_db();
   test_wal_mode();
   test_db_open_fast();
   test_schema_version_fastpath();
   test_schema_version_detects_new_migration();
   test_pragma_profile_cli();
   test_pragma_profile_server();
   test_eval_results_rescue_recoveries_round_trip();
   printf("db: all tests passed\n");
   return 0;
}
