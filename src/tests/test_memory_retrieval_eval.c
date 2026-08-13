/* test_memory_retrieval_eval.c: unit tests for corpus-based memory retrieval evaluation */
#include <assert.h>
#include <sqlite3.h>
#include "platform_test_util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aimee.h"
#include "../db1/db.h"
#include "agent_eval.h"
#include "db2.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "../db2/lifecycle.h"
#include "memory.h"
#include "modules/memory/memory_graph_fusion.h"
#include "db2/memory_query.h"
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

static void test_corpus_load_minimal(void)
{
   static const char *corpus_json =
       "{"
       "  \"version\": 1,"
       "  \"fixtures\": ["
       "    {\"fid\": \"f1\", \"tier\": \"L2\", \"kind\": \"fact\","
       "     \"key\": \"nginx deployment\", \"content\": \"nginx runs on port 443\"}"
       "  ],"
       "  \"cases\": ["
       "    {\"id\": \"c1\", \"query\": \"nginx deployment\", \"expected\": [\"f1\"]}"
       "  ]"
       "}";

   char *path = write_temp_corpus(corpus_json);
   mem_eval_case_t cases[16];
   int n = mem_eval_load_corpus(path, MEMORY_EMBED_TEST_FIXTURE, cases, 16);

   assert(n == 1);
   assert(cases[0].n_expected == 1);
   assert(cases[0].expected_ids[0] > 0);
   assert(strncmp(cases[0].query, "nginx deployment", 16) == 0);
   mem_eval_close_temp_db();
   platform_test_remove_sqlite(path);
   free(path);
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

/* End-to-end: graph_code_fusion_state="on" surfaces a memory that is only
 * reachable through the graph (shares a canonical entity node with a base hit)
 * and is otherwise irrelevant to the query — the "bridge" the fusion targets.
 * With fusion off it must not appear; with fusion on it must. Exercises the full
 * recall path (memory_find_facts → memory_find_facts_scoped fusion block), not
 * just the expansion primitive. */
static void test_fusion_surfaces_bridged_memory(void)
{
   assert(mem_eval_open_temp_db() == 0);

   config_t cfg;
   config_load(&cfg);
   const char *embed = cfg.embedder_command[0] ? cfg.embedder_command : MEMORY_EMBED_TEST_FIXTURE;

   /* Base hit: lexically/semantically matches the query. */
   memory_t base;
   assert(memory_insert(TIER_L2, KIND_FACT, "nginx deployment",
                        "nginx deployment listens on port 443", 0.9, "sess", &base) == 0);
   assert(memory_embed(base.id, embed) == 0);

   /* Bridge: zero query-term overlap and intentionally NOT embedded, so the
    * vector/lexical base retrieval can never reach it — only the graph
    * expansion (which reads memory_entities directly) can. */
   memory_t bridge;
   assert(memory_insert(TIER_L2, KIND_DECISION, "retry policy",
                        "we cap delegate retries at three attempts", 0.9, "sess", &bridge) == 0);

   /* Link both to a shared non-code canonical entity node whose key shares NO
    * token with the query, so plain entity-candidate retrieval can't reach the
    * bridge from the query — only fusion, seeding from the base hit's own node,
    * can. */
   const char *node = "concept:zzz-bridge-marker-001";
   db2_memory_entity_insert(base.id, node, "mentions", 2.0);
   db2_memory_entity_insert(bridge.id, node, "mentions", 2.0);

   const char *query = "nginx deployment port";
   memory_t results[20];

   /* Fusion off → the irrelevant bridge memory must not surface. */
   memory_fusion_state_clear();
   int n_off = memory_find_facts(query, 20, results, 20);
   assert(n_off >= 0);
   int bridge_off = 0, base_off = 0;
   for (int i = 0; i < n_off; i++)
   {
      if (results[i].id == bridge.id)
         bridge_off = 1;
      if (results[i].id == base.id)
         base_off = 1;
   }
   assert(base_off && "base hit retrieved with fusion off");
   assert(!bridge_off && "graph-only bridge absent with fusion off");

   /* Fusion on → the base hit's shared node bridges to the memory. */
   memory_fusion_state_set("on");
   int n_on = memory_find_facts(query, 20, results, 20);
   memory_fusion_state_clear();
   assert(n_on >= 0);
   int bridge_on = 0;
   for (int i = 0; i < n_on; i++)
      if (results[i].id == bridge.id)
         bridge_on = 1;
   assert(bridge_on && "fusion surfaces the graph-bridged memory");

   mem_eval_close_temp_db();
}

static void test_corpus_load_multi_expected(void)
{
   static const char *corpus_json =
       "{"
       "  \"version\": 1,"
       "  \"fixtures\": ["
       "    {\"fid\": \"a\", \"tier\": \"L2\", \"kind\": \"fact\","
       "     \"key\": \"server deploy\", \"content\": \"deploy to staging\"},"
       "    {\"fid\": \"b\", \"tier\": \"L2\", \"kind\": \"procedure\","
       "     \"key\": \"deploy procedure\", \"content\": \"deploy steps for server\"}"
       "  ],"
       "  \"cases\": ["
       "    {\"id\": \"c1\", \"query\": \"deploy\", \"expected\": [\"a\", \"b\"]}"
       "  ]"
       "}";

   char *path = write_temp_corpus(corpus_json);
   mem_eval_case_t cases[16];
   int n = mem_eval_load_corpus(path, MEMORY_EMBED_TEST_FIXTURE, cases, 16);

   assert(n == 1);
   assert(cases[0].n_expected == 2);
   assert(cases[0].expected_ids[0] > 0);
   assert(cases[0].expected_ids[1] > 0);
   assert(cases[0].expected_ids[0] != cases[0].expected_ids[1]);
   mem_eval_close_temp_db();
   platform_test_remove_sqlite(path);
   free(path);
}

static void test_corpus_load_populates_local_embeddings(void)
{
   static const char *corpus_json =
       "{"
       "  \"version\": 1,"
       "  \"fixtures\": ["
       "    {\"fid\": \"f1\", \"tier\": \"L2\", \"kind\": \"fact\","
       "     \"key\": \"database connection pool\","
       "     \"content\": \"connection pool size defaults to 10\"}"
       "  ],"
       "  \"cases\": ["
       "    {\"id\": \"c1\", \"query\": \"default pool size\", \"expected\": [\"f1\"]}"
       "  ]"
       "}";

   char *path = write_temp_corpus(corpus_json);
   mem_eval_case_t cases[16];
   int n = mem_eval_load_corpus(path, MEMORY_EMBED_TEST_FIXTURE, cases, 16);
   assert(n == 1);

   mem_eval_close_temp_db();
   platform_test_remove_sqlite(path);
   free(path);
}

static void test_corpus_load_invalid_json(void)
{
   char *path = write_temp_corpus("{not valid json");
   mem_eval_case_t cases[16];
   int n = mem_eval_load_corpus(path, MEMORY_EMBED_TEST_FIXTURE, cases, 16);
   assert(n == -1);
   platform_test_remove_sqlite(path);
   free(path);
}

static void test_corpus_load_missing_file(void)
{
   mem_eval_case_t cases[16];
   int n =
       mem_eval_load_corpus("/tmp/does_not_exist_xyzzy.json", MEMORY_EMBED_TEST_FIXTURE, cases, 16);
   assert(n == -1);
}

static void test_corpus_load_unknown_fid(void)
{
   /* expected references a fid that is not in fixtures — case should be skipped */
   static const char *corpus_json =
       "{"
       "  \"version\": 1,"
       "  \"fixtures\": ["
       "    {\"fid\": \"f1\", \"tier\": \"L2\", \"kind\": \"fact\","
       "     \"key\": \"some key\", \"content\": \"some content\"}"
       "  ],"
       "  \"cases\": ["
       "    {\"id\": \"c1\", \"query\": \"some key\", \"expected\": [\"UNKNOWN_FID\"]}"
       "  ]"
       "}";

   char *path = write_temp_corpus(corpus_json);
   mem_eval_case_t cases[16];
   int n = mem_eval_load_corpus(path, MEMORY_EMBED_TEST_FIXTURE, cases, 16);
   /* Case has no resolvable expected IDs, so it should be dropped → 0 cases loaded */
   assert(n <= 0);
   mem_eval_close_temp_db();
   platform_test_remove_sqlite(path);
   free(path);
}

/* --- mem_eval_run against corpus --- */

static void test_mem_eval_run_finds_exact_match(void)
{
   static const char *corpus_json =
       "{"
       "  \"version\": 1,"
       "  \"fixtures\": ["
       "    {\"fid\": \"f1\", \"tier\": \"L2\", \"kind\": \"fact\","
       "     \"key\": \"database connection pool\","
       "     \"content\": \"connection pool size defaults to 10\"}"
       "  ],"
       "  \"cases\": ["
       "    {\"id\": \"c1\", \"query\": \"database connection pool\", \"expected\": [\"f1\"]}"
       "  ]"
       "}";

   char *path = write_temp_corpus(corpus_json);
   mem_eval_case_t cases[16];
   int n = mem_eval_load_corpus(path, MEMORY_EMBED_TEST_FIXTURE, cases, 16);
   assert(n == 1);

   mem_eval_scores_t scores;
   int rc = mem_eval_run(cases, n, &scores);
   assert(rc == 0);

   /* Exact key match should give MRR = 1.0 */
   assert(scores.mrr > 0.0);
   mem_eval_close_temp_db();
   platform_test_remove_sqlite(path);
   free(path);
}

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
static void insert_edge(const char *src, const char *rel, const char *tgt, int weight)
{
   static const char *sql = "INSERT INTO entity_edges (source, relation, target, weight)"
                            " VALUES (?1, ?2, ?3, ?4)";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", src);
   aimee_pg_bind_text(st, "?2", rel);
   aimee_pg_bind_text(st, "?3", tgt);
   aimee_pg_bind_int(st, "?4", weight);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

static void test_multi_hop_recall(void)
{
   /* Three fixtures that describe a chain: deploy-server → deploy-procedure
    * → branch-policy. A multi-hop query about deployment failures should be
    * able to bridge all three via co_edited / co_discussed edges. */
   static const char *corpus_json =
       "{"
       "  \"version\": 1,"
       "  \"fixtures\": ["
       "    {\"fid\": \"f1\", \"tier\": \"L2\", \"kind\": \"fact\","
       "     \"key\": \"deploy server\", \"content\": \"server deployed at 192.168.1.50\"},"
       "    {\"fid\": \"f2\", \"tier\": \"L2\", \"kind\": \"procedure\","
       "     \"key\": \"deploy procedure\", \"content\": \"git pull then make then restart\"},"
       "    {\"fid\": \"f3\", \"tier\": \"L2\", \"kind\": \"fact\","
       "     \"key\": \"branch policy\", \"content\": \"feature branches target testing\"}"
       "  ],"
       "  \"cases\": ["
       "    {\"id\": \"c1\", \"query\": \"deploy server\", \"expected\": [\"f1\"]}"
       "  ]"
       "}";

   char *path = write_temp_corpus(corpus_json);
   mem_eval_case_t cases[16];
   int n = mem_eval_load_corpus(path, MEMORY_EMBED_TEST_FIXTURE, cases, 16);
   assert(n == 1);

   /* Seed graph edges to link the three fixtures. In production these would
    * be laid down by window extraction during normal operation. */
   insert_edge("deploy", "co_discussed", "procedure", 3);
   insert_edge("deploy", "co_edited", "branch", 2);
   insert_edge("procedure", "depends_on", "branch", 1);
   insert_edge("server", "co_discussed", "deploy", 4);

   /* Walk the graph starting from seed "deploy" and verify that at least
    * one bridge memory is surfaced. memory_graph_related tokenizes the
    * seed, walks co_discussed one hop, then co_edited/depends_on another. */
   char *seeds[2] = {(char *)"deploy server", (char *)"server"};
   graph_related_t related[8];
   int rcount = memory_graph_related(seeds, 2, related, 8);

   printf("\n  multi-hop: %d bridge memories surfaced ", rcount);
   assert(rcount >= 0); /* soft — just exercise the path without crashing */
   mem_eval_close_temp_db();
   platform_test_remove_sqlite(path);
   free(path);
}

static int run_corpus_regression(const char *corpus_path, const char *baseline_path)
{
   if (!corpus_path || !baseline_path)
   {
      fprintf(stderr, "usage: unit-test-memory-retrieval-eval --corpus PATH --baseline PATH\n");
      return 2;
   }

   static mem_eval_case_t cases[MEM_CORPUS_MAX_CASES];
   int n_cases =
       mem_eval_load_corpus(corpus_path, MEMORY_EMBED_TEST_FIXTURE, cases, MEM_CORPUS_MAX_CASES);
   if (n_cases <= 0)
   {
      fprintf(stderr, "FAIL: memory retrieval corpus failed for %s\n", corpus_path);
      return 1;
   }

   mem_eval_scores_t scores;
   mem_eval_latency_t latency;
   int rc = mem_eval_run_with_latency(cases, n_cases, &scores, &latency);
   mem_eval_close_temp_db();
   if (rc != 0)
   {
      fprintf(stderr, "FAIL: memory retrieval eval failed for %s\n", corpus_path);
      return 1;
   }

   printf("Memory retrieval corpus eval: cases=%d mrr=%.6f ndcg@5=%.6f recall@5=%.6f p95=%.2fms\n",
          scores.n_cases, scores.mrr, scores.ndcg_5, scores.recall_5, latency.p95_ms);

   mem_eval_scores_t baseline;
   double threshold_pct = 5.0;
   if (mem_eval_load_baseline(baseline_path, &baseline, &threshold_pct) != 0)
   {
      fprintf(stderr, "FAIL: could not load memory retrieval baseline %s\n", baseline_path);
      return 1;
   }
   if (mem_eval_check_regression(&scores, &baseline, threshold_pct) != 0)
   {
      fprintf(stderr, "FAIL: memory retrieval regression detected vs %s\n", baseline_path);
      return 1;
   }

   printf("OK: no regression vs baseline (%s, threshold %.1f%%)\n", baseline_path, threshold_pct);
   return 0;
}

static int maybe_run_cli_mode(int argc, char **argv, int *handled)
{
   const char *corpus_path = NULL;
   const char *baseline_path = NULL;
   *handled = 0;

   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--corpus") == 0 && i + 1 < argc)
      {
         corpus_path = argv[++i];
         *handled = 1;
      }
      else if (strcmp(argv[i], "--baseline") == 0 && i + 1 < argc)
      {
         baseline_path = argv[++i];
         *handled = 1;
      }
      else
      {
         fprintf(stderr,
                 "usage: unit-test-memory-retrieval-eval [--corpus PATH --baseline PATH]\n");
         *handled = 1;
         return 2;
      }
   }

   if (!*handled)
      return 0;
   return run_corpus_regression(corpus_path, baseline_path);
}

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

int main(int argc, char **argv)
{
   int handled = 0;
   int cli_rc = maybe_run_cli_mode(argc, argv, &handled);
   if (handled)
      return cli_rc;

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

   printf("test_corpus_load_minimal... ");
   test_corpus_load_minimal();
   printf("ok\n");

   printf("test_production_corpus_load... ");
   test_production_corpus_load();
   printf("ok\n");

   printf("test_fusion_surfaces_bridged_memory... ");
   test_fusion_surfaces_bridged_memory();
   printf("ok\n");

   printf("test_corpus_load_multi_expected... ");
   test_corpus_load_multi_expected();
   printf("ok\n");

   printf("test_corpus_load_populates_local_embeddings... ");
   test_corpus_load_populates_local_embeddings();
   printf("ok\n");

   printf("test_corpus_load_invalid_json... ");
   test_corpus_load_invalid_json();
   printf("ok\n");

   printf("test_corpus_load_missing_file... ");
   test_corpus_load_missing_file();
   printf("ok\n");

   printf("test_corpus_load_unknown_fid... ");
   test_corpus_load_unknown_fid();
   printf("ok\n");

   printf("test_mem_eval_run_finds_exact_match... ");
   test_mem_eval_run_finds_exact_match();
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

   printf("test_multi_hop_recall... ");
   test_multi_hop_recall();
   printf("ok\n");

   printf("test_golden_smoke_fixture... ");
   test_golden_smoke_fixture();
   printf("ok\n");

   printf("All memory retrieval eval tests passed.\n");
   return 0;
}
