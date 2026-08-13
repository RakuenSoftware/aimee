/* test_kb_maintenance.c: DB-backed tests for KB temporal confidence decay. */

#include "config.h"
#include "db2_test_shim.h"
#include "artifacts.h"
#include "kb_maintenance.h"
#include "db_postgres.h"
#include "db2_internal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

#define TOL 0.0001

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

static void exec_sql(const char *sql)
{
   char err[512] = "";
   assert(aimee_pg_exec(db2_conn(), sql, err, sizeof(err)) == 0);
}

static double query_double(const char *sql)
{
   char err[512] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st != NULL);
   double out = 0.0;
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   out = aimee_pg_column_double(st, 0);
   aimee_pg_finalize(st);
   return out;
}

static int query_int(const char *sql)
{
   char err[512] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st != NULL);
   int out = 0;
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   out = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return out;
}

static void query_text(const char *sql, char *out, size_t out_len)
{
   char err[512] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st != NULL);
   out[0] = '\0';
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   const char *v = aimee_pg_column_text(st, 0);
   snprintf(out, out_len, "%s", v ? v : "");
   aimee_pg_finalize(st);
}

static void seed_artifact(const char *id, double confidence, int age_days, int cited)
{
   assert(db2_artifact_write(id, "claim", "committed", "global", "", "", confidence, "{}") == 0);

   char sql[512];
   snprintf(sql, sizeof(sql),
            "UPDATE artifacts"
            "   SET created_at = datetime('now', '-%d days'),"
            "       committed_at = datetime('now', '-%d days'),"
            "       last_accessed_at = datetime('now', '-%d days'),"
            "       last_decay_at = NULL"
            " WHERE id = '%s'",
            age_days, age_days, age_days, id);
   exec_sql(sql);

   if (cited)
      assert(db2_artifact_cite(id, "test", "source") == 0);
}

static kb_maintenance_config_t test_cfg(void)
{
   kb_maintenance_config_t cfg;
   kb_maintenance_config_defaults(&cfg);
   cfg.lambda = 0.005;
   cfg.confidence_floor = 0.10;
   cfg.min_age_days = 7;
   cfg.orphan_prune_days = 90;
   cfg.dry_run = 0;
   return cfg;
}

static void test_config_defaults_are_disabled_but_complete(void)
{
   char tmpdir[256];
   snprintf(tmpdir, sizeof tmpdir, "%s/aimee-kbm-home-XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmpdir) != NULL);

   const char *old_home = getenv("HOME");
   char old_home_buf[512] = "";
   if (old_home)
      snprintf(old_home_buf, sizeof(old_home_buf), "%s", old_home);

   assert(setenv("HOME", tmpdir, 1) == 0);
   assert(setenv("AIMEE_NO_CACHE", "1", 1) == 0);

   config_t cfg;
   assert(config_load(&cfg) == 0);
   assert(cfg.kb_maintenance_enabled == 0);
   assert(cfg.kb_maintenance_interval_hours == 24);
   assert(fabs(cfg.kb_maintenance_lambda - 0.005) < TOL);
   assert(fabs(cfg.kb_maintenance_floor - 0.10) < TOL);
   assert(cfg.kb_maintenance_min_age_days == 7);
   assert(cfg.kb_maintenance_orphan_days == 90);

   if (old_home_buf[0])
      assert(setenv("HOME", old_home_buf, 1) == 0);
   else
      assert(unsetenv("HOME") == 0);
   assert(unsetenv("AIMEE_NO_CACHE") == 0);

   printf("  config_defaults_are_disabled_but_complete: ok\n");
}

static void test_decay_is_idempotent_after_first_run(void)
{
   open_db();
   seed_artifact("artifact-idempotent", 1.0, 100, 1);

   kb_maintenance_config_t cfg = test_cfg();
   kb_maintenance_result_t first;
   assert(kb_maintenance_run(&cfg, &first) == 0);
   assert(first.rows_decayed == 1);
   double after_first =
       query_double("SELECT confidence FROM artifacts WHERE id = 'artifact-idempotent'");
   assert(after_first < 1.0);
   assert(after_first > cfg.confidence_floor);

   kb_maintenance_result_t second;
   assert(kb_maintenance_run(&cfg, &second) == 0);
   double after_second =
       query_double("SELECT confidence FROM artifacts WHERE id = 'artifact-idempotent'");
   assert(second.rows_decayed == 0);
   assert(fabs(after_first - after_second) < TOL);
   assert(query_int("SELECT COUNT(*) FROM kb_maintenance_runs") == 2);

   close_db();
   printf("  decay_is_idempotent_after_first_run: ok\n");
}

static void test_floor_clamping(void)
{
   open_db();
   seed_artifact("artifact-floor", 0.20, 1000, 1);

   kb_maintenance_config_t cfg = test_cfg();
   kb_maintenance_result_t result;
   assert(kb_maintenance_run(&cfg, &result) == 0);
   assert(result.rows_decayed == 1);

   double conf = query_double("SELECT confidence FROM artifacts WHERE id = 'artifact-floor'");
   assert(fabs(conf - cfg.confidence_floor) < TOL);

   close_db();
   printf("  floor_clamping: ok\n");
}

static void test_dry_run_rolls_back_artifact_and_run_rows(void)
{
   open_db();
   seed_artifact("artifact-dry-run", 1.0, 100, 1);

   kb_maintenance_config_t cfg = test_cfg();
   cfg.dry_run = 1;
   kb_maintenance_result_t result;
   assert(kb_maintenance_run(&cfg, &result) == 0);
   assert(result.rows_decayed == 1);

   double conf = query_double("SELECT confidence FROM artifacts WHERE id = 'artifact-dry-run'");
   assert(fabs(conf - 1.0) < TOL);
   assert(query_int("SELECT COUNT(*) FROM kb_maintenance_runs") == 0);

   close_db();
   printf("  dry_run_rolls_back_artifact_and_run_rows: ok\n");
}

static void test_orphan_pruning_respects_links_and_age(void)
{
   open_db();
   seed_artifact("artifact-orphan", 0.10, 120, 0);
   seed_artifact("artifact-linked-a", 0.10, 120, 0);
   seed_artifact("artifact-linked-b", 0.10, 120, 0);
   assert(db2_artifact_link("artifact-linked-b", "artifact-linked-a", "supports") == 0);

   kb_maintenance_config_t cfg = test_cfg();
   kb_maintenance_result_t result;
   assert(kb_maintenance_run(&cfg, &result) == 0);
   assert(result.orphans_pruned == 1);

   char state[32];
   query_text("SELECT state FROM artifacts WHERE id = 'artifact-orphan'", state, sizeof(state));
   assert(strcmp(state, "retired") == 0);
   query_text("SELECT state FROM artifacts WHERE id = 'artifact-linked-a'", state, sizeof(state));
   assert(strcmp(state, "committed") == 0);

   close_db();
   printf("  orphan_pruning_respects_links_and_age: ok\n");
}

static void test_maintenance_run_metrics_are_recorded(void)
{
   open_db();
   seed_artifact("artifact-metrics", 1.0, 100, 1);

   kb_maintenance_config_t cfg = test_cfg();
   kb_maintenance_result_t result;
   assert(kb_maintenance_run(&cfg, &result) == 0);
   assert(result.rows_decayed == 1);
   assert(result.orphans_pruned == 0);
   assert(result.run_id[0] != '\0');

   assert(query_int("SELECT COUNT(*) FROM kb_maintenance_runs") == 1);
   assert(query_int("SELECT rows_decayed FROM kb_maintenance_runs") == 1);
   assert(query_int("SELECT orphans_pruned FROM kb_maintenance_runs") == 0);
   assert(query_int("SELECT dry_run FROM kb_maintenance_runs") == 0);
   double floor = query_double("SELECT confidence_floor FROM kb_maintenance_runs");
   assert(fabs(floor - cfg.confidence_floor) < TOL);

   close_db();
   printf("  maintenance_run_metrics_are_recorded: ok\n");
}

int main(void)
{
   printf("test_kb_maintenance\n");
   test_config_defaults_are_disabled_but_complete();
   test_decay_is_idempotent_after_first_run();
   test_floor_clamping();
   test_dry_run_rolls_back_artifact_and_run_rows();
   test_orphan_pruning_respects_links_and_age();
   test_maintenance_run_metrics_are_recorded();
   printf("All tests passed.\n");
   return 0;
}
