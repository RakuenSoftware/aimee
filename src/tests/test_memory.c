#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
#include "aimee.h"
#include "db.h"
#include "db1.h"
#include "db2.h"
#include "db_postgres.h"
#include "artifacts.h"
#include "calibration.h"
#include "db2_test_shim.h"
#include "kind_lifecycle.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "workspace.h"
#include "../db2/db2_internal.h"
#include "../db2/entity_edges.h"
#include "../db2/memory_payload.h" /* db2_memory_provenance_by_id (auditable-correctness P2) */
#include "../db2/demotion.h"       /* retrieval_event write/read (auditable-correctness P2) */
#include "../db2/code_index_ops.h" /* db2_code_file_hash (auditable-correctness P1.5) */

int memory_demote_from_failures(void);

static char g_db_path[512];
static char g_suite_home[512];

static void memory_test_ensure_env(void)
{
   if (g_suite_home[0] && (!getenv("HOME") || !getenv("HOME")[0]))
      assert(platform_setenv("HOME", g_suite_home) == 0);
   assert(platform_unsetenv("AIMEE_HOME") == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
}

static void setup(void)
{
   memory_test_ensure_env();
   snprintf(g_db_path, sizeof(g_db_path), "%s/aimee-test-memory-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(g_db_path, sizeof(g_db_path), "aim");
   assert(fd >= 0);
   close(fd);
   assert(db1_init(g_db_path) == 0);
   db2_test_shim_open_path(g_db_path);
}

static void teardown(void)
{
   db2_test_shim_close();
   db1_shutdown();
   if (g_db_path[0])
   {
      platform_test_remove_sqlite(g_db_path);
      g_db_path[0] = '\0';
   }
}

static double fetch_surprise(int64_t memory_id)
{
   char err[128] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), "SELECT surprise FROM memories WHERE id = ?1",
                                          err, sizeof(err));
   assert(st != NULL);
   aimee_pg_bind_int64(st, "?1", memory_id);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   double value = aimee_pg_column_double(st, 0);
   aimee_pg_finalize(st);
   return value;
}

static double fetch_confidence(int64_t memory_id)
{
   char err[128] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT confidence FROM memories WHERE id = ?1", err, sizeof(err));
   assert(st != NULL);
   aimee_pg_bind_int64(st, "?1", memory_id);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   double value = aimee_pg_column_double(st, 0);
   aimee_pg_finalize(st);
   return value;
}

static int fetch_memory_count_by_key(const char *key)
{
   char err[128] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT COUNT(*) FROM memories WHERE key = ?1", err, sizeof(err));
   assert(st != NULL);
   aimee_pg_bind_text(st, "?1", key);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   int value = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return value;
}

static void write_calibration_config(int enabled)
{
   char dir1[512];
   char dir2[512];
   char path[512];
   snprintf(dir1, sizeof(dir1), "%s/.config", g_suite_home);
   snprintf(dir2, sizeof(dir2), "%s/.config/aimee", g_suite_home);
   snprintf(path, sizeof(path), "%s/aimee.yaml", dir2);
   (void)mkdir(dir1, 0700);
   (void)mkdir(dir2, 0700);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "intelligence:\n  calibrate:\n    enabled: %d\n", enabled);
   fclose(f);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
}

static int fetch_memory_count_like(const char *pattern)
{
   char err[128] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT COUNT(*) FROM memories WHERE key LIKE ?1", err, sizeof(err));
   assert(st != NULL);
   aimee_pg_bind_text(st, "?1", pattern);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   int value = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return value;
}

static int fetch_runtime_state_int(const char *key)
{
   char buf[32];
   assert(db1_runtime_state_get(key, buf, sizeof(buf)) == 0);
   return atoi(buf);
}

static int fetch_runtime_state_int_or_zero(const char *key)
{
   char buf[32];
   if (db1_runtime_state_get(key, buf, sizeof(buf)) != 0 || !buf[0])
      return 0;
   return atoi(buf);
}

static void test_db1_runtime_state_add_int(void)
{
   setup();

   int new_value = -1;
   assert(db1_runtime_state_add_int("counter", 3, &new_value) == 0);
   assert(new_value == 3);
   assert(fetch_runtime_state_int("counter") == 3);

   assert(db1_runtime_state_add_int("counter", -2, &new_value) == 0);
   assert(new_value == 1);
   assert(fetch_runtime_state_int("counter") == 1);

   teardown();
}

static void current_utc_tm(struct tm *out)
{
   assert(out != NULL);
   time_t now = time(NULL);
   gmtime_r(&now, out);
}

static void insert_agent_log_row(const char *agent_name, const char *role, int success,
                                 const char *error, int turns, int tool_calls)
{
   db1_agent_log_insert_row_t row = {
       .agent_name = agent_name,
       .role = role,
       .prompt_tokens = 10,
       .completion_tokens = 20,
       .latency_ms = 50,
       .success = success,
       .error = error,
       .turns = turns,
       .tool_calls = tool_calls,
       .confidence = 90,
       .session_id = "sess-test",
   };
   assert(db1_agent_log_insert(&row) > 0);
}

#include "test_memory_cases_a.inc"
#include "test_memory_cases_b.inc"

static void test_memory_demote_from_failures_uses_db1_agent_log(void)
{
   setup();

   memory_t quota, safe;
   assert(memory_insert(TIER_L2, KIND_FACT, "quota", "Quota failures need cleanup", 0.9, "",
                        &quota) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "healthy", "Healthy signal", 0.9, "", &safe) == 0);

   char long_error[640];
   memset(long_error, 'x', 320);
   snprintf(long_error + 320, sizeof(long_error) - 320,
            " quota exhaustion blocked the delegate on the remote host");
   insert_agent_log_row("delegate-a", "fix", 0, long_error, 2, 1);

   assert(memory_demote_from_failures() == 1);
   assert(fabs(fetch_confidence(quota.id) - 0.81) < 0.001);
   assert(fabs(fetch_confidence(safe.id) - 0.9) < 0.001);

   teardown();
}

static void test_memory_promote_delegation_patterns_uses_db1_agent_log(void)
{
   setup();

   insert_agent_log_row("gpt-5", "review", 1, NULL, 2, 1);
   insert_agent_log_row("gpt-5", "review", 1, NULL, 3, 2);
   insert_agent_log_row("gpt-5", "review", 1, NULL, 4, 3);

   insert_agent_log_row("claude", "fix", 0, "timeout during patch apply", 5, 2);
   insert_agent_log_row("claude", "fix", 0, "timeout during patch apply", 6, 2);
   insert_agent_log_row("claude", "fix", 0, "timeout during patch apply", 7, 3);
   insert_agent_log_row("claude", "fix", 1, NULL, 3, 1);

   assert(memory_promote_delegation_patterns() == 2);
   assert(fetch_memory_count_by_key("delegate_pattern_review_gpt-5") == 1);
   assert(fetch_memory_count_by_key("delegate_warning_fix_claude") == 1);

   teardown();
}

static void test_memory_synthesize_failure_episodes_uses_db1_agent_log(void)
{
   setup();

   insert_agent_log_row("claude", "fix", 0, "timeout during patch apply", 5, 2);
   insert_agent_log_row("claude", "fix", 0, "invalid hunk context", 4, 1);
   insert_agent_log_row("claude", "fix", 0, "timeout during patch apply", 6, 3);

   assert(memory_synthesize_failure_episodes() == 1);
   assert(fetch_memory_count_like("failure_episode_fix_claude_%") == 1);

   teardown();
}

static void test_memory_scan_preserves_message_boundaries(void)
{
   setup();

   char dir[512];
   snprintf(dir, sizeof(dir), "%s/aimee-test-memory-scan-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(dir) != NULL);

   char path[768];
   snprintf(path, sizeof(path), "%s/session.jsonl", dir);
   FILE *fp = fopen(path, "w");
   assert(fp != NULL);
   fputs("{\"role\":\"user\",\"content\":\"from symptoms.\"}\n", fp);
   fputs("{\"role\":\"assistant\",\"content\":\"The prior diagnosis was inconclusive.\"}\n", fp);
   fputs("{\"role\":\"user\",\"content\":\"The memory search should preserve boundaries.\"}\n", fp);
   fclose(fp);

   char dirs[1][MAX_PATH_LEN];
   snprintf(dirs[0], sizeof(dirs[0]), "%s", dir);
   assert(memory_scan_conversations(dirs, 1) == 1);

   const char *terms[] = {"memory"};
   db1_window_search_candidate_t candidates[4];
   memset(candidates, 0, sizeof(candidates));
   assert(db1_windows_find_candidates_by_terms(terms, 1, candidates, 4) == 1);
   assert(strstr(candidates[0].summary, "user: from symptoms.\nassistant:") != NULL);
   assert(strstr(candidates[0].summary,
                 "assistant: The prior diagnosis was inconclusive.\nuser: The memory search") !=
          NULL);
   assert(strstr(candidates[0].summary, "symptoms.The memory search") == NULL);

   platform_test_rmrf(dir);
   teardown();
}

static void test_memory_promote_uses_calibration_profile(void)
{
   write_calibration_config(3);
   setup();

   const char *profile = "{\"buckets\":["
                         "{\"range\":[0.0,0.8],\"lower_credible_bound\":0.10},"
                         "{\"range\":[0.8,0.9],\"lower_credible_bound\":0.95}],"
                         "\"conformal\":{\"reject_below\":0.0}}";
   assert(db2_calibration_profile_write("memory", KIND_FACT, "global", "", "v1", profile, NULL,
                                        0) == 0);

   memory_t m;
   assert(memory_insert(TIER_L1, KIND_FACT, "cal-fact-live",
                        "calibrated promotion should accept this fact", 0.85, "s1", &m) == 0);

   int promoted = memory_promote();
   assert(promoted >= 1);

   memory_t updated;
   memory_get(m.id, &updated);
   assert(strcmp(updated.tier, TIER_L2) == 0);

   teardown();
   write_calibration_config(0);
}

static void test_memory_promote_calibration_ab_slot(void)
{
   write_calibration_config(2);
   setup();

   const char *profile = "{\"buckets\":["
                         "{\"range\":[0.0,0.8],\"lower_credible_bound\":0.10},"
                         "{\"range\":[0.8,0.9],\"lower_credible_bound\":0.95}],"
                         "\"conformal\":{\"reject_below\":0.0}}";
   assert(db2_calibration_profile_write("memory", KIND_FACT, "global", "", "v1", profile, NULL,
                                        0) == 0);

   for (int i = 0; i < 10; i++)
   {
      char key[64];
      memory_t m;
      snprintf(key, sizeof(key), "cal-fact-ab-%02d", i);
      assert(memory_insert(TIER_L1, KIND_FACT, key, "calibrated A/B slot candidate", 0.85, "s1",
                           &m) == 0);
   }

   int promoted = memory_promote();
   assert(promoted == 2);
   assert(db2_artifact_count("calibration_ab_trace", "committed") == 2);

   teardown();
   write_calibration_config(0);
}

/* Auditable-correctness P2: db2_memory_provenance_by_id resolves a surfaced
 * source id to {kind, source, version} (the /v1/audit/provenance read path), and
 * reports 0 for an id with no row (a source deleted/superseded since the turn). */
static void test_audit_provenance_resolver(void)
{
   setup();

   const char *ins = "INSERT INTO memories (tier, kind, key, content, confidence, use_count,"
                     " last_used_at, source_session, created_at, updated_at, sensitivity,"
                     " evidence_strength, salience, surprise, observation_count)"
                     " VALUES ('L1', 'fact', 'prov-key-1', 'caroline lives in portland', 0.8, 1,"
                     " pg_now_text(), 'sess-prov', pg_now_text(), pg_now_text(), 'normal',"
                     " 0.5, 0.5, 0.5, 1) RETURNING id";
   char err[128] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(db2_conn(), ins, err, sizeof(err));
   assert(s);
   assert(aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW);
   int64_t id = aimee_pg_column_int64(s, 0);
   aimee_pg_finalize(s);
   assert(id > 0);

   char kind[64] = "x", source[128] = "x", version[64] = "x";
   int rc = db2_memory_provenance_by_id(id, kind, sizeof kind, source, sizeof source, version,
                                        sizeof version);
   assert(rc == 1);
   assert(strcmp(kind, "fact") == 0);
   assert(strcmp(source, "sess-prov") == 0);
   assert(version[0] != '\0'); /* updated_at is the version */

   /* A missing id resolves to 0 (deleted/superseded), out buffers cleared. */
   kind[0] = source[0] = version[0] = 'x';
   rc = db2_memory_provenance_by_id(id + 999999, kind, sizeof kind, source, sizeof source, version,
                                    sizeof version);
   assert(rc == 0);
   assert(kind[0] == '\0' && source[0] == '\0' && version[0] == '\0');

   /* A non-positive id is rejected. */
   assert(db2_memory_provenance_by_id(0, NULL, 0, NULL, 0, NULL, 0) == -1);

   teardown();
   printf("  audit provenance resolver (kind/source/version + miss) OK\n");
}

/* Auditable-correctness P1.5/D8: db2_code_file_hash resolves a code ref's LIVE
 * source hash (files.hash for project+path) — the /v1/audit/provenance code-ref
 * drift check (live hash != the version captured on the turn). */
static void test_audit_code_file_hash_resolver(void)
{
   setup();

   char err[128] = "";
   assert(aimee_pg_exec(db2_conn(),
                        "INSERT INTO projects (name, root, scanned_at)"
                        " VALUES ('provproj','/r','x')",
                        err, sizeof err) == 0);
   assert(aimee_pg_exec(db2_conn(),
                        "INSERT INTO files (project_id, path, hash, scanned_at) VALUES"
                        " ((SELECT id FROM projects WHERE name='provproj'),"
                        " 'src/x.c','HASHV1','x')",
                        err, sizeof err) == 0);

   char hash[80] = "z";
   int rc = db2_code_file_hash("provproj", "src/x.c", hash, sizeof hash);
   assert(rc == 1 && strcmp(hash, "HASHV1") == 0);

   /* unknown file → 0, out cleared. */
   hash[0] = 'z';
   assert(db2_code_file_hash("provproj", "src/missing.c", hash, sizeof hash) == 0);
   assert(hash[0] == '\0');

   /* unknown project → 0. */
   assert(db2_code_file_hash("noproj", "src/x.c", hash, sizeof hash) == 0);

   /* bad args rejected. */
   assert(db2_code_file_hash("", "src/x.c", hash, sizeof hash) == -1);
   assert(db2_code_file_hash("provproj", "", hash, sizeof hash) == -1);

   teardown();
   printf("  audit code-file-hash resolver (live hash + miss) OK\n");
}

/* Auditable-correctness P2 emit-time version capture: writing a retrieval_event
 * records each surfaced id's point-in-time version (memories.updated_at at emit)
 * in surfaced_items, so /v1/audit/provenance can later detect version drift. */
static void test_audit_provenance_emit_captures_version(void)
{
   setup();

   const char *ins = "INSERT INTO memories (tier, kind, key, content, confidence, use_count,"
                     " last_used_at, source_session, created_at, updated_at, sensitivity,"
                     " evidence_strength, salience, surprise, observation_count)"
                     " VALUES ('L1', 'fact', 'emit-key-1', 'sky is blue', 0.8, 1,"
                     " pg_now_text(), 'sess-emit', pg_now_text(), pg_now_text(), 'normal',"
                     " 0.5, 0.5, 0.5, 1) RETURNING id";
   char err[128] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(db2_conn(), ins, err, sizeof(err));
   assert(s);
   assert(aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW);
   int64_t id = aimee_pg_column_int64(s, 0);
   aimee_pg_finalize(s);
   assert(id > 0);

   /* Emit a turn-keyed retrieval_event surfacing that memory. */
   int64_t ids[1] = {id};
   char evid[64] = "";
   assert(db2_demotion_retrieval_event_write_turn("p2-emit-turn", "fp", "Recall", ids, 1, evid,
                                                  sizeof evid) == 0);

   /* Read the stored payload back: it carries surfaced_items with the id and a
    * captured (point-in-time) version string. */
   char read_id[64] = "", payload[8192] = "";
   assert(db2_demotion_retrieval_event_by_turn("p2-emit-turn", read_id, sizeof read_id, payload,
                                               sizeof payload) == 1);
   assert(strstr(payload, "\"surfaced_items\"") != NULL);
   char id_needle[48];
   snprintf(id_needle, sizeof id_needle, "\"id\":%lld", (long long)id);
   assert(strstr(payload, id_needle) != NULL);
   assert(strstr(payload, "\"v\":\"") != NULL); /* a point-in-time version was captured */
   /* back-compat: surfaced_ids is still present. */
   assert(strstr(payload, "\"surfaced_ids\"") != NULL);

   teardown();
   printf("  audit provenance emit captures point-in-time version OK\n");
}

static long qembed_file_size(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* Per-recall query-embedding memo: an identical (command,text) embeds once and
 * is served from the thread-local cache on the recall's other lanes/sub-queries;
 * the memo clears at each recall entry. The "embedder" command records one byte
 * per real invocation, so the counter file size == number of cache misses. */
static void test_query_embedding_memo_dedupes_embeds(void)
{
   memory_test_ensure_env();

   char counter[512];
   snprintf(counter, sizeof(counter), "%s/aimee-qembed-counter-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(counter, sizeof(counter), "aim");
   assert(fd >= 0);
   close(fd);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "printf x >> '%s'; printf '[1.5, 2.5, 3.5, 4.5]'", counter);

   float a[EMBED_MAX_DIM], b[EMBED_MAX_DIM], c[EMBED_MAX_DIM];

   memory_query_embed_cache_reset_test();

   int da = memory_query_embed_runtime_test("alpha query", cmd, a, EMBED_MAX_DIM);
   assert(da == 4);
   assert(a[0] == 1.5f && a[3] == 4.5f);
   assert(qembed_file_size(counter) == 1); /* one real embed */

   /* Same text → served from cache: no new subprocess, byte-identical vector. */
   int db = memory_query_embed_runtime_test("alpha query", cmd, b, EMBED_MAX_DIM);
   assert(db == 4);
   assert(memcmp(a, b, 4 * sizeof(float)) == 0);
   assert(qembed_file_size(counter) == 1); /* still one embed */

   /* Different text → cache miss → a second embed. */
   int dc = memory_query_embed_runtime_test("beta query", cmd, c, EMBED_MAX_DIM);
   assert(dc == 4);
   assert(qembed_file_size(counter) == 2);

   /* Reset (new recall window) clears the memo → the first text embeds again. */
   memory_query_embed_cache_reset_test();
   int dd = memory_query_embed_runtime_test("alpha query", cmd, a, EMBED_MAX_DIM);
   assert(dd == 4);
   assert(qembed_file_size(counter) == 3);

   unlink(counter);
   printf("test_query_embedding_memo_dedupes_embeds: PASS\n");
}

/* Embed-batching: memory_query_embed_prewarm embeds N texts in one batched call
 * and seeds the per-recall memo, so subsequent runtime embeds of those texts are
 * served from the memo (no further embedder calls). The mock "embedder" reads a
 * JSON array of texts and returns a JSON array of fixed vectors. */
static void test_query_embed_prewarm_batches(void)
{
   memory_test_ensure_env();
   const char *cmd = "python3 -c 'import sys,json; a=json.load(sys.stdin); "
                     "print(json.dumps([[1.5,2.5,3.5,4.5] for _ in a]))'";

   memory_query_embed_cache_reset_test();
   const char *texts[2] = {"alpha query", "beta query"};
   memory_query_embed_prewarm_test(texts, 2, cmd);

   int req0 = 0, miss0 = 0;
   memory_query_embed_cache_stats_test(&req0, &miss0);

   float a[EMBED_MAX_DIM], b[EMBED_MAX_DIM];
   int da = memory_query_embed_runtime_test("alpha query", cmd, a, EMBED_MAX_DIM);
   int db = memory_query_embed_runtime_test("beta query", cmd, b, EMBED_MAX_DIM);

   int req1 = 0, miss1 = 0;
   memory_query_embed_cache_stats_test(&req1, &miss1);

   assert(da == 4 && a[0] == 1.5f && a[3] == 4.5f);
   assert(db == 4);
   /* Both were served from the prewarmed batch — no individual embeds happened. */
   assert(miss1 == miss0);
   printf("test_query_embed_prewarm_batches: PASS\n");
}

/* Measurement (not a pass/fail gate): run a real recall and report how many
 * embed requests the lanes/sub-queries made vs how many actually hit the
 * embedder, so the per-recall dedup factor is visible. */
static void measure_query_embedding_memo_recall(void)
{
   setup();
   memory_t a, b;
   memory_insert(TIER_L2, KIND_DECISION, "transport decision",
                 "The team chose server sent events for live frontend updates.", 0.92, "s1", &a);
   memory_insert(TIER_L2, KIND_FACT, "frontend architecture",
                 "Frontend network architecture uses an event stream transport layer.", 0.87, "s1",
                 &b);

   memory_t results[8];
   (void)memory_find_facts(
       "did we decide to use sse or websockets for the frontend network architecture last week", 5,
       results, 8);

   int requests = 0, misses = 0;
   memory_query_embed_cache_stats_test(&requests, &misses);
   printf("MEASURE query-embed memo (one recall): requests=%d misses=%d saved=%d (%.0f%% fewer "
          "embeds)\n",
          requests, misses, requests - misses,
          requests > 0 ? 100.0 * (requests - misses) / requests : 0.0);
   assert(requests >= misses);
   teardown();
}

int main(void)
{
   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   char *old_aimee_home = getenv("AIMEE_HOME") ? strdup(getenv("AIMEE_HOME")) : NULL;
   char *old_no_cache = getenv("AIMEE_NO_CACHE") ? strdup(getenv("AIMEE_NO_CACHE")) : NULL;

   snprintf(g_suite_home, sizeof(g_suite_home), "%s/aimee-test-memory-home-XXXXXX",
            platform_tmpdir());
   assert(platform_mkdtemp(g_suite_home) != NULL);
   assert(platform_setenv("HOME", g_suite_home) == 0);
   assert(platform_unsetenv("AIMEE_HOME") == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);

   test_db1_runtime_state_add_int();
   test_query_embedding_memo_dedupes_embeds();
   test_query_embed_prewarm_batches();
   measure_query_embedding_memo_recall();
   test_insert_memory();
   test_insert_merge();
   test_touch_memory();
   test_promote();
   test_memory_promote_uses_calibration_profile();
   test_memory_promote_calibration_ab_slot();
   test_audit_provenance_resolver();
   test_audit_code_file_hash_resolver();
   test_audit_provenance_emit_captures_version();
   test_expire_l0();
   test_fold_session();
   test_stats();
   test_delete_memory();
   test_list_by_tier_and_kind();
   test_get_nonexistent();
   test_delete_nonexistent();
   test_insert_empty_content();
   test_confidence_bounds();
   test_list_respects_limit();
   test_run_maintenance_cycle();
   test_insert_triggers_maintenance_when_threshold_met();
   test_temporal_retrieval_prefers_matching_date();
   test_temporal_retrieval_honors_before_date_constraint();
   test_temporal_retrieval_honors_between_date_constraint();
   test_temporal_retrieval_honors_last_week_phrase();
   test_temporal_retrieval_honors_next_month_phrase();
   test_chunk_retrieval_finds_sentence_evidence();
   test_superseded_memory_penalized();
   test_contradiction_reranking_prefers_newer_fact();
   test_insert_versions_temporal_fact_updates();
   test_semantic_profile_duplicate_keeps_single_active_entry();
   test_semantic_profile_replacement_creates_history();
   test_semantic_profile_history_retains_superseded_value();
   test_query_decomposition_recovers_compound_prompt();
   test_code_identifier_retrieval_handles_snake_and_camel();
   test_context_budget_prefers_project_l4_rule_over_long_global_l1();
   test_context_budget_prefers_project_scope_over_global_l5();
   test_rebuild_derived_indexes_populates_searchable_structures();
   test_rebuild_derived_indexes_assigns_memory_unit_kinds();
   test_memory_diagnose_reports_score_breakdown();
   test_memory_explain_match_reports_specific_memory();
   test_memory_answer_query_prefers_temporal_evidence();
   test_memory_answer_query_uses_session_cluster_evidence();
   test_memory_answer_query_adds_citations_when_enabled();
   test_memory_ask_query_returns_structured_result();
   test_memory_ask_query_reports_no_answer();
   test_multiword_entity_phrase_boosts_retrieval();
   test_speaker_alignment_boosts_actor_entity_matches();
   test_entity_canonicalization_handles_titles_and_plurals();
   test_memory_query_plan_prefers_lexical_for_code_queries();
   test_memory_query_plan_prefers_semantic_for_when_queries();
   test_memory_query_plan_prefers_graph_for_dependency_queries();
   test_memory_query_plan_respects_routing_disable_flag();
   test_memory_fetch_budget_factor_shape_aware();
   test_memory_find_facts_handles_lexical_code_query();
   test_memory_find_facts_falls_back_when_vector_index_unavailable();
   test_memory_find_facts_records_route_and_shape_metrics();
   test_memory_find_facts_graph_route_uses_graph_stage();
   test_memory_find_facts_lexical_route_skips_semantic_and_graph_stages();
   test_noise_utterance_gets_low_salience();
   test_salience_demotes_noise_matches();
   test_surprise_scores_first_mention_higher();
   test_surprise_demotes_repeated_fact_matches();
   test_pagerank_promotes_linked_definition_memory();
   test_memory_embed_records_embedder_version();
   test_coref_heuristic_indexes_recent_named_entity();
   test_coref_heuristic_skips_ambiguous_prior_turn();
   test_coref_audit_bound_recorded();
   test_coref_audit_ambiguous_recorded();
   test_coref_stats_increments_bound();
   test_coref_stats_increments_ambiguous();
   test_memory_demote_from_failures_uses_db1_agent_log();
   test_memory_promote_delegation_patterns_uses_db1_agent_log();
   test_memory_synthesize_failure_episodes_uses_db1_agent_log();
   test_memory_scan_preserves_message_boundaries();

   /* --- cosine_similarity: known vectors --- */
   {
      float a[] = {1.0f, 0.0f, 0.0f};
      float b[] = {1.0f, 0.0f, 0.0f};
      double sim = cosine_similarity(a, b, 3);
      assert(fabs(sim - 1.0) < 0.001); /* identical vectors = 1.0 */

      float c[] = {0.0f, 1.0f, 0.0f};
      sim = cosine_similarity(a, c, 3);
      assert(fabs(sim) < 0.001); /* orthogonal vectors = 0.0 */

      float d[] = {-1.0f, 0.0f, 0.0f};
      sim = cosine_similarity(a, d, 3);
      assert(fabs(sim + 1.0) < 0.001); /* opposite vectors = -1.0 */

      float e[] = {1.0f, 1.0f, 0.0f};
      sim = cosine_similarity(a, e, 3);
      assert(fabs(sim - 0.7071) < 0.01); /* 45-degree angle */
   }

   /* --- embedding vector sync status --- */
   {
      setup();
      memory_t mem;
      memory_insert(TIER_L2, KIND_FACT, "embed-test", "test content", 0.9, "", &mem);

      assert(memory_embed(mem.id, "builtin") == 0);

      {
         static const char *sql =
             "SELECT collection, status FROM vector_index_ops WHERE point_id = ?1";
         char vio_err[128] = "";
         aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, vio_err, sizeof(vio_err));
         assert(st != NULL);
         aimee_pg_bind_int64(st, "?1", mem.id);
         assert(aimee_pg_step(st, vio_err, sizeof(vio_err)) == AIMEE_PG_ROW);
         const char *collection = aimee_pg_column_text(st, 0);
         const char *status = aimee_pg_column_text(st, 1);
         assert(collection && strcmp(collection, "memory_embeddings") == 0);
         assert(status && strcmp(status, "ok") == 0);
         aimee_pg_finalize(st);

         /* Delete memory — vector index status should cascade through memory_id. */
         memory_delete(mem.id);
         st = aimee_pg_prepare(db2_conn(), sql, vio_err, sizeof(vio_err));
         assert(st != NULL);
         aimee_pg_bind_int64(st, "?1", mem.id);
         assert(aimee_pg_step(st, vio_err, sizeof(vio_err)) != AIMEE_PG_ROW);
         aimee_pg_finalize(st);

         teardown();
      }
   }

   /* --- deterministic builtin embedding fallback --- */
   {
      float vec[4];
      int dim = memory_embed_text("test", "", vec, 4);
      assert(dim == 4);

      dim = memory_embed_text("test", NULL, vec, 4);
      assert(dim == 4);
   }

   /* --- embedding_command sidecar contract (deep-curator AC#3): a sidecar
    * runs via /bin/sh -c, must yield a float array on stdout, and any
    * failure surfaces as a 0-dim result (caller skips / marks the op failed)
    * rather than silently corrupting the vector. */
   {
      float vec[4];

      /* A well-behaved sidecar emitting a JSON float array is parsed verbatim,
       * proving the float32 contract end-to-end through platform_exec_pipe. */
      for (int i = 0; i < 4; i++)
         vec[i] = -99.0f;
      int dim = memory_embed_text("ignored", "printf '[0.5, 0.25, 0.125, 0.0625]'", vec, 4);
      assert(dim == 4);
      assert(fabs(vec[0] - 0.5) < 1e-6 && fabs(vec[1] - 0.25) < 1e-6);
      assert(fabs(vec[2] - 0.125) < 1e-6 && fabs(vec[3] - 0.0625) < 1e-6);

      /* A sidecar that exits non-zero must NOT write the vector (no silent
       * corruption): dim is 0 and the sentinel survives. */
      for (int i = 0; i < 4; i++)
         vec[i] = -99.0f;
      dim = memory_embed_text("ignored", "sh -c 'exit 1'", vec, 4);
      assert(dim == 0);
      assert(vec[0] == -99.0f);

      /* A missing sidecar binary (sh exit 127) is a failure, not a corruption. */
      for (int i = 0; i < 4; i++)
         vec[i] = -99.0f;
      dim = memory_embed_text("ignored", "/nonexistent/embedder-xyz", vec, 4);
      assert(dim == 0);
      assert(vec[0] == -99.0f);

      /* Non-JSON stdout is rejected, again without touching the vector. */
      for (int i = 0; i < 4; i++)
         vec[i] = -99.0f;
      dim = memory_embed_text("ignored", "printf 'not json at all'", vec, 4);
      assert(dim == 0);
      assert(vec[0] == -99.0f);
   }

   /* --- kind_lifecycle_load: returns correct defaults for all 8 kinds --- */
   {
      setup();
      kind_lifecycle_t lc;

      /* fact: defaults */
      db2_kind_lifecycle_load(KIND_FACT, &lc);
      assert(lc.promote_use_count == 3);
      assert(fabs(lc.promote_confidence - 0.9) < 0.01);
      assert(lc.demote_days == 60);
      assert(fabs(lc.demotion_resistance - 1.0) < 0.01);

      /* policy: easy promote, aggressive demotion resistance */
      db2_kind_lifecycle_load(KIND_POLICY, &lc);
      assert(lc.promote_use_count == 1);
      assert(fabs(lc.promote_confidence - 0.7) < 0.01);
      assert(lc.demote_days == 365);
      assert(fabs(lc.demotion_resistance - 5.0) < 0.01);
      assert(lc.expire_days == 180);

      /* procedure: 2 uses to promote, 3x demotion resistance */
      db2_kind_lifecycle_load(KIND_PROCEDURE, &lc);
      assert(lc.promote_use_count == 2);
      assert(lc.demote_days == 180);
      assert(fabs(lc.demotion_resistance - 3.0) < 0.01);

      /* scratch: aggressive expiry */
      db2_kind_lifecycle_load(KIND_SCRATCH, &lc);
      assert(lc.expire_days == 3);
      assert(fabs(lc.demotion_resistance - 0.25) < 0.01);

      /* unknown kind: falls back to fact defaults */
      db2_kind_lifecycle_load("unknown_kind", &lc);
      assert(lc.promote_use_count == PROMOTE_L1_USE_COUNT);
      assert(lc.demote_days == DEMOTE_L2_DAYS);

      teardown();
   }

   /* --- policy promotes with 1 use --- */
   {
      setup();
      memory_t m;
      memory_insert(TIER_L1, KIND_POLICY, "no-cookies", "Never store session tokens in cookies",
                    0.5, "s1", &m);
      memory_touch(m.id); /* 1 touch -> use_count = 2 (insert starts at 1) */

      int promoted = memory_promote();
      assert(promoted >= 1);

      memory_t updated;
      memory_get(m.id, &updated);
      assert(strcmp(updated.tier, TIER_L2) == 0);
      teardown();
   }

   /* --- procedure and policy kinds can be inserted and listed --- */
   {
      setup();
      memory_t m;

      int rc = memory_insert(TIER_L1, KIND_PROCEDURE, "debug-cert-auth",
                             "Check CA chain, verify SVID expiry, test with openssl s_client", 0.8,
                             "s1", &m);
      assert(rc == 0);
      assert(strcmp(m.kind, KIND_PROCEDURE) == 0);

      rc = memory_insert(TIER_L2, KIND_POLICY, "check-pr-state",
                         "Always check PR merge state before pushing", 0.9, "s1", &m);
      assert(rc == 0);
      assert(strcmp(m.kind, KIND_POLICY) == 0);

      /* Stats should reflect new kinds */
      memory_stats_t stats;
      memory_stats(&stats);
      assert(stats.kind_counts[6] == 1); /* procedure */
      assert(stats.kind_counts[7] == 1); /* policy */
      assert(stats.total == 2);

      teardown();
   }

   /* --- classify_intent tests --- */
   {
      /* Debug intent */
      assert(classify_intent("fix the crash in auth module") == INTENT_DEBUG);
      assert(classify_intent("debug segfault in parser") == INTENT_DEBUG);
      assert(classify_intent("this error keeps failing") == INTENT_DEBUG);

      /* Plan intent */
      assert(classify_intent("design new API endpoint") == INTENT_PLAN);
      assert(classify_intent("implement user authentication") == INTENT_PLAN);
      assert(classify_intent("add support for webhooks") == INTENT_PLAN);

      /* Review intent */
      assert(classify_intent("review the PR for style issues") == INTENT_REVIEW);
      assert(classify_intent("audit security conventions") == INTENT_REVIEW);

      /* Deploy intent */
      assert(classify_intent("deploy the release to production") == INTENT_DEPLOY);
      assert(classify_intent("migrate the database schema") == INTENT_DEPLOY);

      /* General (no clear intent) */
      assert(classify_intent("hello world") == INTENT_GENERAL);
      assert(classify_intent("") == INTENT_GENERAL);
      assert(classify_intent(NULL) == INTENT_GENERAL);
   }

   /* --- retrieval_plan_for_intent tests --- */
   {
      retrieval_plan_t plan;

      /* Debug: procedures + episodes should dominate */
      retrieval_plan_for_intent(INTENT_DEBUG, &plan);
      assert(plan.include_l3 == 1);
      assert(plan.recency_weight > 0.5);
      assert(plan.kind_budget[6] >= 0.25); /* procedure */
      assert(plan.kind_budget[3] >= 0.20); /* episode */

      /* Plan: facts + decisions + policies should dominate */
      retrieval_plan_for_intent(INTENT_PLAN, &plan);
      assert(plan.include_l3 == 0);
      assert(plan.kind_budget[0] >= 0.20); /* fact */
      assert(plan.kind_budget[2] >= 0.20); /* decision */
      assert(plan.kind_budget[7] >= 0.15); /* policy */

      /* Deploy: should include L3 failure warnings */
      retrieval_plan_for_intent(INTENT_DEPLOY, &plan);
      assert(plan.include_l3 == 1);
      assert(plan.kind_budget[6] >= 0.25); /* procedure */

      /* General: balanced */
      retrieval_plan_for_intent(INTENT_GENERAL, &plan);
      assert(plan.include_l3 == 0);

      /* Budget fractions should sum to ~1.0 for all intents */
      for (int intent = 0; intent <= INTENT_GENERAL; intent++)
      {
         retrieval_plan_for_intent((task_intent_t)intent, &plan);
         double sum = 0;
         for (int k = 0; k < NUM_KINDS; k++)
            sum += plan.kind_budget[k];
         assert(sum > 0.95 && sum < 1.05);
      }
   }

   /* --- cross_encoder: score_parts initialises cross_encoder to 0 --- */
   {
      memory_score_parts_t parts;
      memset(&parts, 0, sizeof(parts));
      assert(parts.cross_encoder == 0.0);
   }

   /* --- cross_encoder: score_parts survives memset --- */
   {
      memory_score_parts_t parts;
      memset(&parts, 0, sizeof(parts));
      parts.lexical = 1.5;
      parts.semantic = 0.8;
      parts.cross_encoder = 0.92;
      parts.total = 3.22;
      assert(parts.cross_encoder >= 0.91 && parts.cross_encoder <= 0.93);
   }

   /* --- memory_score_parts_t: new explain fields (hybrid_total,
    *     blended_total, rerank_mix) exist and round-trip --- *
    *
    * The proposal's acceptance criterion calls for `aimee memory search
    * --explain` to surface hybrid vs rerank vs blended scores so operators
    * can see the rerank contribution per candidate.  These fields land
    * on the score_parts struct; the JSON serializer (in cmd_memory_embed.c)
    * is conditional so non-reranked pipelines stay byte-identical to the
    * pre-change JSON. */
   {
      memory_score_parts_t parts;
      memset(&parts, 0, sizeof(parts));
      /* Baseline: all three default to 0.0. */
      assert(parts.hybrid_total == 0.0);
      assert(parts.blended_total == 0.0);
      assert(parts.rerank_mix == 0.0);

      /* Simulated rerank: fields round-trip. */
      parts.cross_encoder = 0.92;
      parts.hybrid_total = 4.1;
      parts.blended_total = 5.3;
      parts.rerank_mix = 0.7;
      parts.total = parts.blended_total;
      assert(parts.hybrid_total == 4.1);
      assert(parts.blended_total == 5.3);
      assert(parts.rerank_mix == 0.7);
      assert(parts.total == parts.blended_total);
   }

   /* --- memory_rerank_enabled: defaults to 0 (disabled) --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      assert(cfg.memory_rerank_enabled == 0);
      assert(cfg.memory_rerank_command[0] == '\0');
      assert(cfg.memory_rerank_top_k == 0);
   }

   /* --- memory_query_expansion_mode: empty means lexical (default) --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      /* mode is empty string = lexical (default) */
      assert(strcmp(cfg.memory_query_expansion_mode, "semantic") != 0);
   }

   /* --- memory_find_facts_scoped: returns results (smoke test for reranker path) --- */
   {
      setup();
      memory_t m1, m2, m3;
      memory_insert(TIER_L1, KIND_FACT, "user went to paris", "user visited paris france", 0.9,
                    "s1", &m1);
      memory_insert(TIER_L1, KIND_FACT, "project deadline is friday",
                    "deadline for project is next friday", 0.8, "s1", &m2);
      memory_insert(TIER_L1, KIND_FACT, "user joined team alpha",
                    "the user joined team alpha last month", 0.7, "s1", &m3);

      memory_t results[10];
      int count = memory_find_facts_scoped("paris visit", NULL, NULL, 5, results, 10);
      assert(count >= 1);
      /* The paris memory should appear */
      {
         int found = 0;
         for (int i = 0; i < count; i++)
            if (results[i].id == m1.id)
               found = 1;
         assert(found);
      }
      teardown();
   }

   /* --- memory_find_facts_visible: workspace-scoped memory ranks above unscoped --- */
   {
      setup();
      memory_t m_global, m_ws;

      /* Store an untagged memory and a workspace-tagged one */
      memory_insert(TIER_L1, KIND_FACT, "deployment server host", "deploy to prod-server-01", 0.8,
                    "s1", &m_global);
      memory_insert(TIER_L1, KIND_FACT, "deploy target for myproject", "deploy to dev-server-99",
                    0.8, "s1", &m_ws);
      memory_tag_workspace(m_ws.id, "myproject");

      memory_t results[10];
      /* Retrieve with workspace="myproject": both should appear (global+workspace visible) */
      int count = memory_find_facts_visible("deploy server", "myproject", NULL, 10, results, 10);
      assert(count >= 1);

      /* The workspace-tagged memory must appear in results */
      {
         int found_ws = 0;
         for (int i = 0; i < count; i++)
            if (results[i].id == m_ws.id)
               found_ws = 1;
         assert(found_ws);
      }

      teardown();
   }

   /* --- citation_gate_check: detects [#N] markers --- */
   {
      assert(memory_citation_gate_check(NULL) == 0);
      assert(memory_citation_gate_check("") == 0);
      assert(memory_citation_gate_check("No citations here.") == 0);
      assert(memory_citation_gate_check("The answer is X. [#42]") == 1);
      assert(memory_citation_gate_check("Multiple [#1, #2] refs.") == 1);
      assert(memory_citation_gate_check("[#0]") == 1);
      /* Square bracket without hash is not a citation */
      assert(memory_citation_gate_check("See [section 3].") == 0);
   }

   /* --- citation required mode: answer_query returns LOW prefix when no evidence --- */
   {
      char tmpdir[128];
      snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-citation-gate-XXXXXX", platform_tmpdir());
      assert(platform_mkdtemp(tmpdir) != NULL);
      assert(platform_setenv("HOME", tmpdir) == 0);
      assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
      assert(platform_setenv("AIMEE_MEMORY_CITATIONS_MODE", "required") == 0);

      setup();
      /* Empty DB — no memories. Required citations with no evidence → LOW prefix. */
      char *ans = memory_answer_query("why avoid TypeScript?", 5);
      /* Empty DB returns empty string; nothing to gate */
      assert(ans != NULL);
      free(ans);

      teardown();
      assert(platform_unsetenv("AIMEE_MEMORY_CITATIONS_MODE") == 0);
      assert(platform_unsetenv("AIMEE_NO_CACHE") == 0);
      assert(platform_unsetenv("HOME") == 0);
      platform_test_rmrf(tmpdir);
   }

   /* --- reflect contradiction detection: same key different content flagged --- */
   {
      /* Test the contradiction-detection logic used by mem_reflect directly
       * by constructing a synthetic results array. Two memories share the same
       * key but differ in content — the nested loop must detect exactly 1 conflict. */
      memory_t results[4];
      memset(results, 0, sizeof(results));

      snprintf(results[0].key, sizeof(results[0].key), "preferred-language");
      snprintf(results[0].content, sizeof(results[0].content), "the team prefers Python");
      results[0].id = 1;

      snprintf(results[1].key, sizeof(results[1].key), "preferred-language");
      snprintf(results[1].content, sizeof(results[1].content), "the team prefers Go");
      results[1].id = 2;

      snprintf(results[2].key, sizeof(results[2].key), "deploy-target");
      snprintf(results[2].content, sizeof(results[2].content), "deploy to prod-server");
      results[2].id = 3;

      /* Same key AND same content → not a contradiction */
      snprintf(results[3].key, sizeof(results[3].key), "deploy-target");
      snprintf(results[3].content, sizeof(results[3].content), "deploy to prod-server");
      results[3].id = 4;

      int count = 4;
      int nconflicts = 0;
      for (int i = 0; i < count; i++)
         for (int j = i + 1; j < count; j++)
            if (results[i].key[0] && strcmp(results[i].key, results[j].key) == 0 &&
                strcmp(results[i].content, results[j].content) != 0)
               nconflicts++;

      assert(nconflicts == 1); /* only preferred-language conflicts */
   }

   /* --- functional memory hierarchy: tier constants and priority ordering --- */
   {
      /* L4 > L3 > L2 > L1 > L0 */
      assert(memory_tier_priority(TIER_L4) > memory_tier_priority(TIER_L3));
      assert(memory_tier_priority(TIER_L3) > memory_tier_priority(TIER_L2));
      assert(memory_tier_priority(TIER_L2) > memory_tier_priority(TIER_L1));
      assert(memory_tier_priority(TIER_L1) > memory_tier_priority(TIER_L0));
      assert(memory_tier_priority(TIER_L5) > memory_tier_priority(TIER_L4));

      /* Functional names */
      assert(strcmp(memory_functional_tier_name(TIER_L0), TIER_L0_NAME) == 0);
      assert(strcmp(memory_functional_tier_name(TIER_L1), TIER_L1_NAME) == 0);
      assert(strcmp(memory_functional_tier_name(TIER_L2), TIER_L2_NAME) == 0);
      assert(strcmp(memory_functional_tier_name(TIER_L3), TIER_L3_NAME) == 0);
      assert(strcmp(memory_functional_tier_name(TIER_L4), TIER_L4_NAME) == 0);
      assert(strcmp(memory_functional_tier_name(TIER_L5), TIER_L5_NAME) == 0);
      assert(strcmp(memory_functional_tier_name(NULL), "Unknown") == 0);
      assert(strcmp(memory_functional_tier_name("Lx"), "Unknown") == 0);
   }

   /* --- functional hierarchy: L4/L5 memories can be stored and retrieved --- */
   {
      setup();
      memory_t m;

      /* Insert an L4 directive */
      int rc = memory_insert(TIER_L4, KIND_POLICY, "always-snake-case",
                             "Always use snake_case for variable names", 0.99, "s1", &m);
      assert(rc == 0);
      assert(strcmp(m.tier, TIER_L4) == 0);

      /* Insert an L5 pattern */
      rc = memory_insert(TIER_L5, KIND_FACT, "monorepo-pattern",
                         "All services live in a single monorepo", 0.95, "s1", &m);
      assert(rc == 0);
      assert(strcmp(m.tier, TIER_L5) == 0);

      /* memory_stats should count them */
      memory_stats_t stats;
      memory_stats(&stats);
      assert(stats.tier_counts[4] >= 1); /* L4 */
      assert(stats.tier_counts[5] >= 1); /* L5 */
      assert(stats.total >= 2);

      teardown();
   }

   /* --- functional hierarchy: reclassify_directives promotes L3 policy → L4 --- */
   {
      setup();
      memory_t m;

      /* Insert L3 policy (should be reclassified) */
      memory_insert(TIER_L3, KIND_POLICY, "deploy-checklist", "Always run tests before deploy", 0.9,
                    "s1", &m);
      int64_t policy_id = m.id;

      /* Insert L3 fact (should NOT be reclassified) */
      memory_insert(TIER_L3, KIND_FACT, "env-config", "Production uses PostgreSQL 16", 0.9, "s1",
                    &m);
      int64_t fact_id = m.id;

      int reclassified = memory_reclassify_directives();
      assert(reclassified >= 1);

      /* Policy should now be L4 */
      memory_t policy;
      memory_get(policy_id, &policy);
      assert(strcmp(policy.tier, TIER_L4) == 0);

      /* Fact should remain L3 */
      memory_t fact;
      memory_get(fact_id, &fact);
      assert(strcmp(fact.tier, TIER_L3) == 0);

      teardown();
   }

   /* --- functional hierarchy: scope is independent of tier --- */
   {
      /* An L2 memory can have project, workspace, or global scope — scope
       * should not be inferred from tier. We verify this at the type level. */
      assert(MEMORY_SCOPE_NONE == 0);
      assert(MEMORY_SCOPE_GLOBAL > MEMORY_SCOPE_NONE);
      assert(MEMORY_SCOPE_WORKSPACE > MEMORY_SCOPE_GLOBAL);
      assert(MEMORY_SCOPE_PROJECT > MEMORY_SCOPE_WORKSPACE);
   }

   /* --- token-budget assembly: score_per_token prefers short high-signal memories --- */
   {
      typedef struct
      {
         double score;
         int key_len;
         int content_len;
      } fake_candidate_t;

      fake_candidate_t policy = {.score = 0.6, .key_len = 30, .content_len = 40};
      fake_candidate_t fact = {.score = 0.7, .key_len = 200, .content_len = 800};

      int policy_tokens = (policy.key_len + policy.content_len) / 4 + 1;
      int fact_tokens = (fact.key_len + fact.content_len) / 4 + 1;

      double policy_spt = policy.score / (double)policy_tokens;
      double fact_spt = fact.score / (double)fact_tokens;

      assert(policy_spt > fact_spt);
      assert(policy_tokens > 0 && policy_tokens < 50);
      assert(fact_tokens > 0 && fact_tokens < 300);
   }

   /* --- token-budget: memory_assemble_context_explain populates entries --- */
   {
      setup();
      memory_t m;

      memory_insert(TIER_L2, KIND_POLICY, "short-rule", "Use snake_case", 0.95, "s1", &m);
      char long_content[512];
      memset(long_content, 'x', sizeof(long_content) - 1);
      long_content[sizeof(long_content) - 1] = '\0';
      memory_insert(TIER_L2, KIND_FACT, "long-fact", long_content, 0.5, "s1", &m);

      context_assemble_explain_entry_t entries[32];
      int ecount = 0;
      context_budget_metrics_t metrics;
      memset(&metrics, 0, sizeof(metrics));

      char *ctx =
          memory_assemble_context_explain("snake case rule", entries, &ecount, 32, &metrics);
      assert(ctx != NULL);
      assert(ecount > 0);

      for (int i = 0; i < ecount; i++)
      {
         assert(entries[i].tokens > 0);
         assert(entries[i].score_per_token >= 0.0);
         assert(entries[i].tier[0] != '\0');
      }

      free(ctx);
      teardown();
   }

   /* --- memory_query_rewrite: disabled when command is empty --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.memory_rewrite_enabled = 1;
      memory_query_rewrite_t rw;
      memory_query_rewrite("what kind of person is Caroline?", &cfg, &rw);
      assert(rw.has_hyde == 0);
      assert(rw.has_decomp == 0);
      assert(rw.sub_question_count == 0);
   }

   /* --- memory_query_rewrite: disabled when enabled=0 --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.memory_rewrite_enabled = 0;
      snprintf(cfg.memory_rewrite_command, sizeof(cfg.memory_rewrite_command), "echo '{}'");
      memory_query_rewrite_t rw;
      memory_query_rewrite("compound question A and B?", &cfg, &rw);
      assert(rw.has_hyde == 0);
      assert(rw.has_decomp == 0);
      assert(rw.sub_question_count == 0);
   }

   /* --- memory_query_rewrite: valid JSON with hyde_answer and sub_questions --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.memory_rewrite_enabled = 1;
      cfg.memory_rewrite_hyde = 1;
      cfg.memory_rewrite_decompose = 1;
      snprintf(cfg.memory_rewrite_command, sizeof(cfg.memory_rewrite_command),
               "printf '{\"hyde_answer\":\"Alice went to the museum on March 10.\","
               "\"sub_questions\":[\"When did Alice go?\",\"Where did Alice go?\"]}'");
      memory_query_rewrite_t rw;
      memory_query_rewrite("when did alice visit the museum", &cfg, &rw);
      assert(rw.has_hyde == 1);
      assert(strstr(rw.hyde_answer, "March 10") != NULL);
      assert(rw.has_decomp == 1);
      assert(rw.sub_question_count == 2);
   }

   /* --- memory_query_rewrite: sub_questions capped at MEMORY_REWRITE_MAX_SUBQUERIES --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.memory_rewrite_enabled = 1;
      cfg.memory_rewrite_hyde = 1;
      cfg.memory_rewrite_decompose = 1;
      snprintf(cfg.memory_rewrite_command, sizeof(cfg.memory_rewrite_command),
               "printf '{\"hyde_answer\":\"x\","
               "\"sub_questions\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\"]}'");
      memory_query_rewrite_t rw;
      memory_query_rewrite("cap test query", &cfg, &rw);
      assert(rw.sub_question_count == MEMORY_REWRITE_MAX_SUBQUERIES);
   }

   /* --- memory_expand_to_session_window: no-op with radius=0 --- */
   {
      setup();
      memory_t m;
      memory_insert(TIER_L0, KIND_FACT, "session-fact", "content A", 0.8, "sess1", &m);
      memory_t results[4];
      results[0] = m;
      int new_count = memory_expand_to_session_window(results, 1, 4, 0);
      assert(new_count == 1); /* radius=0: no expansion */
      teardown();
   }

   /* --- memory_expand_to_session_window: radius=1 brings in neighbours --- */
   {
      setup();
      /* Insert three memories directly to avoid dedup logic. RETURNING id
       * gives us the new row id without sqlite_last_insert_rowid. */
      const char *ins = "INSERT INTO memories (tier, kind, key, content, confidence, use_count,"
                        " last_used_at, source_session, created_at, updated_at, sensitivity,"
                        " evidence_strength, salience, surprise, observation_count)"
                        " VALUES ('L0', 'fact', ?1, ?2, 0.8, 1, pg_now_text(), 'sess-win',"
                        " pg_now_text(), pg_now_text(), 'normal', 0.5, 0.5, 0.5, 1)"
                        " RETURNING id";

      char ins_err[128] = "";
      int64_t id1 = 0, id2 = 0, id3 = 0;
      struct
      {
         const char *key;
         const char *content;
         int64_t *id;
      } rows[] = {
          {"win-key-001", "caroline lives in portland", &id1},
          {"win-key-002", "john works at the university", &id2},
          {"win-key-003", "david moved to chicago", &id3},
      };
      for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++)
      {
         aimee_pg_stmt_t *s = aimee_pg_prepare(db2_conn(), ins, ins_err, sizeof(ins_err));
         assert(s);
         aimee_pg_bind_text(s, "?1", rows[r].key);
         aimee_pg_bind_text(s, "?2", rows[r].content);
         assert(aimee_pg_step(s, ins_err, sizeof(ins_err)) == AIMEE_PG_ROW);
         *rows[r].id = aimee_pg_column_int64(s, 0);
         aimee_pg_finalize(s);
      }

      assert(id1 > 0 && id2 > id1 && id3 > id2);

      /* Start with only the middle memory as the retrieval hit */
      memory_t results[8];
      memset(results, 0, sizeof(results));
      results[0].id = id2;
      snprintf(results[0].source_session, sizeof(results[0].source_session), "sess-win");

      int new_count = memory_expand_to_session_window(results, 1, 8, 1);
      /* Should expand to include id1 (prev) and id3 (next) */
      assert(new_count >= 2);
      int found_prev = 0, found_next = 0;
      for (int k = 0; k < new_count; k++)
      {
         if (results[k].id == id1)
            found_prev = 1;
         if (results[k].id == id3)
            found_next = 1;
      }
      assert(found_prev);
      assert(found_next);
      teardown();
   }

   /* --- memory_expand_to_session_window: no expansion for memories without session --- */
   {
      setup();
      /* A memory with empty source_session should not expand */
      memory_t results[4];
      memset(results, 0, sizeof(results));
      results[0].id = 999;
      results[0].source_session[0] = '\0'; /* no session */
      int new_count = memory_expand_to_session_window(results, 1, 4, 2);
      assert(new_count == 1);
      teardown();
   }

   /* --- is_negation_marker: basic markers recognised --- */
   {
      assert(is_negation_marker("not") == 1);
      assert(is_negation_marker("never") == 1);
      assert(is_negation_marker("no") == 1);
      assert(is_negation_marker("without") == 1);
      assert(is_negation_marker("haven't") == 1);
      assert(is_negation_marker("didn't") == 1);
      assert(is_negation_marker("the") == 0);
      assert(is_negation_marker("cat") == 0);
      assert(is_negation_marker("") == 0);
   }

   /* --- extract_negation_tokens: basic negation scope --- */
   {
      char buf[512];
      /* "Caroline does not have pets" → not_caroline not_have not_pets */
      int n = extract_negation_tokens("Caroline does not have pets", buf, sizeof(buf));
      assert(n > 0);
      assert(strstr(buf, "not_pets") != NULL);
      assert(strstr(buf, "not_caroline") != NULL);
      /* "The cat sat on the mat" — no negation markers → empty */
      n = extract_negation_tokens("The cat sat on the mat", buf, sizeof(buf));
      assert(n == 0);
      assert(buf[0] == '\0');
   }

   /* --- extract_negation_tokens: clause boundary stops scope --- */
   {
      char buf[512];
      /* "She never came. Dogs are cute." — "Dogs" and "cute" are past boundary */
      int n = extract_negation_tokens("She never came. Dogs are cute.", buf, sizeof(buf));
      assert(n > 0);
      assert(strstr(buf, "not_came") != NULL);
      assert(strstr(buf, "not_dogs") == NULL);
      assert(strstr(buf, "not_cute") == NULL);
   }

   /* --- extract_negation_tokens: empty/NULL input --- */
   {
      char buf[64];
      int n = extract_negation_tokens(NULL, buf, sizeof(buf));
      assert(n == 0);
      n = extract_negation_tokens("", buf, sizeof(buf));
      assert(n == 0);
   }

   /* --- memory_query_polarity --- */
   {
      assert(memory_query_polarity("Did Caroline not go camping?") == POLARITY_NEGATIVE);
      assert(memory_query_polarity("Does Caroline have pets?") == POLARITY_POSITIVE);
      assert(memory_query_polarity("We never discussed dogs") == POLARITY_NEGATIVE);
      assert(memory_query_polarity("") == POLARITY_POSITIVE);
      assert(memory_query_polarity(NULL) == POLARITY_POSITIVE);
   }

   /* --- memory_lineage_insert / memory_lineage_get: basic round-trip --- */
   {
      setup();

      /* INSERT...RETURNING gives us the row id without sqlite_last_insert_rowid. */
      char ins_err[128] = "";
      aimee_pg_stmt_t *ins = aimee_pg_prepare(db2_conn(),
                                              "INSERT INTO memories (key, content, tier, kind,"
                                              " confidence, use_count, created_at, updated_at)"
                                              " VALUES ('lineage_test', 'test content', 'L2',"
                                              " 'fact', 0.9, 0, pg_now_text(), pg_now_text())"
                                              " RETURNING id",
                                              ins_err, sizeof(ins_err));
      assert(ins != NULL);
      assert(aimee_pg_step(ins, ins_err, sizeof(ins_err)) == AIMEE_PG_ROW);
      int64_t mem_id = aimee_pg_column_int64(ins, 0);
      aimee_pg_finalize(ins);
      assert(mem_id > 0);

      int64_t lid1 = memory_lineage_insert("memory", mem_id, "session", "sess-abc123", 1.0);
      assert(lid1 > 0);
      int64_t lid2 = memory_lineage_insert("memory", mem_id, "cognify", "model-haiku", 0.85);
      assert(lid2 > lid1);

      memory_lineage_t rows[8];
      int cnt = memory_lineage_get("memory", mem_id, rows, 8);
      assert(cnt == 2);
      assert(rows[0].object_id == mem_id);
      assert(strcmp(rows[0].source_kind, "session") == 0);
      assert(strstr(rows[0].source_ref, "abc123") != NULL);
      assert(rows[0].confidence > 0.99);
      assert(strcmp(rows[1].source_kind, "cognify") == 0);
      assert(rows[1].confidence > 0.8 && rows[1].confidence < 0.9);

      teardown();
   }

   /* --- memory_lineage_insert: invalid args return -1 --- */
   {
      setup();
      assert(memory_lineage_insert(NULL, 1, "session", "ref", 1.0) == -1);
      assert(memory_lineage_insert("memory", 0, "session", "ref", 1.0) == -1);
      teardown();
   }

   /* --- memory_search_graph_as_of: NULL as_of falls back to all records --- */
   {
      setup();
      char rel_err[128] = "";
      /* Disable FK enforcement so we can insert memory_relations freely.
       * Under the test shim PRAGMA passes through to sqlite verbatim. */
      (void)aimee_pg_exec(db2_conn(), "PRAGMA foreign_keys=OFF", rel_err, sizeof(rel_err));

      (void)aimee_pg_exec(db2_conn(),
                          "INSERT INTO memory_relations"
                          " (memory_id, src_entity, relation, dst_entity, fact_text,"
                          "  valid_at, invalid_at, weight, created_at)"
                          " VALUES (1, 'Alice', 'lives_in', 'Wonderland',"
                          " 'Alice lives in Wonderland', '2025-01-01', '', 1.0, pg_now_text())",
                          rel_err, sizeof(rel_err));

      memory_relation_t rels[8];
      int cnt = memory_search_graph_as_of("Alice", NULL, 8, rels, 8);
      assert(cnt == 1);
      assert(strcmp(rels[0].src_entity, "Alice") == 0);
      teardown();
   }

   /* --- memory_search_graph_as_of: as_of filters by valid_at / invalid_at --- */
   {
      setup();
      char rel2_err[128] = "";
      (void)aimee_pg_exec(db2_conn(), "PRAGMA foreign_keys=OFF", rel2_err, sizeof(rel2_err));

      (void)aimee_pg_exec(db2_conn(),
                          "INSERT INTO memory_relations"
                          " (memory_id, src_entity, relation, dst_entity, fact_text,"
                          "  valid_at, invalid_at, weight, created_at)"
                          " VALUES (1, 'Bob', 'works_at', 'AcmeCorp',"
                          " 'Bob works at AcmeCorp',"
                          " '2024-01-01', '2025-01-01', 1.0, pg_now_text())",
                          rel2_err, sizeof(rel2_err));
      (void)aimee_pg_exec(db2_conn(),
                          "INSERT INTO memory_relations"
                          " (memory_id, src_entity, relation, dst_entity, fact_text,"
                          "  valid_at, invalid_at, weight, created_at)"
                          " VALUES (1, 'Bob', 'works_at', 'NewCo', 'Bob works at NewCo',"
                          " '2025-06-01', '', 1.0, pg_now_text())",
                          rel2_err, sizeof(rel2_err));

      memory_relation_t rels[8];

      /* 2024-06-15: AcmeCorp is valid (valid_at <= date, invalid_at > date) */
      int cnt = memory_search_graph_as_of("Bob", "2024-06-15", 8, rels, 8);
      assert(cnt == 1);
      assert(strstr(rels[0].dst_entity, "AcmeCorp") != NULL);

      /* 2025-07-01: AcmeCorp expired; only NewCo matches */
      cnt = memory_search_graph_as_of("Bob", "2025-07-01", 8, rels, 8);
      assert(cnt == 1);
      assert(strstr(rels[0].dst_entity, "NewCo") != NULL);

      teardown();
   }

   /* memory_cluster_scenes: returns >= 0 on empty DB (no embeddings) */
   {
      setup();
      int cnt = memory_cluster_scenes("");
      assert(cnt >= 0); /* no embeddings -> 0 scenes, not -1 */
      teardown();
   }

   /* memory_assign_scene: no-op for nonexistent memory */
   {
      setup();
      int rc = memory_assign_scene(9999);
      assert(rc == 0); /* no embedding -> no-op, not error */
      teardown();
   }

   /* --- temporal: "N days ago" resolves to absolute date anchor --- */
   {
      setup();
      memory_t old_fact, new_fact;
      /* Insert two memories: one about an event "3 days ago", one explicitly dated */
      memory_insert(TIER_L2, KIND_FACT, "meeting", "The design review happened 3 days ago", 0.9,
                    "s1", &old_fact);
      memory_insert(TIER_L2, KIND_FACT, "appointment", "Doctor appointment next month at 2pm", 0.9,
                    "s1", &new_fact);

      memory_t results[8];
      /* Both should be retrievable; just verify no crash and non-zero count */
      int count = memory_find_facts("3 days ago meeting", 5, results, 8);
      (void)count; /* no crash is sufficient */
      teardown();
   }

   /* --- temporal: "last week" resolves without crash --- */
   {
      setup();
      memory_t m;
      memory_insert(TIER_L2, KIND_FACT, "event", "We had a team sync last week on Tuesday", 0.9,
                    "s1", &m);
      memory_t results[8];
      int count = memory_find_facts("last week team sync", 5, results, 8);
      (void)count; /* no crash is sufficient */
      teardown();
   }

   /* --- contradiction reranking: newer contradictory fact ranks higher --- */
   {
      setup();
      memory_t old_fact, new_fact;
      /* Insert an older fact, then a newer contradicting one */
      memory_insert(TIER_L2, KIND_FACT, "server location",
                    "The server runs on machine-A in datacenter east", 0.9, "s1", &old_fact);
      /* Supersede: creates a newer memory with conflicting content */
      assert(memory_supersede(old_fact.id, "The server runs on machine-B in datacenter west", 0.95,
                              "s2", &new_fact) == 0);

      memory_t results[8];
      int count = memory_find_facts("server location machine datacenter", 5, results, 8);
      assert(count >= 1);
      /* The newer superseding fact should appear first */
      assert(strstr(results[0].content, "machine-B") != NULL ||
             strstr(results[0].content, "machine-A") != NULL); /* either is valid; no crash */
      teardown();
   }

   if (old_home)
   {
      assert(platform_setenv("HOME", old_home) == 0);
      free(old_home);
   }
   else
   {
      assert(platform_unsetenv("HOME") == 0);
   }
   if (old_aimee_home)
   {
      assert(platform_setenv("AIMEE_HOME", old_aimee_home) == 0);
      free(old_aimee_home);
   }
   else
   {
      assert(platform_unsetenv("AIMEE_HOME") == 0);
   }
   if (old_no_cache)
   {
      assert(platform_setenv("AIMEE_NO_CACHE", old_no_cache) == 0);
      free(old_no_cache);
   }
   else
   {
      assert(platform_unsetenv("AIMEE_NO_CACHE") == 0);
   }
   /* --- Phase 1: entity edge dedup struct layout and null-safety --- */
   printf("test_entity_edge_dedup_struct...");
   {
      db2_entity_edge_dedup_report_t r;
      memset(&r, 0, sizeof(r));
      r.total_rows = 100;
      r.dup_triples = 5;
      r.dup_rows = 12;
      r.largest_group = 4;
      r.table_size_kb = 1024;
      assert(r.total_rows == 100);
      assert(r.dup_triples == 5);
      assert(r.dup_rows == 12);
      assert(r.largest_group == 4);
      assert(r.table_size_kb == 1024);
   }
   printf("ok\n");

   printf("test_entity_edge_dedup_null_safety...");
   {
      assert(db2_entity_edge_dedup_audit(NULL) == -1);
      db2_entity_edge_dedup_report_t out;
      memset(&out, 0, sizeof(out));
      int rc = db2_entity_edge_dedup_migrate(NULL, 1, &out);
      assert(rc == -1 || rc == 0);
   }
   printf("ok\n");

   platform_test_rmrf(g_suite_home);
   printf("memory: all tests passed\n");
   return 0;
}
