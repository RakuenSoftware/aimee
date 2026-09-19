/* Corpus seeding, embedding, malformed inputs and baseline regression now run
 * in Go: modules/memory/cmd/aimee-memory-eval/corpus_test.go. Graph traversal
 * regressions run in modules/memory/graph_score_test.go. */
#include "json_fluent.h"
#include "module_commands.h"
/* test_memory_retrieval_eval.c: unit tests for corpus-based memory retrieval evaluation */
#include <assert.h>
#include "modules/db2/c/db2_test_shim.h"
#include <sqlite3.h>
#include "platform_test_util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aimee.h"
#include "agent_eval.h"
#include "modules/db2/c/db2.h"
#include "../modules/db2/c/db2_internal.h"
#include "../modules/db2/c/db_postgres.h"
#include "../modules/db2/c/lifecycle.h"
#include "memory.h"
#include "modules/db2/c/memory_query.h"
#include "modules/db2/c/memory_vectors.h"
#include "config.h"
#include "agent_eval_internal.h"

/* --- IR metric tests --- */

static void test_mrr_hit_first(void)
{
   int64_t retrieved[] = {10, 20, 30};
   int64_t relevant[] = {10};
   double mrr = ir_mrr(retrieved, 3, relevant, 1);
   assert(fabs(mrr - 1.0) < 1e-9);
}

static void test_mrr_hit_second(void)
{
   int64_t retrieved[] = {99, 10, 30};
   int64_t relevant[] = {10};
   double mrr = ir_mrr(retrieved, 3, relevant, 1);
   assert(fabs(mrr - 0.5) < 1e-9);
}

static void test_mrr_miss(void)
{
   int64_t retrieved[] = {1, 2, 3};
   int64_t relevant[] = {99};
   double mrr = ir_mrr(retrieved, 3, relevant, 1);
   assert(fabs(mrr) < 1e-9);
}

static void test_mrr_empty(void)
{
   double mrr = ir_mrr(NULL, 0, NULL, 0);
   assert(fabs(mrr) < 1e-9);
}

static void test_ndcg_perfect(void)
{
   int64_t retrieved[] = {1, 2, 3};
   int64_t relevant[] = {1, 2, 3};
   double ndcg = ir_ndcg_at_k(retrieved, 3, relevant, 3, 5);
   assert(fabs(ndcg - 1.0) < 1e-9);
}

static void test_ndcg_zero(void)
{
   int64_t retrieved[] = {4, 5, 6};
   int64_t relevant[] = {1, 2, 3};
   double ndcg = ir_ndcg_at_k(retrieved, 3, relevant, 3, 5);
   assert(fabs(ndcg) < 1e-9);
}

static void test_recall_perfect(void)
{
   int64_t retrieved[] = {1, 2, 3};
   int64_t relevant[] = {1, 2};
   double recall = ir_recall_at_k(retrieved, 3, relevant, 2, 5);
   assert(fabs(recall - 1.0) < 1e-9);
}

static void test_recall_partial(void)
{
   int64_t retrieved[] = {1, 9, 9};
   int64_t relevant[] = {1, 2};
   double recall = ir_recall_at_k(retrieved, 3, relevant, 2, 5);
   assert(fabs(recall - 0.5) < 1e-9);
}

static void test_recall_zero(void)
{
   int64_t retrieved[] = {9};
   int64_t relevant[] = {1, 2};
   double recall = ir_recall_at_k(retrieved, 1, relevant, 2, 5);
   assert(fabs(recall) < 1e-9);
}

/* --- Corpus loading tests --- */

/* Write a minimal JSON corpus to a temp file and return the path. Caller frees. */
static char *write_temp_corpus(const char *json)
{
   char path_tmpl[256];
   snprintf(path_tmpl, sizeof path_tmpl, "%s/test_corpus_XXXXXX.json", platform_tmpdir());
   char *path = strdup(path_tmpl);
   int fd = mkstemps(path, 5);
   assert(fd >= 0);
   write(fd, json, strlen(json));
   close(fd);
   return path;
}

/* Write a minimal baseline JSON to a temp file and return the path. Caller frees. */
static char *write_temp_baseline(const char *json)
{
   char path_tmpl[256];
   snprintf(path_tmpl, sizeof path_tmpl, "%s/test_baseline_XXXXXX.json", platform_tmpdir());
   char *path = strdup(path_tmpl);
   int fd = mkstemps(path, 5);
   assert(fd >= 0);
   write(fd, json, strlen(json));
   close(fd);
   return path;
}

/* The production-corpus loader reads pre-resolved live DB2 ids from
 * `expected_ids` and opens no scratch DB; every well-formed query is loaded,
 * including ones still awaiting labelling (empty expected_ids). */
static void test_production_corpus_load(void)
{
   static const char *corpus_json =
       "{"
       "  \"version\": 1,"
       "  \"query_count\": 2,"
       "  \"queries\": ["
       "    {\"query\": \"src/memory_graph.c\", \"category\": \"code_file\","
       "     \"code_shaped\": true, \"expected_ids\": [101, 202]},"
       "    {\"query\": \"where is foo defined\", \"category\": \"code_file\","
       "     \"code_shaped\": false, \"expected_ids\": []}"
       "  ]"
       "}";

   char *path = write_temp_corpus(corpus_json);
   mem_eval_case_t cases[16];
   int n = mem_eval_load_production_corpus(path, cases, 16);

   assert(n == 2); /* both queries load, including the unlabelled one */
   assert(cases[0].n_expected == 2);
   assert(cases[0].expected_ids[0] == 101);
   assert(cases[0].expected_ids[1] == 202);
   assert(strncmp(cases[0].query, "src/memory_graph.c", 18) == 0);
   assert(cases[1].n_expected == 0); /* unlabelled query still present */
   /* No scratch DB is opened, so no mem_eval_close_temp_db() teardown. */
   platform_test_remove_sqlite(path);
   free(path);
}

/* Graph-fusion admission and scope coverage now lives in Go graph_score_test.go. */

/* --- mem_eval_run against corpus --- */

/* --- Baseline load/save/check tests --- */

static void test_baseline_load_save_roundtrip(void)
{
   mem_eval_scores_t scores = {
       .mrr = 0.85,
       .ndcg_5 = 0.80,
       .ndcg_10 = 0.76,
       .recall_5 = 0.70,
       .recall_10 = 0.78,
       .n_cases = 42,
   };

   char path_tmpl[256];
   snprintf(path_tmpl, sizeof path_tmpl, "%s/test_baseline_rt_XXXXXX.json", platform_tmpdir());
   char *path = strdup(path_tmpl);
   int fd = mkstemps(path, 5);
   close(fd);

   int rc = mem_eval_save_baseline(path, &scores, 5.0);
   assert(rc == 0);

   mem_eval_scores_t loaded;
   double threshold = 0.0;
   rc = mem_eval_load_baseline(path, &loaded, &threshold);
   assert(rc == 0);

   assert(fabs(loaded.mrr - scores.mrr) < 1e-5);
   assert(fabs(loaded.ndcg_5 - scores.ndcg_5) < 1e-5);
   assert(fabs(loaded.ndcg_10 - scores.ndcg_10) < 1e-5);
   assert(fabs(loaded.recall_5 - scores.recall_5) < 1e-5);
   assert(fabs(loaded.recall_10 - scores.recall_10) < 1e-5);
   assert(loaded.n_cases == scores.n_cases);
   assert(fabs(threshold - 5.0) < 1e-9);

   platform_test_remove_sqlite(path);
   free(path);
}

static void test_baseline_load_missing(void)
{
   mem_eval_scores_t out;
   int rc = mem_eval_load_baseline("/tmp/no_such_baseline_xyz.json", &out, NULL);
   assert(rc == -1);
}

static void test_regression_check_no_regression(void)
{
   mem_eval_scores_t baseline = {.mrr = 0.80, .ndcg_5 = 0.75};
   mem_eval_scores_t scores = {.mrr = 0.82, .ndcg_5 = 0.76}; /* better than baseline */
   int result = mem_eval_check_regression(&scores, &baseline, 5.0);
   assert(result == 0);
}

static void test_regression_check_regression(void)
{
   mem_eval_scores_t baseline = {
       .mrr = 0.80, .ndcg_5 = 0.75, .ndcg_10 = 0.70, .recall_5 = 0.65, .recall_10 = 0.72};
   /* Drop MRR by 10% → should exceed 5% threshold */
   mem_eval_scores_t scores = {
       .mrr = 0.70, .ndcg_5 = 0.75, .ndcg_10 = 0.70, .recall_5 = 0.65, .recall_10 = 0.72};
   int result = mem_eval_check_regression(&scores, &baseline, 5.0);
   assert(result == 1);
}

static void test_regression_check_below_threshold(void)
{
   mem_eval_scores_t baseline = {.mrr = 1.0};
   /* 4% drop — well below the 5% threshold, should not trigger */
   mem_eval_scores_t scores = {.mrr = 0.96};
   int result = mem_eval_check_regression(&scores, &baseline, 5.0);
   assert(result == 0);
}

static void test_regression_check_zero_baseline(void)
{
   /* Baseline = 0.0 means that metric is not tracked; no false regression */
   mem_eval_scores_t baseline = {.mrr = 0.0, .ndcg_5 = 0.0};
   mem_eval_scores_t scores = {.mrr = 0.0, .ndcg_5 = 0.0};
   int result = mem_eval_check_regression(&scores, &baseline, 5.0);
   assert(result == 0);
}

/* --- Baseline load from static JSON string (parsing test) --- */

/* --- Multi-hop recall test ---
 *
 * Complex cases in the corpus require connecting 2-3 fixtures via graph
 * edges. This test inserts a small set of fixtures, seeds entity_edges to
 * simulate co-occurrence across sessions, then uses memory_graph_related()
 * to walk the graph and checks that bridge memories surface. The assertion
 * is soft (>= 0) because graph coverage depends on edge density; the
 * primary goal is to exercise the multi-hop traversal code path and
 * report the metric. */

static void test_baseline_load_from_file(void)
{
   static const char *json = "{\"mrr\": 0.75, \"ndcg_5\": 0.72, \"ndcg_10\": 0.70,"
                             " \"recall_5\": 0.65, \"recall_10\": 0.73, \"n_cases\": 105,"
                             " \"threshold_pct\": 5.0}";

   char *path = write_temp_baseline(json);

   mem_eval_scores_t out;
   double threshold = 0.0;
   int rc = mem_eval_load_baseline(path, &out, &threshold);
   assert(rc == 0);
   assert(fabs(out.mrr - 0.75) < 1e-6);
   assert(fabs(out.ndcg_5 - 0.72) < 1e-6);
   assert(fabs(threshold - 5.0) < 1e-9);
   assert(out.n_cases == 105);

   platform_test_remove_sqlite(path);
   free(path);
}

static void test_agent_eval_ablation_presets(void)
{
   agent_ablation_flags_t flags;

   assert(agent_eval_ablation_preset(NULL, &flags) == 0);
   assert(flags.configured == 1);
   assert(flags.rescue == 1);
   assert(flags.respond_tool == 1);
   assert(flags.sampling_defaults == 1);
   assert(flags.normalize == 1);
   assert(flags.retry == 1);

   assert(agent_eval_ablation_preset("no_rescue", &flags) == 0);
   assert(flags.configured == 1);
   assert(flags.rescue == 0);
   assert(flags.respond_tool == 1);
   assert(flags.sampling_defaults == 1);
   assert(flags.normalize == 1);
   assert(flags.retry == 1);

   assert(agent_eval_ablation_preset("bare", &flags) == 0);
   assert(flags.configured == 1);
   assert(flags.rescue == 0);
   assert(flags.respond_tool == 0);
   assert(flags.sampling_defaults == 0);
   assert(flags.normalize == 0);
   assert(flags.retry == 0);

   assert(agent_eval_ablation_preset("definitely_not_a_preset", &flags) != 0);
}

static void test_agent_eval_manifest_comparability(void)
{
   eval_task_t tasks[1];
   memset(tasks, 0, sizeof(tasks));
   snprintf(tasks[0].name, sizeof(tasks[0].name), "manifest fixture");
   snprintf(tasks[0].prompt, sizeof(tasks[0].prompt), "answer from evidence");
   snprintf(tasks[0].role, sizeof(tasks[0].role), "review");
   snprintf(tasks[0].success_check_type, sizeof(tasks[0].success_check_type), "contains");
   snprintf(tasks[0].success_check_value, sizeof(tasks[0].success_check_value), "ok");

   agent_eval_manifest_t a, b;
   assert(agent_eval_manifest_build("suite", tasks, 1, "agent:model", 7, &a) == 0);
   assert(agent_eval_manifest_build("suite", tasks, 1, "agent:model", 7, &b) == 0);
   assert(strlen(a.dataset_hash) == 64 && strlen(a.target_hash) == 64);
   assert(a.hardware_profile[0] && strcmp(a.harness_version, "2") == 0);
   const char *reason = NULL;
   assert(agent_eval_manifest_compare(&a, &b, AGENT_EVAL_COMPARE_QUALITY, &reason) ==
          AGENT_EVAL_COMPARABLE);
   assert(strcmp(reason, "comparable") == 0);

   snprintf(b.hardware_profile, sizeof(b.hardware_profile), "different-machine");
   assert(agent_eval_manifest_compare(&a, &b, AGENT_EVAL_COMPARE_QUALITY, &reason) ==
          AGENT_EVAL_COMPARABLE);
   assert(agent_eval_manifest_compare(&a, &b, AGENT_EVAL_COMPARE_LATENCY, &reason) ==
          AGENT_EVAL_INCOMPARABLE);
   assert(strcmp(reason, "hardware_changed") == 0);

   b = a;
   b.seed++;
   assert(agent_eval_manifest_compare(&a, &b, AGENT_EVAL_COMPARE_QUALITY, &reason) ==
          AGENT_EVAL_INCOMPARABLE);
   assert(strcmp(reason, "seed_changed") == 0);
   b = a;
   b.dataset_hash[0] = '\0';
   assert(agent_eval_manifest_compare(&a, &b, AGENT_EVAL_COMPARE_QUALITY, &reason) ==
          AGENT_EVAL_COMPARABILITY_UNKNOWN);
}

/* --- Golden smoke fixture for Phase 0 (effectiveness-weighted code vector graph) --- */

/* 30 queries covering code/file, memory/recall, config/identity, agent/session,
 * infrastructure/ops, and memory quality/learning topics. Phase 7 will extend
 * these with expected result sets for production-gate validation. */
static const char *golden_smoke_queries[] = {
    /* code/file relationships */
    "db2 entity edges schema",
    "memory graph traversal",
    "session key building",
    "delivery router platform",
    "kb client search fusion mode",
    /* memory/recall topics */
    "contextual bandits exploration",
    "virtual context assembly",
    "guardrails semantic sidecar",
    "ACP delegate transport",
    "minimax tool call arguments",
    /* configuration/identity */
    "working profile injection",
    "identity snapshot",
    "config gate enable",
    "plugin kind taxonomy",
    "delegate execution role",
    /* agent/session lifecycle */
    "primary agent max turns",
    "session compaction",
    "agent bridge tool budget",
    "delegate nudge suppress",
    "session key canonical",
    /* infrastructure/ops */
    "gateway delivery target ntfy",
    "webhook HMAC verification",
    "delivery mirror transcript",
    "gateway platform registry",
    "session key thread",
    /* memory quality/learning */
    "utility score half life",
    "memory contradiction detection",
    "memory lifecycle state",
    "effectiveness weighted recall",
    "memory graph boost utility",
};
static const int golden_smoke_count =
    sizeof(golden_smoke_queries) / sizeof(golden_smoke_queries[0]);

static void test_golden_smoke_fixture(void)
{
   for (int i = 0; i < golden_smoke_count; i++)
   {
      const char *q = golden_smoke_queries[i];
      assert(strlen(q) > 0);
      assert(strlen(q) < 256);
      printf("smoke fixture: %s\n", q);
   }
}

int main(void)
{
   /* The eval scratch store needs a disposable database when the test shim is
    * backed by Postgres; a no-op under the sqlite shim, which makes its own
    * in-memory handle. */
   assert(db2_test_shim_prepare_eval_store() == 0);

   printf("test_mrr_hit_first... ");
   test_mrr_hit_first();
   printf("ok\n");

   printf("test_mrr_hit_second... ");
   test_mrr_hit_second();
   printf("ok\n");

   printf("test_mrr_miss... ");
   test_mrr_miss();
   printf("ok\n");

   printf("test_mrr_empty... ");
   test_mrr_empty();
   printf("ok\n");

   printf("test_ndcg_perfect... ");
   test_ndcg_perfect();
   printf("ok\n");

   printf("test_ndcg_zero... ");
   test_ndcg_zero();
   printf("ok\n");

   printf("test_recall_perfect... ");
   test_recall_perfect();
   printf("ok\n");

   printf("test_recall_partial... ");
   test_recall_partial();
   printf("ok\n");

   printf("test_recall_zero... ");
   test_recall_zero();
   printf("ok\n");

   printf("test_production_corpus_load... ");
   test_production_corpus_load();
   printf("ok\n");

   printf("test_baseline_load_save_roundtrip... ");
   test_baseline_load_save_roundtrip();
   printf("ok\n");

   printf("test_baseline_load_missing... ");
   test_baseline_load_missing();
   printf("ok\n");

   printf("test_regression_check_no_regression... ");
   test_regression_check_no_regression();
   printf("ok\n");

   printf("test_regression_check_regression... ");
   test_regression_check_regression();
   printf("ok\n");

   printf("test_regression_check_below_threshold... ");
   test_regression_check_below_threshold();
   printf("ok\n");

   printf("test_regression_check_zero_baseline... ");
   test_regression_check_zero_baseline();
   printf("ok\n");

   printf("test_baseline_load_from_file... ");
   test_baseline_load_from_file();
   printf("ok\n");

   printf("test_agent_eval_ablation_presets... ");
   test_agent_eval_ablation_presets();
   printf("ok\n");

   printf("test_agent_eval_manifest_comparability... ");
   test_agent_eval_manifest_comparability();
   printf("ok\n");

   printf("test_golden_smoke_fixture... ");
   test_golden_smoke_fixture();
   printf("ok\n");

   printf("All memory retrieval eval tests passed.\n");
   return 0;
}
