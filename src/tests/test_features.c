/* test_features.c — unit tests for the feature_rows substrate and KB ranker.
 *
 * Tests:
 *   1. feature_row_upsert: write a feature row and read it back.
 *   2. feature_row_upsert_conflict: upsert updates existing row.
 *   3. kb_features_upsert: compute and store lex/dense/temp features.
 *   4. kb_features_read: retrieve stored feature row.
 *   5. kb_features_upsert_with_sketch: emits sketch.* features.
 *   6. ranker_model_write: write a ranker_model artifact.
 *   7. ranker_rerank_noop: ranker returns 0 when no model loaded.
 *   8. ranker_rerank_sketch_features: ranker consumes sketch.* weights.
 *   9. config_ranking_defaults: ranking config defaults are sane.
 *   10. drift_detect_disabled: kb_detect_observe returns 0 when disabled.
 *   11. sketch_store_params_json: persisted sketches declare params + rotation.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "feature_rows.h"
#include "db2_test_shim.h"
#include "../db2/db_postgres.h"
#include "../db2/lifecycle.h"
#include "../db2/sketch.h"
#include "../kb_features.h"
#include "../kb_ranker.h"
#include "../kb_detect.h"
#include "config.h"
#include "config_learning.h"

static void open_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void close_db(void)
{
   db2_test_shim_close();
}

/* ---- 1. feature_row_upsert ---- */
static void test_feature_row_upsert(void)
{
   open_db();

   const char *json = "{\"lex.cos\":0.85,\"dense.cos\":0.92}";
   int rc = db2_feature_row_upsert("doc-1", "kb_document", "", "", "v1", json, NULL);
   assert(rc == 0);

   char buf[512];
   int rr = db2_feature_row_read("doc-1", "kb_document", "v1", buf, sizeof(buf));
   assert(rr == 0);
   assert(strstr(buf, "0.85") != NULL);
   assert(strstr(buf, "0.92") != NULL);

   close_db();
   printf("  feature_row_upsert: ok\n");
}

/* ---- 2. feature_row_upsert_conflict ---- */
static void test_feature_row_upsert_conflict(void)
{
   open_db();

   db2_feature_row_upsert("doc-2", "kb_document", "", "", "v1", "{\"lex.cos\":0.5}", NULL);
   db2_feature_row_upsert("doc-2", "kb_document", "", "", "v1", "{\"lex.cos\":0.75}", NULL);

   char buf[256];
   assert(db2_feature_row_read("doc-2", "kb_document", "v1", buf, sizeof(buf)) == 0);
   assert(strstr(buf, "0.75") != NULL);

   close_db();
   printf("  feature_row_upsert_conflict: ok\n");
}

/* ---- 3. kb_features_upsert ---- */
static void test_kb_features_upsert(void)
{
   open_db();

   int rc = kb_features_upsert(999, 0.7, 0.88, 5.0);
   assert(rc == 0);

   char buf[512];
   assert(kb_features_read(999, buf, sizeof(buf)) == 0);
   assert(strstr(buf, "lex.cos") != NULL);
   assert(strstr(buf, "dense.cos") != NULL);
   assert(strstr(buf, "temp.age_days") != NULL);
   assert(strstr(buf, "temp.recency") != NULL);

   close_db();
   printf("  kb_features_upsert: ok\n");
}

/* ---- 4. kb_features_read_miss ---- */
static void test_kb_features_read_miss(void)
{
   open_db();

   char buf[256];
   int rc = kb_features_read(99999999, buf, sizeof(buf));
   assert(rc == -1);

   close_db();
   printf("  kb_features_read_miss: ok\n");
}

/* ---- 5. kb_features_upsert_with_sketch ---- */
static void test_kb_features_upsert_with_sketch(void)
{
   open_db();

   kb_sketch_features_t sketch = {3.0, 42.0};
   int rc = kb_features_upsert_with_sketch(1001, 0.7, 0.88, 5.0, &sketch);
   assert(rc == 0);

   char buf[512];
   assert(kb_features_read(1001, buf, sizeof(buf)) == 0);
   assert(strstr(buf, "sketch.frequency_kind_scope") != NULL);
   assert(strstr(buf, "sketch.distinct_sources_hll") != NULL);
   assert(strstr(buf, "42.000000") != NULL);

   close_db();
   printf("  kb_features_upsert_with_sketch: ok\n");
}

/* ---- 6. ranker_model_write ---- */
static void test_ranker_model_write(void)
{
   open_db();

   char id[64];
   int rc = kb_ranker_model_write("{\"dense.cos\":0.6,\"lex.cos\":0.4,\"temp.recency\":0.0}", id,
                                  sizeof(id));
   assert(rc == 0);
   assert(strlen(id) == 36);

   close_db();
   printf("  ranker_model_write: ok\n");
}

/* ---- 7. ranker_rerank_noop ---- */
static void test_ranker_rerank_noop(void)
{
   /* When no model is loaded in the global cache, rerank returns 0. */
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_ranker_enabled = 1;

   int64_t ids[3] = {10, 20, 30};
   double lex[3] = {0.5, 0.6, 0.4};
   double dense[3] = {0.7, 0.8, 0.6};
   double age[3] = {1.0, 2.0, 0.5};
   int64_t ranked[3];

   /* The global weight cache starts empty (loaded=0) in a fresh process.
    * After test_ranker_model_write above opened+closed the DB without calling
    * kb_ranker_model_load(), the cache should still be unloaded. */
   int n = kb_ranker_rerank(&cfg, ids, lex, dense, age, 3, ranked, NULL);
   /* Returns 0 because g_weights.loaded == 0. */
   assert(n == 0);

   printf("  ranker_rerank_noop: ok\n");
}

/* ---- 8. ranker_rerank_sketch_features ---- */
static void test_ranker_rerank_sketch_features(void)
{
   open_db();

   char id[64];
   int rc = kb_ranker_model_write("{\"dense.cos\":0.0,\"lex.cos\":0.0,\"temp.recency\":0.0,"
                                  "\"sketch.frequency_kind_scope\":1.0,"
                                  "\"sketch.distinct_sources_hll\":0.0}",
                                  id, sizeof(id));
   assert(rc == 0);
   assert(kb_ranker_model_load() == 0);

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_ranker_enabled = 1;

   int64_t ids[2] = {10, 20};
   double lex[2] = {0.0, 0.0};
   double dense[2] = {0.0, 0.0};
   double age[2] = {0.0, 0.0};
   kb_sketch_features_t sketch[2] = {{1.0, 0.0}, {5.0, 0.0}};
   int64_t ranked[2];
   double scores[2];

   int n = kb_ranker_rerank_with_sketch(&cfg, ids, lex, dense, age, sketch, 2, ranked, scores);
   assert(n == 2);
   assert(ranked[0] == 20);
   assert(scores[0] > scores[1]);

   close_db();
   printf("  ranker_rerank_sketch_features: ok\n");
}

/* ---- 9. config_ranking_defaults ---- */
static void test_config_ranking_defaults(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   config_apply_ranking_settings(&cfg, NULL);

   assert(cfg.kb_ranker_enabled == 0);
   assert(cfg.ranker_fuse_command[0] == '\0');
   assert(cfg.drift_detect_shadow_enabled == 0);

   printf("  config_ranking_defaults: ok\n");
}

/* ---- 10. drift_detect_disabled ---- */
static void test_drift_detect_disabled(void)
{
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.drift_detect_shadow_enabled = 0;

   int rc = kb_detect_observe(&cfg, 0.85, 10);
   assert(rc == 0);

   printf("  drift_detect_disabled: ok\n");
}

static void read_sketch_params(const char *sketch_kind, const char *scope_id,
                               const char *feature_family, char *out, size_t out_cap)
{
   char err[256] = "";
   const char *sql = "SELECT params_json FROM sketch_store"
                     " WHERE sketch_kind = ?1 AND scope_kind = 'kb_project'"
                     "   AND scope_id = ?2 AND feature_family = ?3";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st != NULL);
   aimee_pg_bind_text(st, "?1", sketch_kind);
   aimee_pg_bind_text(st, "?2", scope_id);
   aimee_pg_bind_text(st, "?3", feature_family);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   snprintf(out, out_cap, "%s", aimee_pg_column_text(st, 0));
   aimee_pg_finalize(st);
}

static void assert_contains(const char *haystack, const char *needle)
{
   assert(strstr(haystack, needle) != NULL);
}

/* ---- 11. sketch_store_params_json ---- */
static void test_sketch_store_params_json(void)
{
   open_db();

   sketch_bloom_t bloom;
   sketch_minhash_t minhash;
   sketch_count_min_t count_min;
   sketch_hll_t hll;

   sketch_bloom_init(&bloom);
   sketch_minhash_init(&minhash);
   sketch_count_min_init(&count_min);
   sketch_hll_init(&hll);

   sketch_bloom_add_hash(&bloom, sketch_fnv1a("doc-a", 5));
   sketch_minhash_add_text(&minhash, "alpha beta gamma delta epsilon", 5);
   sketch_count_min_add_hash(&count_min, sketch_fnv1a("alpha", 5), 3);
   sketch_hll_add_hash(&hll, sketch_fnv1a("source-a", 8));

   assert(db2_sketch_bloom_save(&bloom, "kb_project", "params_project", "file_hash") == 0);
   assert(db2_sketch_minhash_save(&minhash, "kb_project", "params_project", "text_shingles") == 0);
   assert(db2_sketch_count_min_save(&count_min, "kb_project", "params_project", "token") == 0);
   assert(db2_sketch_hll_save(&hll, "kb_project", "params_project", "source_id") == 0);

   char params[256];
   read_sketch_params("bloom", "params_project", "file_hash", params, sizeof(params));
   assert_contains(params, "\"m\":1048576");
   assert_contains(params, "\"k\":7");
   assert_contains(params, "\"bloom_rotate\":\"30d\"");

   read_sketch_params("minhash", "params_project", "text_shingles", params, sizeof(params));
   assert_contains(params, "\"permutations\":128");
   assert_contains(params, "\"lsh_bands\":32");
   assert_contains(params, "\"lsh_rows_per_band\":4");
   assert_contains(params, "\"lsh_refresh\":\"7d\"");

   read_sketch_params("count_min", "params_project", "token", params, sizeof(params));
   assert_contains(params, "\"width\":65536");
   assert_contains(params, "\"depth\":4");
   assert_contains(params, "\"count_min_reset\":\"30d\"");

   read_sketch_params("hll", "params_project", "source_id", params, sizeof(params));
   assert_contains(params, "\"precision\":12");
   assert_contains(params, "\"hll_reset\":\"never\"");

   char err[256] = "";
   assert(aimee_pg_exec(db2_conn(),
                        "UPDATE sketch_store SET params_json = '{}'"
                        " WHERE sketch_kind = 'bloom' AND scope_id = 'params_project'",
                        err, sizeof(err)) == 0);
   assert(db2_sketch_bloom_save(&bloom, "kb_project", "params_project", "file_hash") == 0);
   read_sketch_params("bloom", "params_project", "file_hash", params, sizeof(params));
   assert_contains(params, "\"bloom_rotate\":\"30d\"");

   close_db();
   printf("  sketch_store_params_json: ok\n");
}

/* ---- main ---- */
int main(void)
{
   printf("features:\n");

   test_feature_row_upsert();
   test_feature_row_upsert_conflict();
   test_kb_features_upsert();
   test_kb_features_read_miss();
   test_kb_features_upsert_with_sketch();
   test_ranker_model_write();
   test_ranker_rerank_noop();
   test_ranker_rerank_sketch_features();
   test_config_ranking_defaults();
   test_drift_detect_disabled();
   test_sketch_store_params_json();

   printf("All features tests passed.\n");
   return 0;
}
