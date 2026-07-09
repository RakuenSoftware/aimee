/* test_calibration.c — unit tests for the calibration_profile DB2 module.
 *
 * Tests:
 *   1. db2_calibration_profile_write: writes a calibration_profile artifact.
 *   2. db2_calibration_profile_read: reads back the profile payload.
 *   3. narrowest-scope fallback: exact > scope-kind > global.
 *   4. db2_calibration_audit_stats: returns 0 buckets when table is empty.
 *   5. db2_calibration_surfaces_with_data: returns 0 when audit_events is empty.
 *   6. config: calibration defaults are sane.
 *   7. kb_calibrate_run: high-confidence-only audit rows still fit a profile.
 *   8. calibration sidecar: fixture request returns conformal profile payload.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "calibration.h"
#include "artifacts.h"
#include "db2_test_shim.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "config.h"
#include "config_learning.h"
#include "kb_calibrate.h"
#include "platform_process.h"
#include <cJSON.h>

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

static void set_audit_verdict(const char *audit_id, const char *verdict)
{
   void *conn = db2_conn();
   assert(conn != NULL);
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE audit_events SET verdict = ?1 WHERE id = ?2", err, sizeof(err));
   assert(st != NULL);
   aimee_pg_bind_text(st, "?1", verdict);
   aimee_pg_bind_text(st, "?2", audit_id);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(st);
}

static void write_audit_fixture(const char *artifact_id, const char *audit_id, const char *kind,
                                const char *surface, const char *scope_kind, const char *scope_id,
                                double confidence, const char *verdict)
{
   assert(db2_artifact_write(artifact_id, kind, "committed", scope_kind, scope_id, "", confidence,
                             "{}") == 0);
   assert(db2_audit_event_write(audit_id, artifact_id, surface, "target", "", scope_kind, scope_id,
                                confidence, 0, "{}", "{}") == 0);
   set_audit_verdict(audit_id, verdict);
}

/* ---- 1. calibration_profile_write ---- */
static void test_profile_write(void)
{
   open_db();

   char id_out[64];
   int rc = db2_calibration_profile_write("memory", "preference", "user", "jbailes", "v1",
                                          "{\"buckets\":[]}", id_out, sizeof(id_out));
   assert(rc == 0);
   assert(strlen(id_out) == 36);

   close_db();
   printf("  calibration_profile_write: ok\n");
}

/* ---- 2. calibration_profile_read ---- */
static void test_profile_read(void)
{
   open_db();

   const char *payload = "{\"target_surface\":\"memory\",\"kind\":\"preference\"}";
   char id_out[64];
   assert(db2_calibration_profile_write("memory", "preference", "user", "jbailes", "v1", payload,
                                        id_out, sizeof(id_out)) == 0);

   char buf[1024];
   int rc =
       db2_calibration_profile_read("memory", "preference", "user", "jbailes", buf, sizeof(buf));
   assert(rc == 0);
   assert(strstr(buf, "memory") != NULL);

   close_db();
   printf("  calibration_profile_read: ok\n");
}

/* ---- 3. narrowest-scope fallback ---- */
static void test_scope_fallback(void)
{
   open_db();

   /* Write a global profile */
   assert(db2_calibration_profile_write("memory", "fact", "global", "", "v1",
                                        "{\"scope\":\"global\"}", NULL, 0) == 0);

   /* Read with an exact scope that doesn't exist — should fall back to global */
   char buf[256];
   int rc =
       db2_calibration_profile_read("memory", "fact", "user", "someone-else", buf, sizeof(buf));
   assert(rc == 0);
   assert(strstr(buf, "global") != NULL);

   /* Write a user-scoped profile — exact match should win */
   assert(db2_calibration_profile_write("memory", "fact", "user", "someone-else", "v1",
                                        "{\"scope\":\"exact\"}", NULL, 0) == 0);
   rc = db2_calibration_profile_read("memory", "fact", "user", "someone-else", buf, sizeof(buf));
   assert(rc == 0);
   assert(strstr(buf, "exact") != NULL);

   /* Unknown scope — global fallback */
   rc = db2_calibration_profile_read("memory", "fact", "project", "other", buf, sizeof(buf));
   assert(rc == 0);
   assert(strstr(buf, "global") != NULL);

   close_db();
   printf("  scope_fallback: ok\n");
}

/* ---- 4. audit_stats on empty table ---- */
static void test_audit_stats_empty(void)
{
   open_db();

   db2_calibration_bucket_t buckets[DB2_CALIBRATION_BUCKETS];
   int n = db2_calibration_audit_stats("memory", "preference", NULL, NULL, 500, buckets,
                                       DB2_CALIBRATION_BUCKETS);
   /* Empty table → 0 buckets filled */
   assert(n == 0);

   close_db();
   printf("  audit_stats_empty: ok\n");
}

static void test_audit_stats_bucket_and_scope(void)
{
   open_db();

   write_audit_fixture("00000000-0000-0000-0000-000000000101",
                       "00000000-0000-0000-0000-000000000201", "preference", "memory", "user",
                       "alice", 0.91, "accepted");
   write_audit_fixture("00000000-0000-0000-0000-000000000102",
                       "00000000-0000-0000-0000-000000000202", "preference", "memory", "user",
                       "alice", 1.0, "rejected");
   write_audit_fixture("00000000-0000-0000-0000-000000000103",
                       "00000000-0000-0000-0000-000000000203", "preference", "memory", "user",
                       "bob", 0.91, "rejected");

   db2_calibration_bucket_t buckets[DB2_CALIBRATION_BUCKETS];
   int n = db2_calibration_audit_stats("memory", "preference", "user", "alice", 500, buckets,
                                       DB2_CALIBRATION_BUCKETS);
   assert(n == 1);
   assert(buckets[9].sample_n == 2);
   assert(fabs(buckets[9].alpha - 1.0) < 1e-9);
   assert(fabs(buckets[9].beta - 1.0) < 1e-9);

   n = db2_calibration_audit_stats("memory", "preference", "user", "bob", 500, buckets,
                                   DB2_CALIBRATION_BUCKETS);
   assert(n == 1);
   assert(buckets[9].sample_n == 1);
   assert(fabs(buckets[9].alpha - 0.0) < 1e-9);
   assert(fabs(buckets[9].beta - 1.0) < 1e-9);

   close_db();
   printf("  audit_stats_bucket_and_scope: ok\n");
}

static void test_conformal_window(void)
{
   open_db();

   write_audit_fixture("00000000-0000-0000-0000-000000000301",
                       "00000000-0000-0000-0000-000000000401", "fact", "memory", "project", "aimee",
                       0.42, "rejected");
   write_audit_fixture("00000000-0000-0000-0000-000000000302",
                       "00000000-0000-0000-0000-000000000402", "fact", "memory", "project", "aimee",
                       0.82, "accepted");

   db2_calibration_conformal_row_t rows[4];
   int n = db2_calibration_conformal_window("memory", "fact", "project", "aimee", 500, rows, 4);
   assert(n == 2);
   assert((strcmp(rows[0].verdict, "accepted") == 0 || strcmp(rows[0].verdict, "rejected") == 0));
   assert(rows[0].applied_confidence >= 0.0);
   assert(rows[1].applied_confidence >= 0.0);

   close_db();
   printf("  conformal_window: ok\n");
}

/* ---- 5. surfaces_with_data on empty table ---- */
static void test_surfaces_empty(void)
{
   open_db();

   int n = db2_calibration_surfaces_with_data(1);
   assert(n == 0);

   close_db();
   printf("  surfaces_with_data_empty: ok\n");
}

static void test_surface_list_discovers_audit_tuples(void)
{
   open_db();

   for (int i = 0; i < 3; i++)
   {
      char artifact_id[64];
      char audit_id[64];
      snprintf(artifact_id, sizeof(artifact_id), "00000000-0000-0000-0000-00000003%04d", i);
      snprintf(audit_id, sizeof(audit_id), "00000000-0000-0000-0000-00000004%04d", i);
      write_audit_fixture(artifact_id, audit_id, "novel_kind", "novel_surface", "project", "aimee",
                          0.91, "accepted");
   }

   db2_calibration_surface_t rows[4];
   int n = db2_calibration_surface_list(3, rows, 4);
   assert(n == 1);
   assert(strcmp(rows[0].target_surface, "novel_surface") == 0);
   assert(strcmp(rows[0].kind, "novel_kind") == 0);
   assert(strcmp(rows[0].scope_kind, "project") == 0);
   assert(strcmp(rows[0].scope_id, "aimee") == 0);

   close_db();
   printf("  surface_list_discovers_audit_tuples: ok\n");
}

/* ---- 6. calibration config defaults ---- */
static void test_config_defaults(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_apply_calibration_settings(&cfg, NULL);

   assert(cfg.calibration_enabled == 1); /* default: shadow (observe-only) */
   assert(cfg.calibration_buckets == 10);
   assert(fabs(cfg.calibration_prior_alpha0 - 2.0) < 1e-9);
   assert(fabs(cfg.calibration_prior_beta0 - 1.0) < 1e-9);
   assert(fabs(cfg.calibration_credible_delta - 0.10) < 1e-9);
   assert(cfg.calibration_conformal_window == 500);
   assert(fabs(cfg.calibration_conformal_epsilon - 0.05) < 1e-9);
   assert(fabs(cfg.calibration_tau_memory_auto - 0.70) < 1e-9);
   assert(fabs(cfg.calibration_tau_memory_flag - 0.55) < 1e-9);
   assert(fabs(cfg.calibration_tau_working_profile_auto - 0.80) < 1e-9);
   assert(fabs(cfg.calibration_tau_working_profile_flag - 0.65) < 1e-9);
   assert(strcmp(cfg.calibration_prompt_version, "v1") == 0);
   assert(strcmp(cfg.calibration_model_version, "beta-binomial-v1") == 0);
   assert(cfg.calibration_command[0] == '\0');

   printf("  config_defaults: ok\n");
}

static void test_config_tau_and_versions(void)
{
   const char *json = "{\"intelligence\":{\"calibrate\":{"
                      "\"prompt_version\":\"prompt-v2\","
                      "\"model_version\":\"model-v3\","
                      "\"tau\":{"
                      "\"memory\":{\"auto\":0.72,\"flag\":0.51},"
                      "\"working_profile.auto\":0.83,"
                      "\"working_profile.flag\":0.61,"
                      "\"unknown.auto\":0.99"
                      "}}}}";
   cJSON *root = cJSON_Parse(json);
   assert(root != NULL);

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_apply_calibration_settings(&cfg, root);
   cJSON_Delete(root);

   assert(fabs(cfg.calibration_tau_memory_auto - 0.72) < 1e-9);
   assert(fabs(cfg.calibration_tau_memory_flag - 0.51) < 1e-9);
   assert(fabs(cfg.calibration_tau_working_profile_auto - 0.83) < 1e-9);
   assert(fabs(cfg.calibration_tau_working_profile_flag - 0.61) < 1e-9);
   assert(strcmp(cfg.calibration_prompt_version, "prompt-v2") == 0);
   assert(strcmp(cfg.calibration_model_version, "model-v3") == 0);

   printf("  config_tau_and_versions: ok\n");
}

static void test_threshold_from_profile_json(void)
{
   const char *profile = "{\"buckets\":["
                         "{\"range\":[0.0,0.6],\"lower_credible_bound\":0.70},"
                         "{\"range\":[0.6,0.8],\"lower_credible_bound\":0.85}],"
                         "\"conformal\":{\"reject_below\":0.65}}";
   double threshold = 0.0;
   assert(db2_calibration_threshold_from_profile_json(profile, 0.80, &threshold) == 0);
   assert(fabs(threshold - 0.65) < 1e-9);

   printf("  threshold_from_profile_json: ok\n");
}

static void test_kb_calibrate_run_high_bucket(void)
{
   open_db();

   for (int i = 0; i < 20; i++)
   {
      char artifact_id[64];
      char audit_id[64];
      snprintf(artifact_id, sizeof(artifact_id), "00000000-0000-0000-0000-00000001%04d", i);
      snprintf(audit_id, sizeof(audit_id), "00000000-0000-0000-0000-00000002%04d", i);
      write_audit_fixture(artifact_id, audit_id, "preference", "memory", "global", "", 0.95,
                          (i % 4 == 0) ? "rejected" : "accepted");
   }

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_apply_calibration_settings(&cfg, NULL);
   cfg.calibration_enabled = 1;
   cfg.calibration_buckets = 10;
   cfg.calibration_conformal_window = 500;

   int written = kb_calibrate_run(&cfg);
   assert(written == 1);

   char buf[256];
   assert(db2_calibration_profile_read("memory", "preference", "global", "", buf, sizeof(buf)) ==
          0);
   assert(strcmp(buf, "{}") == 0);

   close_db();
   printf("  kb_calibrate_run_high_bucket: ok\n");
}

static void test_kb_calibrate_run_discovers_dynamic_surface(void)
{
   open_db();

   for (int i = 0; i < 20; i++)
   {
      char artifact_id[64];
      char audit_id[64];
      snprintf(artifact_id, sizeof(artifact_id), "00000000-0000-0000-0000-00000005%04d", i);
      snprintf(audit_id, sizeof(audit_id), "00000000-0000-0000-0000-00000006%04d", i);
      write_audit_fixture(artifact_id, audit_id, "novel_kind", "novel_surface", "global", "", 0.88,
                          "accepted");
   }

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_apply_calibration_settings(&cfg, NULL);
   cfg.calibration_enabled = 1;
   cfg.calibration_buckets = 10;
   cfg.calibration_conformal_window = 500;

   int written = kb_calibrate_run(&cfg);
   assert(written == 1);

   char buf[256];
   assert(db2_calibration_profile_read("novel_surface", "novel_kind", "global", "", buf,
                                       sizeof(buf)) == 0);

   close_db();
   printf("  kb_calibrate_run_discovers_dynamic_surface: ok\n");
}

static void test_calibration_sidecar_fixture(void)
{
   const char *req = "{"
                     "\"version\":1,"
                     "\"role\":\"calibrate\","
                     "\"model_version\":\"beta-binomial-v1\","
                     "\"prompt_version\":\"fixture-v1\","
                     "\"scope\":{\"kind\":\"global\",\"id\":\"\"},"
                     "\"inputs\":{"
                     "\"target_surface\":\"memory\","
                     "\"kind\":\"preference\","
                     "\"buckets\":["
                     "{\"range\":[0.8,0.9],\"n_accepted\":8,\"n_rejected\":2},"
                     "{\"range\":[0.9,1.0],\"n_accepted\":18,\"n_rejected\":1}"
                     "],"
                     "\"conformal_window\":["
                     "{\"applied_confidence\":0.40,\"verdict\":\"rejected\"},"
                     "{\"applied_confidence\":0.90,\"verdict\":\"rejected\"},"
                     "{\"applied_confidence\":0.95,\"verdict\":\"accepted\"}"
                     "],"
                     "\"config\":{"
                     "\"prior_alpha0\":2.0,"
                     "\"prior_beta0\":1.0,"
                     "\"buckets\":10,"
                     "\"credible_delta\":0.10,"
                     "\"conformal_window_size\":500,"
                     "\"conformal_epsilon\":0.05"
                     "}"
                     "}"
                     "}";

   char *out = NULL;
   size_t out_len = 0;
   int rc = platform_exec_pipe("python3 ../scripts/calibration-sidecar.py", req, strlen(req), &out,
                               &out_len);
   assert(rc == 0);
   assert(out != NULL);
   assert(out_len > 0);

   cJSON *root = cJSON_ParseWithLength(out, out_len);
   free(out);
   assert(root != NULL);

   cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
   assert(cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0);

   cJSON *profile = cJSON_GetObjectItemCaseSensitive(root, "profile");
   assert(cJSON_IsObject(profile));
   cJSON *prompt_version = cJSON_GetObjectItemCaseSensitive(profile, "prompt_version");
   cJSON *model_version = cJSON_GetObjectItemCaseSensitive(profile, "model_version");
   assert(cJSON_IsString(prompt_version) && strcmp(prompt_version->valuestring, "fixture-v1") == 0);
   assert(cJSON_IsString(model_version) &&
          strcmp(model_version->valuestring, "beta-binomial-v1") == 0);
   cJSON *conformal = cJSON_GetObjectItemCaseSensitive(profile, "conformal");
   assert(cJSON_IsObject(conformal));
   cJSON *reject_below = cJSON_GetObjectItemCaseSensitive(conformal, "reject_below");
   assert(cJSON_IsNumber(reject_below));
   assert(fabs(reject_below->valuedouble - 0.90) < 1e-9);

   cJSON *buckets = cJSON_GetObjectItemCaseSensitive(profile, "buckets");
   assert(cJSON_IsArray(buckets) && cJSON_GetArraySize(buckets) == 10);
   cJSON *bucket_8 = cJSON_GetArrayItem(buckets, 8);
   assert(cJSON_IsObject(bucket_8));
   cJSON *lower_8 = cJSON_GetObjectItemCaseSensitive(bucket_8, "lower_credible_bound");
   assert(cJSON_IsNumber(lower_8));
   assert(fabs(lower_8->valuedouble - 0.5947044352095313) < 1e-9);

   cJSON *bucket_9 = cJSON_GetArrayItem(buckets, 9);
   assert(cJSON_IsObject(bucket_9));
   cJSON *lower_9 = cJSON_GetObjectItemCaseSensitive(bucket_9, "lower_credible_bound");
   assert(cJSON_IsNumber(lower_9));
   assert(fabs(lower_9->valuedouble - 0.7997292658403681) < 1e-9);

   cJSON *sample_sizes = cJSON_GetObjectItemCaseSensitive(profile, "sample_sizes");
   assert(cJSON_IsObject(sample_sizes));
   cJSON *accepted = cJSON_GetObjectItemCaseSensitive(sample_sizes, "accepted");
   cJSON *rejected = cJSON_GetObjectItemCaseSensitive(sample_sizes, "rejected");
   assert(cJSON_IsNumber(accepted) && (int)accepted->valuedouble == 26);
   assert(cJSON_IsNumber(rejected) && (int)rejected->valuedouble == 3);

   cJSON_Delete(root);
   printf("  calibration_sidecar_fixture: ok\n");
}

/* ---- main ---- */
int main(void)
{
   printf("calibration:\n");

   test_profile_write();
   test_profile_read();
   test_scope_fallback();
   test_audit_stats_empty();
   test_audit_stats_bucket_and_scope();
   test_conformal_window();
   test_surfaces_empty();
   test_surface_list_discovers_audit_tuples();
   test_config_defaults();
   test_config_tau_and_versions();
   test_threshold_from_profile_json();
   test_kb_calibrate_run_high_bucket();
   test_kb_calibrate_run_discovers_dynamic_surface();
   test_calibration_sidecar_fixture();

   printf("All calibration tests passed.\n");
   return 0;
}
