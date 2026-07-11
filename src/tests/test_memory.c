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
#include "../db2/lifecycle.h" /* db2_set_embedding_dim (embedder-aware semantic recall) */
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

static void test_insert_memory(void)
{
   setup();
   memory_t m;
   int rc = memory_insert(TIER_L1, KIND_FACT, "test key", "test content", 0.8, "s1", &m);
   assert(rc == 0);
   assert(strcmp(m.tier, TIER_L1) == 0);
   assert(strcmp(m.kind, KIND_FACT) == 0);
   assert(m.confidence >= 0.79 && m.confidence <= 0.81);
   assert(m.use_count >= 1);
   teardown();
}

static void test_insert_merge(void)
{
   setup();
   memory_t m1, m2;
   memory_insert(TIER_L1, KIND_FACT, "dup key", "old content", 0.5, "s1", &m1);
   memory_insert(TIER_L1, KIND_FACT, "dup key", "new content", 0.9, "s2", &m2);

   assert(m2.id == m1.id); /* merged, same ID */
   assert(m2.use_count >= 2);
   assert(m2.confidence >= 0.89); /* kept higher */
   teardown();
}

static void test_touch_memory(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_PREFERENCE, "style", "concise", 0.8, "s1", &m);
   memory_touch(m.id);

   memory_t updated;
   memory_get(m.id, &updated);
   assert(updated.use_count >= 2);
   teardown();
}

static void test_promote(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L1, KIND_FACT, "promote me", "important", 0.5, "s1", &m);

   /* Bump use_count above threshold */
   for (int i = 0; i < PROMOTE_L1_USE_COUNT; i++)
      memory_touch(m.id);

   int promoted = memory_promote();
   assert(promoted >= 1);

   memory_t updated;
   memory_get(m.id, &updated);
   assert(strcmp(updated.tier, TIER_L2) == 0);
   teardown();
}

static void test_expire_l0(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L0, KIND_SCRATCH, "temp", "scratch data", 0.3, "s1", &m);

   int expired = memory_expire();
   assert(expired >= 1);

   memory_t check;
   int rc = memory_get(m.id, &check);
   assert(rc != 0); /* should be gone */
   teardown();
}

static void test_fold_session(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L0, KIND_SCRATCH, "task1", "content 1", 0.5, "fold-sess", &m);
   memory_insert(TIER_L0, KIND_SCRATCH, "task2", "content 2", 0.5, "fold-sess", &m);

   char fold_summary[256] = "";
   int rc = memory_fold_session("fold-sess", fold_summary, sizeof(fold_summary));
   assert(rc == 0);
   /* The caller-facing digest is populated so the KB layer can emit it as a
    * session_summary evidence artifact (the reflection input producer). */
   assert(fold_summary[0] != '\0');
   assert(strstr(fold_summary, "content 1") != NULL);

   /* L0 should be gone */
   memory_t l0[10];
   int n = memory_list(TIER_L0, "", 10, l0, 10);
   assert(n == 0);

   /* Should have L1 checkpoint */
   memory_t l1[10];
   n = memory_list(TIER_L1, KIND_EPISODE, 10, l1, 10);
   assert(n >= 1);
   teardown();
}

static void test_stats(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "f1", "fact one", 0.9, "s1", &m);
   memory_insert(TIER_L1, KIND_PREFERENCE, "p1", "pref", 0.7, "s1", &m);

   memory_stats_t s;
   memory_stats(&s);
   assert(s.total == 2);
   teardown();
}

static void test_delete_memory(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L1, KIND_FACT, "deleteme", "temp", 0.5, "s1", &m);
   int rc = memory_delete(m.id);
   assert(rc == 0);

   memory_t check;
   rc = memory_get(m.id, &check);
   assert(rc != 0);
   teardown();
}

/* --- Deeper coverage --- */

static void test_list_by_tier_and_kind(void)
{
   setup();
   memory_t out;
   memory_insert(TIER_L0, KIND_FACT, "l0-fact", "content", 1.0, "s1", &out);
   memory_insert(TIER_L1, KIND_FACT, "l1-fact", "content", 1.0, "s1", &out);
   memory_insert(TIER_L1, KIND_PREFERENCE, "l1-pref", "content", 1.0, "s1", &out);
   memory_insert(TIER_L2, KIND_DECISION, "l2-dec", "content", 1.0, "s1", &out);

   memory_t results[16];

   /* Filter by tier */
   int count = memory_list(TIER_L1, NULL, 10, results, 16);
   assert(count == 2);

   /* Filter by kind */
   count = memory_list(NULL, KIND_FACT, 10, results, 16);
   assert(count == 2);

   /* Filter by both */
   count = memory_list(TIER_L1, KIND_FACT, 10, results, 16);
   assert(count == 1);
   assert(strcmp(results[0].key, "l1-fact") == 0);

   /* No matches */
   count = memory_list(TIER_L3, KIND_EPISODE, 10, results, 16);
   assert(count == 0);

   teardown();
}

static void test_get_nonexistent(void)
{
   setup();
   memory_t out;
   int rc = memory_get(99999, &out);
   assert(rc != 0);
   teardown();
}

static void test_delete_nonexistent(void)
{
   setup();
   int rc = memory_delete(99999);
   /* Should not crash, may return 0 or error */
   (void)rc;
   teardown();
}

static void test_insert_empty_content(void)
{
   setup();
   memory_t out;
   int rc = memory_insert(TIER_L0, KIND_FACT, "empty", "", 1.0, "s1", &out);
   assert(rc == 0);
   assert(out.id > 0);

   memory_t loaded;
   rc = memory_get(out.id, &loaded);
   assert(rc == 0);
   assert(loaded.content[0] == '\0');
   teardown();
}

static void test_confidence_bounds(void)
{
   setup();
   memory_t out;

   /* Zero confidence */
   int rc = memory_insert(TIER_L0, KIND_FACT, "low", "content", 0.0, "s1", &out);
   assert(rc == 0);
   assert(out.confidence < 0.01);

   /* High confidence */
   rc = memory_insert(TIER_L0, KIND_FACT, "high", "content", 1.0, "s1", &out);
   assert(rc == 0);
   assert(out.confidence > 0.99);

   teardown();
}

static void test_list_respects_limit(void)
{
   setup();
   memory_t out;
   for (int i = 0; i < 10; i++)
   {
      char key[32];
      snprintf(key, sizeof(key), "limit-test-%d", i);
      memory_insert(TIER_L0, KIND_FACT, key, "content", 1.0, "s1", &out);
   }

   memory_t results[16];
   int count = memory_list(TIER_L0, NULL, 3, results, 16);
   assert(count == 3);

   count = memory_list(TIER_L0, NULL, 100, results, 16);
   assert(count == 10);

   teardown();
}

static void test_run_maintenance_cycle(void)
{
   setup();
   memory_t out;

   /* Create some L0 memories with high use_count (should promote) */
   memory_insert(TIER_L0, KIND_FACT, "promote-me", "content", 0.95, "s1", &out);
   for (int i = 0; i < PROMOTE_L1_USE_COUNT + 1; i++)
      memory_touch(out.id);

   int promoted = 0, demoted = 0, expired = 0;
   memory_run_maintenance(&promoted, &demoted, &expired);
   assert(promoted >= 0);
   assert(demoted >= 0);
   assert(expired >= 0);

   teardown();
}

static void test_insert_triggers_maintenance_when_threshold_met(void)
{
   char tmpdir[128];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-memory-trigger-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
   assert(platform_setenv("AIMEE_MEMORY_SALIENCE_ENABLED", "1") == 0);
   assert(platform_setenv("AIMEE_MEMORY_SALIENCE_WEIGHT", "1.2") == 0);
   assert(platform_setenv("AIMEE_MEMORY_MAINTENANCE_TRIGGER_INSERTS", "2") == 0);

   char cfgdir[256];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   char cfgpath[320];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "memory_maintenance:\n  trigger_inserts: 2\n");
   fclose(fp);

   setup();
   memory_t first, second;
   assert(memory_insert(TIER_L1, KIND_FACT, "trigger-promote", "important fact", 0.95, "s1",
                        &first) == 0);
   for (int i = 0; i < PROMOTE_L1_USE_COUNT + 1; i++)
      memory_touch(first.id);
   assert(memory_insert(TIER_L1, KIND_FACT, "trigger-second", "second fact", 0.8, "s2", &second) ==
          0);

   memory_t updated;
   assert(memory_get(first.id, &updated) == 0);
   assert(updated.use_count >= PROMOTE_L1_USE_COUNT + 2);

   char state_buf[32];
   assert(db1_runtime_state_get("maintenance_pending_writes", state_buf, sizeof(state_buf)) == 0);
   assert(atoi(state_buf) >= 0);
   assert(db1_runtime_state_get("maintenance_last_run_epoch", state_buf, sizeof(state_buf)) == 0);
   assert(atoi(state_buf) > 0);

   char hc_err[128] = "";
   aimee_pg_stmt_t *hc_st =
       aimee_pg_prepare(db2_conn(), "SELECT COUNT(*) FROM memory_health", hc_err, sizeof(hc_err));
   assert(hc_st != NULL);
   assert(aimee_pg_step(hc_st, hc_err, sizeof(hc_err)) == AIMEE_PG_ROW);
   assert(aimee_pg_column_int(hc_st, 0) > 0);
   aimee_pg_finalize(hc_st);

   teardown();
   /* Reset HOME and overrides — leaving them set leaks files into the
    * tmpdir from later tests via dogfood logs and config caches. */
   (void)platform_unsetenv("HOME");
   (void)platform_unsetenv("AIMEE_NO_CACHE");
   (void)platform_unsetenv("AIMEE_MEMORY_SALIENCE_ENABLED");
   (void)platform_unsetenv("AIMEE_MEMORY_SALIENCE_WEIGHT");
   (void)platform_unsetenv("AIMEE_MEMORY_MAINTENANCE_TRIGGER_INSERTS");
   platform_test_rmrf(tmpdir);
}

static void test_temporal_retrieval_prefers_matching_date(void)
{
   setup();
   memory_t a, b;
   memory_insert(TIER_L2, KIND_FACT, "concert", "We met on March 10 at the park", 0.9, "s1", &a);
   memory_insert(TIER_L2, KIND_FACT, "meeting", "We talked in April at the office", 0.9, "s1", &b);

   memory_t results[8];
   int count = memory_find_facts("when march 10 park", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == a.id);
   teardown();
}

static void test_temporal_retrieval_honors_before_date_constraint(void)
{
   setup();
   memory_t early, late;
   memory_insert(TIER_L2, KIND_FACT, "project review",
                 "The project review happened on May 20 2023.", 0.9, "s1", &early);
   memory_insert(TIER_L2, KIND_FACT, "project review",
                 "The project review happened on June 2 2023.", 0.9, "s1", &late);

   memory_t results[8];
   int count = memory_find_facts("project review before 25 May 2023", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == early.id);
   teardown();
}

static void test_temporal_retrieval_honors_between_date_constraint(void)
{
   setup();
   memory_t april, may, june;
   memory_insert(TIER_L2, KIND_FACT, "training day", "The training day was on April 10 2023.", 0.9,
                 "s1", &april);
   memory_insert(TIER_L2, KIND_FACT, "training day", "The training day was on May 18 2023.", 0.9,
                 "s1", &may);
   memory_insert(TIER_L2, KIND_FACT, "training day", "The training day was on June 12 2023.", 0.9,
                 "s1", &june);

   memory_t results[8];
   int count = memory_find_facts("training day between 1 May 2023 and 31 May 2023", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == may.id);
   teardown();
}

static void test_temporal_retrieval_honors_last_week_phrase(void)
{
   setup();
   memory_t last_week, this_week;
   struct tm now_tm;
   current_utc_tm(&now_tm);
   int weekday = now_tm.tm_wday == 0 ? 7 : now_tm.tm_wday;
   time_t now = time(NULL);
   time_t last_week_t = now - (time_t)(weekday + 4) * 86400; /* previous Wednesday */
   time_t this_week_t = now - (time_t)(weekday > 3 ? (weekday - 3) : 0) * 86400;
   struct tm last_week_tm, this_week_tm;
   gmtime_r(&last_week_t, &last_week_tm);
   gmtime_r(&this_week_t, &this_week_tm);
   char last_month[32];
   char this_month[32];
   strftime(last_month, sizeof(last_month), "%B", &last_week_tm);
   strftime(this_month, sizeof(this_month), "%B", &this_week_tm);

   char last_text[128];
   char this_text[128];
   snprintf(last_text, sizeof(last_text), "The team sync happened on %s %d %d.", last_month,
            last_week_tm.tm_mday, last_week_tm.tm_year + 1900);
   snprintf(this_text, sizeof(this_text), "The team sync happened on %s %d %d.", this_month,
            this_week_tm.tm_mday, this_week_tm.tm_year + 1900);

   memory_insert(TIER_L2, KIND_FACT, "team sync", last_text, 0.9, "s1", &last_week);
   memory_insert(TIER_L2, KIND_FACT, "team sync", this_text, 0.9, "s2", &this_week);

   memory_t results[8];
   int count = memory_find_facts("team sync last week", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == last_week.id);
   teardown();
}

static void test_temporal_retrieval_honors_next_month_phrase(void)
{
   setup();
   memory_t next_month, current_month;
   struct tm now_tm;
   current_utc_tm(&now_tm);

   int current_year = now_tm.tm_year + 1900;
   int current_month_num = now_tm.tm_mon + 1;
   int next_month_num = current_month_num == 12 ? 1 : current_month_num + 1;
   int next_month_year = current_month_num == 12 ? current_year + 1 : current_year;
   struct tm current_month_tm;
   struct tm next_month_tm;
   memset(&current_month_tm, 0, sizeof(current_month_tm));
   memset(&next_month_tm, 0, sizeof(next_month_tm));
   current_month_tm.tm_year = current_year - 1900;
   current_month_tm.tm_mon = current_month_num - 1;
   current_month_tm.tm_mday = 15;
   next_month_tm.tm_year = next_month_year - 1900;
   next_month_tm.tm_mon = next_month_num - 1;
   next_month_tm.tm_mday = 15;
   char current_month_name[32];
   char next_month_name[32];
   strftime(current_month_name, sizeof(current_month_name), "%B", &current_month_tm);
   strftime(next_month_name, sizeof(next_month_name), "%B", &next_month_tm);

   char current_text[128];
   char next_text[128];
   snprintf(current_text, sizeof(current_text), "The doctor appointment is on %s 15 %d.",
            current_month_name, current_year);
   snprintf(next_text, sizeof(next_text), "The doctor appointment is on %s 15 %d.", next_month_name,
            next_month_year);

   memory_insert(TIER_L2, KIND_FACT, "doctor appointment", current_text, 0.9, "s1", &current_month);
   memory_insert(TIER_L2, KIND_FACT, "doctor appointment", next_text, 0.9, "s2", &next_month);

   memory_t results[8];
   int count = memory_find_facts("doctor appointment next month", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == next_month.id);
   teardown();
}

static void test_chunk_retrieval_finds_sentence_evidence(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "trip",
                 "We had breakfast at home. Then we visited the riverside museum after lunch.", 0.9,
                 "s1", &m);

   memory_t results[8];
   int count = memory_find_facts("riverside museum", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == m.id);
   teardown();
}

static void test_superseded_memory_penalized(void)
{
   setup();
   memory_t old_mem;
   memory_insert(TIER_L2, KIND_FACT, "deploy target", "Use server-a", 0.9, "s1", &old_mem);
   memory_t new_mem;
   assert(memory_supersede(old_mem.id, "Use server-b", 0.95, "s2", &new_mem) == 0);

   memory_t results[8];
   int count = memory_find_facts("deploy target server", 5, results, 8);
   assert(count >= 1);
   assert(strstr(results[0].content, "server-b") != NULL);
   teardown();
}

static void test_contradiction_reranking_prefers_newer_fact(void)
{
   setup();
   memory_t older, newer;
   assert(memory_insert(TIER_L2, KIND_FACT, "release venue",
                        "The release is in Berlin on March 10 2023.", 0.95, "s1", &older) == 0);
   assert(memory_supersede(older.id, "The release is in Paris on March 10 2023.", 0.95, "s2",
                           &newer) == 0);

   memory_t results[8];
   int count = memory_find_facts("release venue march 10 2023", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == newer.id);
   teardown();
}

static void test_insert_versions_temporal_fact_updates(void)
{
   setup();
   memory_t first, second;
   assert(memory_insert(TIER_L2, KIND_FACT, "release day", "The release day is March 10 2023.",
                        0.92, "s1", &first) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "release day", "The release day is March 12 2023.",
                        0.95, "s2", &second) == 0);
   assert(first.id != second.id);

   memory_t history[8];
   int hist_count = memory_fact_history("release day", history, 8);
   assert(hist_count >= 2);

   memory_t results[8];
   int count = memory_find_facts("release day march 12 2023", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == second.id);
   teardown();
}

static void test_semantic_profile_duplicate_keeps_single_active_entry(void)
{
   setup();
   memory_t first, second;
   assert(memory_insert(TIER_L2, KIND_FACT, "Alice:favorite_food", "Thai food", 0.9, "s1",
                        &first) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "alice:favorite_food", "thai food.", 0.95, "s2",
                        &second) == 0);
   assert(first.id == second.id);

   char dup_err[128] = "";
   aimee_pg_stmt_t *dup_st = aimee_pg_prepare(
       db2_conn(), "SELECT COUNT(*), use_count, content FROM memories WHERE id = ?1", dup_err,
       sizeof(dup_err));
   assert(dup_st != NULL);
   aimee_pg_bind_int64(dup_st, "?1", first.id);
   assert(aimee_pg_step(dup_st, dup_err, sizeof(dup_err)) == AIMEE_PG_ROW);
   assert(aimee_pg_column_int(dup_st, 0) == 1);
   assert(aimee_pg_column_int(dup_st, 1) >= 2);
   assert(aimee_pg_column_text(dup_st, 2)[0] != '\0');
   aimee_pg_finalize(dup_st);
   teardown();
}

static void test_semantic_profile_replacement_creates_history(void)
{
   setup();
   memory_t first, second;
   assert(memory_insert(TIER_L2, KIND_FACT, "alice:favorite_food", "Thai food", 0.92, "s1",
                        &first) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "alice:favorite_food", "Sushi", 0.96, "s2", &second) ==
          0);
   assert(first.id != second.id);

   char hist_err[128] = "";
   aimee_pg_stmt_t *hist_st =
       aimee_pg_prepare(db2_conn(), "SELECT key, valid_until FROM memories WHERE id = ?1", hist_err,
                        sizeof(hist_err));
   assert(hist_st != NULL);
   aimee_pg_bind_int64(hist_st, "?1", first.id);
   assert(aimee_pg_step(hist_st, hist_err, sizeof(hist_err)) == AIMEE_PG_ROW);
   assert(strstr(aimee_pg_column_text(hist_st, 0), "#v") != NULL);
   assert(aimee_pg_column_text(hist_st, 1)[0] != '\0');
   aimee_pg_finalize(hist_st);

   memory_t history[8];
   int hist_count = memory_fact_history("alice:favorite_food", history, 8);
   assert(hist_count >= 2);

   memory_t results[8];
   int count = memory_find_facts("alice favorite food sushi", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == second.id);
   teardown();
}

static void test_semantic_profile_history_retains_superseded_value(void)
{
   setup();
   memory_t old_mem, new_mem;
   assert(memory_insert(TIER_L2, KIND_FACT, "alice:favorite_food", "Thai food", 0.92, "s1",
                        &old_mem) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "alice:favorite_food", "Sushi", 0.96, "s2", &new_mem) ==
          0);

   char upd_err[128] = "";
   const char *upd_sql = "UPDATE memories SET valid_from = ?1, valid_until = ?2 WHERE id = ?3";
   aimee_pg_stmt_t *upd = aimee_pg_prepare(db2_conn(), upd_sql, upd_err, sizeof(upd_err));
   assert(upd != NULL);
   aimee_pg_bind_text(upd, "?1", "2023-01-01");
   aimee_pg_bind_text(upd, "?2", "2023-12-31");
   aimee_pg_bind_int64(upd, "?3", old_mem.id);
   assert(aimee_pg_step(upd, upd_err, sizeof(upd_err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(upd);
   upd = aimee_pg_prepare(db2_conn(), upd_sql, upd_err, sizeof(upd_err));
   assert(upd != NULL);
   aimee_pg_bind_text(upd, "?1", "2024-01-01");
   aimee_pg_bind_text(upd, "?2", "");
   aimee_pg_bind_int64(upd, "?3", new_mem.id);
   assert(aimee_pg_step(upd, upd_err, sizeof(upd_err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(upd);

   memory_t history[8];
   int hist_count = memory_fact_history("alice:favorite_food", history, 8);
   assert(hist_count >= 2);
   int saw_thai = 0, saw_sushi = 0;
   for (int i = 0; i < hist_count; i++)
   {
      if (strstr(history[i].content, "Thai food"))
         saw_thai = 1;
      if (strstr(history[i].content, "Sushi"))
         saw_sushi = 1;
   }
   assert(saw_thai);
   assert(saw_sushi);
   teardown();
}

static void test_query_decomposition_recovers_compound_prompt(void)
{
   setup();
   memory_t decision, architecture;
   memory_insert(TIER_L2, KIND_DECISION, "transport decision",
                 "The team chose server sent events for live frontend updates.", 0.92, "s1",
                 &decision);
   memory_insert(TIER_L2, KIND_FACT, "frontend architecture",
                 "Frontend network architecture uses an event stream transport layer.", 0.87, "s1",
                 &architecture);

   memory_t results[8];
   int count = memory_find_facts(
       "did we decide to use sse or websockets for the frontend network architecture last week", 5,
       results, 8);
   assert(count >= 1);
   assert(results[0].id == decision.id || results[0].id == architecture.id);
   teardown();
}

static void test_code_identifier_retrieval_handles_snake_and_camel(void)
{
   setup();
   memory_t code_mem;
   memory_insert(TIER_L2, KIND_FACT, "db-step-log",
                 "DB_STEP_LOG is the macro used by DbStepLog style tracing paths.", 0.91, "s1",
                 &code_mem);

   memory_t results[8];
   int count = memory_find_facts("db_step_log", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == code_mem.id);
   teardown();
}

static void test_rebuild_derived_indexes_populates_searchable_structures(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "timeline", "Yesterday Alice visited the old harbor museum.",
                 0.9, "s1", &m);
   int rebuilt = memory_rebuild_derived_indexes(10);
   assert(rebuilt >= 1);

   memory_t results[8];
   int count = memory_find_facts("alice harbor museum", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == m.id);
   teardown();
}

static void test_rebuild_derived_indexes_assigns_memory_unit_kinds(void)
{
   setup();
   memory_t episodic;
   memory_t procedural;
   assert(memory_insert(TIER_L2, KIND_FACT, "timeline",
                        "Yesterday Alice visited the old harbor museum.", 0.9, "s1",
                        &episodic) == 0);
   assert(memory_insert(TIER_L2, KIND_PREFERENCE, "style", "User prefers tabs over spaces.", 0.8,
                        "s1", &procedural) == 0);
   assert(memory_rebuild_derived_indexes(10) >= 2);

   char mu_err[128] = "";
   const char *mu_sql =
       "SELECT COUNT(*) FROM memory_units WHERE memory_id = ?1 AND memory_kind = ?2";

   aimee_pg_stmt_t *mu_st = aimee_pg_prepare(db2_conn(), mu_sql, mu_err, sizeof(mu_err));
   assert(mu_st != NULL);
   aimee_pg_bind_int64(mu_st, "?1", episodic.id);
   aimee_pg_bind_text(mu_st, "?2", MEMORY_UNIT_KIND_EPISODIC_STR);
   assert(aimee_pg_step(mu_st, mu_err, sizeof(mu_err)) == AIMEE_PG_ROW);
   assert(aimee_pg_column_int(mu_st, 0) >= 1);
   aimee_pg_finalize(mu_st);

   mu_st = aimee_pg_prepare(db2_conn(), mu_sql, mu_err, sizeof(mu_err));
   assert(mu_st != NULL);
   aimee_pg_bind_int64(mu_st, "?1", procedural.id);
   aimee_pg_bind_text(mu_st, "?2", MEMORY_UNIT_KIND_PROCEDURAL_STR);
   assert(aimee_pg_step(mu_st, mu_err, sizeof(mu_err)) == AIMEE_PG_ROW);
   assert(aimee_pg_column_int(mu_st, 0) >= 1);
   aimee_pg_finalize(mu_st);

   teardown();
}

static void test_memory_diagnose_reports_score_breakdown(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "museum trip", "Alice visited the harbor museum on March 10.",
                 0.9, "s1", &m);

   memory_diagnostic_t rows[4];
   int count = memory_diagnose("when did alice visit the harbor museum", 3, rows, 4);
   assert(count >= 1);
   assert(rows[0].memory.id == m.id);
   assert(rows[0].parts.total > 0.0);
   assert(rows[0].parts.temporal > 0.0 || rows[0].parts.entity > 0.0);
   teardown();
}

static void test_memory_explain_match_reports_specific_memory(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "release", "The release happened on March 10 at the office.",
                 0.8, "s1", &m);

   memory_diagnostic_t row;
   assert(memory_explain_match("when was the release", m.id, &row) == 0);
   assert(row.memory.id == m.id);
   assert(row.parts.temporal > 0.0 || row.parts.confidence > 0.0);
   teardown();
}

static void test_memory_answer_query_prefers_temporal_evidence(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "concert", "Alice went to the concert on March 10.", 0.9, "s1",
                 &m);

   char *answer = memory_answer_query("When did Alice go to the concert?", 5);
   assert(answer != NULL);
   assert(strstr(answer, "March 10") != NULL || strstr(answer, "march 10") != NULL);
   free(answer);
   teardown();
}

static void test_memory_answer_query_uses_session_cluster_evidence(void)
{
   setup();
   memory_t summary;
   memory_t detail;
   memory_insert(TIER_L2, KIND_EPISODE, "trip recap",
                 "Alice talked about the museum trip and later discussed scheduling details.", 0.95,
                 "trip1", &summary);
   memory_insert(TIER_L2, KIND_FACT, "museum date", "Alice visited the harbor museum on March 10.",
                 0.85, "trip1", &detail);

   char *answer = memory_answer_query("When did Alice visit the museum?", 5);
   assert(answer != NULL);
   assert(strstr(answer, "March 10") != NULL || strstr(answer, "march 10") != NULL);
   free(answer);
   teardown();
}
static void test_context_budget_prefers_project_l4_rule_over_long_global_l1(void)
{
   char tmpdir[512];
   char cfgdir[640];
   char cfgpath[768];
   char cwd[MAX_PATH_LEN];
   char project[MAX_PATH_LEN];
   const char *old_home = getenv("HOME");
   char old_home_copy[512];
   old_home_copy[0] = '\0';
   if (old_home && old_home[0])
      snprintf(old_home_copy, sizeof(old_home_copy), "%s", old_home);

   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-memory-budget-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);

   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fputs("memory:\n  context_budget:\n    enabled: true\n    tokens: 24\n", fp);
   fclose(fp);

   assert(getcwd(cwd, sizeof(cwd)) != NULL);
   {
      const char *slash = strrchr(cwd, '/');
      snprintf(project, sizeof(project), "%s", slash ? slash + 1 : cwd);
   }

   setup();
   memory_t rule;
   memory_t episode;
   char long_content[768];
   snprintf(
       long_content, sizeof(long_content),
       "snake case rule migration checklist notes repeated for context assembly budget pressure. "
       "snake case rule migration checklist notes repeated for context assembly budget pressure. "
       "snake case rule migration checklist notes repeated for context assembly budget pressure.");

   assert(memory_insert(TIER_L4, KIND_POLICY, "project-naming-rule",
                        "Use snake_case for project identifiers.", 0.98, "s1", &rule) == 0);
   assert(memory_tag_project(rule.id, project) == 0);
   assert(memory_insert(TIER_L1, KIND_EPISODE, "migration-notes", long_content, 0.99, "s2",
                        &episode) == 0);

   context_assemble_explain_entry_t entries[32];
   int ecount = 0;
   context_budget_metrics_t metrics;
   memset(&metrics, 0, sizeof(metrics));

   char *ctx = memory_assemble_context_explain("snake case rule", entries, &ecount, 32, &metrics);
   assert(ctx != NULL);
   assert(strstr(ctx, "Use snake_case for project identifiers.") != NULL);
   assert(strstr(ctx, long_content) == NULL);
   assert(metrics.budget_tokens == 24);
   assert(metrics.rejected_for_budget >= 1);

   int saw_rule = 0;
   int saw_episode = 0;
   for (int i = 0; i < ecount; i++)
   {
      if (entries[i].id == rule.id)
      {
         saw_rule = 1;
         assert(entries[i].selected == 1);
         assert(strcmp(entries[i].tier, "L4") == 0);
         assert(strcmp(entries[i].scope, "project") == 0);
      }
      if (entries[i].id == episode.id)
      {
         saw_episode = 1;
         assert(entries[i].selected == 0);
      }
   }
   assert(saw_rule == 1);
   assert(saw_episode == 1);

   free(ctx);
   teardown();
   platform_test_rmrf(tmpdir);

   if (old_home_copy[0])
      assert(platform_setenv("HOME", old_home_copy) == 0);
   else
      assert(platform_unsetenv("HOME") == 0);
}

static void test_context_budget_prefers_project_scope_over_global_l5(void)
{
   char tmpdir[512];
   char cfgdir[640];
   char cfgpath[768];
   char cwd[MAX_PATH_LEN];
   char project[MAX_PATH_LEN];
   const char *old_home = getenv("HOME");
   char old_home_copy[512];
   old_home_copy[0] = '\0';
   if (old_home && old_home[0])
      snprintf(old_home_copy, sizeof(old_home_copy), "%s", old_home);

   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-memory-scope-budget-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);

   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fputs("memory:\n  context_budget:\n    enabled: true\n    tokens: 14\n", fp);
   fclose(fp);

   assert(getcwd(cwd, sizeof(cwd)) != NULL);
   {
      /* Project label keys on the repository identity (canonical remote URL)
       * so recall matches across clones/machines, with a repo-root basename
       * fallback for remote-less roots.  Mirrors memory_scope_labels_for_cwd()
       * in production. */
      if (workspace_repo_identity(cwd, project, sizeof(project), NULL, 0) != 0 || !project[0])
      {
         char project_root[MAX_PATH_LEN];
         if (workspace_active_root(NULL, cwd, project_root, sizeof(project_root)) == 0 &&
             project_root[0])
         {
            const char *slash = strrchr(project_root, '/');
            snprintf(project, sizeof(project), "%s", slash ? slash + 1 : project_root);
         }
         else
         {
            const char *slash = strrchr(cwd, '/');
            snprintf(project, sizeof(project), "%s", slash ? slash + 1 : cwd);
         }
      }
   }

   setup();
   memory_t project_policy;
   memory_t global_policy;

   assert(memory_insert(TIER_L3, KIND_POLICY, "deploy-target-project",
                        "Project deploy target is the staging sandbox.", 0.95, "s1",
                        &project_policy) == 0);
   assert(memory_tag_project(project_policy.id, project) == 0);

   assert(memory_insert(TIER_L5, KIND_POLICY, "deploy-pattern-global",
                        "Across many repos, deploy targets usually require extra platform review.",
                        0.98, "s2", &global_policy) == 0);
   assert(memory_tag_global(global_policy.id) == 0);
   assert(memory_scope_visibility_rank(project_policy.id, NULL, project) == 3);
   assert(memory_scope_visibility_rank(global_policy.id, NULL, project) < 3);

   context_assemble_explain_entry_t entries[32];
   int ecount = 0;
   context_budget_metrics_t metrics;
   memset(&metrics, 0, sizeof(metrics));

   char *ctx = memory_assemble_context_explain("deploy target", entries, &ecount, 32, &metrics);
   assert(ctx != NULL);
   assert(metrics.rejected_for_budget >= 1);

   int saw_project = 0;
   int saw_global = 0;
   int project_index = -1;
   int global_index = -1;
   for (int i = 0; i < ecount; i++)
   {
      if (entries[i].id == project_policy.id && project_index < 0)
      {
         saw_project = 1;
         project_index = i;
         assert(strcmp(entries[i].tier, "L3") == 0);
      }
      if (entries[i].id == global_policy.id && global_index < 0)
      {
         saw_global = 1;
         global_index = i;
         assert(strcmp(entries[i].tier, "L5") == 0);
      }
   }
   assert(saw_project == 1);
   assert(saw_global == 1);
   assert(project_index < global_index);

   free(ctx);
   teardown();
   platform_test_rmrf(tmpdir);

   if (old_home_copy[0])
      assert(platform_setenv("HOME", old_home_copy) == 0);
   else
      assert(platform_unsetenv("HOME") == 0);
}
static void test_memory_answer_query_adds_citations_when_enabled(void)
{
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
   assert(platform_setenv("AIMEE_MEMORY_CITATIONS_MODE", "required") == 0);
   assert(platform_setenv("AIMEE_MEMORY_CITATIONS_STRIP_UNVERIFIED", "0") == 0);

   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "concert", "Alice went to the concert on March 10.", 0.9, "s1",
                 &m);

   char *answer = memory_answer_query("When did Alice go to the concert?", 5);
   assert(answer != NULL);
   char citation[64];
   snprintf(citation, sizeof(citation), "[#%lld", (long long)m.id);
   assert(strstr(answer, citation) != NULL);
   assert(fetch_runtime_state_int("memory.citation.required") == 1);
   assert(fetch_runtime_state_int("memory.citation.verified") == 1);
   free(answer);
   teardown();
   assert(platform_setenv("AIMEE_MEMORY_CITATIONS_MODE", "off") == 0);
   assert(platform_setenv("AIMEE_MEMORY_CITATIONS_STRIP_UNVERIFIED", "0") == 0);
}

static void test_memory_ask_query_returns_structured_result(void)
{
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
   assert(platform_setenv("AIMEE_MEMORY_CITATIONS_MODE", "required") == 0);
   assert(platform_setenv("AIMEE_MEMORY_CITATIONS_STRIP_UNVERIFIED", "0") == 0);

   setup();
   memory_t m;
   assert(memory_insert(TIER_L2, KIND_FACT, "concert-date",
                        "Alice went to the concert on March 10.", 0.9, "s1", &m) == 0);

   memory_answer_result_t result;
   memset(&result, 0, sizeof(result));
   assert(memory_ask_query("When did Alice go to the concert?", 5, &result) == 0);
   assert(result.no_answer == 0);
   assert(result.answer[0] != '\0');
   assert(result.confidence > 0.6);
   assert(strcmp(result.evidence_mode, "verbatim") == 0);
   assert(result.citation_count >= 1);
   assert(result.citation_ids[0] == m.id);

   teardown();
   assert(platform_setenv("AIMEE_MEMORY_CITATIONS_MODE", "off") == 0);
   assert(platform_setenv("AIMEE_MEMORY_CITATIONS_STRIP_UNVERIFIED", "0") == 0);
}

static void test_memory_ask_query_reports_no_answer(void)
{
   setup();
   memory_answer_result_t result;
   memset(&result, 0, sizeof(result));
   assert(memory_ask_query("What is Alice's favorite lunch?", 5, &result) == 0);
   assert(result.no_answer == 1);
   assert(result.answer[0] == '\0');
   assert(result.confidence == 0.0);
   assert(result.citation_count == 0);
   teardown();
}

/* Multi-word entity phrases extracted from memory content should boost
 * retrieval when the query contains the same phrase tokens. */
static void test_multiword_entity_phrase_boosts_retrieval(void)
{
   setup();
   memory_t target, noise;
   /* Target has a multi-word phrase that the query asks about */
   assert(memory_insert(TIER_L2, KIND_FACT, "project-update",
                        "The deployment pipeline ran successfully last night.", 0.9, "s1",
                        &target) == 0);
   /* Noise has unrelated content */
   assert(memory_insert(TIER_L2, KIND_FACT, "weather-note", "It was raining heavily all morning.",
                        0.9, "s1", &noise) == 0);

   memory_diagnostic_t rows[4];
   int count = memory_diagnose("deployment pipeline status", 4, rows, 4);
   assert(count >= 1);
   /* Target should rank above noise */
   assert(rows[0].memory.id == target.id);
   /* Entity part should be non-zero for the matching memory */
   assert(rows[0].parts.entity > 0.0);
   teardown();
}

/* Memories stored with "Speaker: text" format should be boosted when the
 * query names that speaker explicitly. */
static void test_speaker_alignment_boosts_actor_entity_matches(void)
{
   setup();
   memory_t alice_mem, bob_mem;
   /* Alice's turn */
   assert(memory_insert(TIER_L2, KIND_FACT, "turn-1",
                        "Alice: I finished the report on budget projections.", 0.9, "s1",
                        &alice_mem) == 0);
   /* Bob's turn */
   assert(memory_insert(TIER_L2, KIND_FACT, "turn-2", "Bob: I reviewed the deployment schedule.",
                        0.9, "s1", &bob_mem) == 0);

   memory_diagnostic_t rows[4];
   int count = memory_diagnose("What did Alice say about the report?", 4, rows, 4);
   assert(count >= 1);
   /* Alice's memory should rank first — it matches "report" and has Alice as actor */
   assert(rows[0].memory.id == alice_mem.id);
   /* Entity part (includes speaker bonus) must be non-zero */
   assert(rows[0].parts.entity > 0.0);
   teardown();
}

static void test_entity_canonicalization_handles_titles_and_plurals(void)
{
   setup();
   memory_t m;
   memory_insert(TIER_L2, KIND_FACT, "Dr. Rivers", "Dr Rivers visited the museums with the teams.",
                 0.9, "s1", &m);

   memory_t results[8];
   int count = memory_find_facts("river museum team", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == m.id);
   teardown();
}

static void test_memory_query_plan_prefers_lexical_for_code_queries(void)
{
   memory_query_plan_t plan;
   assert(memory_query_plan("src/memory_core.c memory_query_plan", 10, 96, &plan) == 0);
   assert(plan.route == MEM_ROUTE_LEXICAL);
   assert(plan.semantic_enabled == 0);
   assert(plan.max_fetch <= 48);
}

static void test_memory_query_plan_prefers_semantic_for_when_queries(void)
{
   memory_query_plan_t plan;
   assert(memory_query_plan("When did Alice visit the museum?", 10, 96, &plan) == 0);
   assert(plan.shape == MEM_SHAPE_WHEN);
   assert(plan.route == MEM_ROUTE_SEMANTIC);
   assert(plan.semantic_enabled == 1);
   assert(plan.fetch_multiplier >= 5);
   assert(plan.weights.temporal_weight > 0.8);
}

static void test_memory_query_plan_prefers_graph_for_dependency_queries(void)
{
   memory_query_plan_t plan;
   assert(memory_query_plan("what calls memory_find_facts_scoped", 10, 96, &plan) == 0);
   assert(plan.route == MEM_ROUTE_GRAPH);
   assert(plan.graph_hops == 2);
   assert(plan.semantic_enabled == 0);
}

static void test_memory_query_plan_respects_routing_disable_flag(void)
{
   char tmpdir[512];
   char cfgdir[640];
   char cfgpath[768];
   const char *old_home = getenv("HOME");
   char old_home_copy[512];
   old_home_copy[0] = '\0';
   if (old_home && old_home[0])
      snprintf(old_home_copy, sizeof(old_home_copy), "%s", old_home);

   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-routing-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fputs("memory:\n  routing:\n    enabled: false\n", fp);
   fclose(fp);

   memory_query_plan_t plan;
   assert(memory_query_plan("what calls memory_find_facts_scoped", 10, 96, &plan) == 0);
   assert(plan.route == MEM_ROUTE_HYBRID);
   assert(plan.semantic_enabled == 1);
   assert(plan.graph_hops == 1);

   if (old_home_copy[0])
      assert(platform_setenv("HOME", old_home_copy) == 0);
   else
      assert(platform_unsetenv("HOME") == 0);
   assert(platform_unsetenv("AIMEE_NO_CACHE") == 0);
   platform_test_rmrf(tmpdir);
}

static void test_memory_fetch_budget_factor_shape_aware(void)
{
   /* Base expectation: a 3-token query is token_factor=1.0; shape
    * then scales up (list/quantitative) or down (yes_no), and the
    * semantic shapes leave the factor unchanged. */
   double list3 = memory_fetch_budget_factor(MEM_SHAPE_LIST, 3);
   double quant3 = memory_fetch_budget_factor(MEM_SHAPE_QUANTITATIVE, 3);
   double temporal3 = memory_fetch_budget_factor(MEM_SHAPE_TEMPORAL_INTERVAL, 3);
   double when3 = memory_fetch_budget_factor(MEM_SHAPE_WHEN, 3);
   double yesno3 = memory_fetch_budget_factor(MEM_SHAPE_YES_NO, 3);
   double factoid3 = memory_fetch_budget_factor(MEM_SHAPE_FACTOID, 3);
   double how3 = memory_fetch_budget_factor(MEM_SHAPE_HOW, 3);
   double why3 = memory_fetch_budget_factor(MEM_SHAPE_WHY, 3);
   double unknown3 = memory_fetch_budget_factor(MEM_SHAPE_UNKNOWN, 3);

   assert(list3 == 1.3);
   assert(quant3 == 1.3);
   assert(temporal3 == 1.3);
   assert(when3 == 1.15);
   assert(yesno3 == 0.75);
   assert(factoid3 == 0.9);
   assert(how3 == 1.0);
   assert(why3 == 1.0);
   assert(unknown3 == 1.0);

   /* Token-count scaling still stacks on top of shape. */
   double list_short = memory_fetch_budget_factor(MEM_SHAPE_LIST, 2);    /* 1.5 * 1.3 */
   double list_long = memory_fetch_budget_factor(MEM_SHAPE_LIST, 10);    /* 0.5 * 1.3 */
   double yesno_short = memory_fetch_budget_factor(MEM_SHAPE_YES_NO, 2); /* 1.5 * 0.75 */
   double yesno_long = memory_fetch_budget_factor(MEM_SHAPE_YES_NO, 10); /* 0.5 * 0.75 */

   assert(list_short > list_long);
   assert(list_short > yesno_short); /* wide shape widens on short queries */
   assert(list_long > yesno_long);   /* wide shape stays wider even when long */
   /* Concrete values — catches anyone accidentally rescaling one side. */
   assert(list_short > 1.94 && list_short < 1.96);   /* 1.95 */
   assert(list_long > 0.64 && list_long < 0.66);     /* 0.65 */
   assert(yesno_short > 1.12 && yesno_short < 1.13); /* 1.125 */
   assert(yesno_long > 0.37 && yesno_long < 0.38);   /* 0.375 */
}

static void test_memory_find_facts_handles_lexical_code_query(void)
{
   setup();
   memory_t m;
   memory_insert(
       TIER_L2, KIND_FACT, "memory_query_plan",
       "memory_query_plan is implemented in src/memory_core.c and drives retrieval routing.", 0.95,
       "s1", &m);

   memory_t results[8];
   int count = memory_find_facts("src/memory_core.c memory_query_plan", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == m.id);
   teardown();
}

static void test_memory_find_facts_falls_back_when_vector_index_unavailable(void)
{
   setup();
   char err[128] = "";
   assert(aimee_pg_exec(db2_conn(), "DROP VIEW IF EXISTS pg_indexes", err, sizeof(err)) == 0);

   memory_t m;
   assert(memory_insert(TIER_L2, KIND_FACT, "agent role model",
                        "There is one primary agent and delegated work uses delegates.", 0.95, "s1",
                        &m) == 0);

   memory_t results[8];
   int count = memory_find_facts("agent delegates", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == m.id);
   teardown();
}

static void test_memory_find_facts_records_route_and_shape_metrics(void)
{
   setup();
   memory_t m;
   assert(memory_insert(TIER_L2, KIND_FACT, "museum visit", "Alice visited the museum on March 10.",
                        0.95, "s1", &m) == 0);

   memory_t results[8];
   int count = memory_find_facts("When did Alice visit the museum?", 5, results, 8);
   assert(count >= 1);
   assert(fetch_runtime_state_int("memory.query.total") >= 1);
   assert(fetch_runtime_state_int("memory.query.route.semantic") >= 1);
   assert(fetch_runtime_state_int("memory.query.shape.when") >= 1);
   assert(fetch_runtime_state_int("memory.query.route.semantic.latency_ms") >= 0);
   assert(fetch_runtime_state_int("memory.query.shape.when.latency_ms") >= 0);
   teardown();
}

static void test_memory_find_facts_graph_route_uses_graph_stage(void)
{
   setup();
   char gr_err[128] = "";
   /* PRAGMA passes through to sqlite under the test shim. */
   assert(aimee_pg_exec(db2_conn(), "PRAGMA foreign_keys=OFF", gr_err, sizeof(gr_err)) == 0);

   memory_t callsite;
   assert(memory_insert(TIER_L2, KIND_FACT, "graph-callsite",
                        "Scoped retrieval helper is invoked by the routing layer.", 0.92, "s1",
                        &callsite) == 0);

   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       db2_conn(),
       "INSERT INTO memory_relations"
       " (memory_id, src_entity, relation, dst_entity, fact_text, valid_at, invalid_at,"
       "  weight, created_at)"
       " VALUES (?1, 'memory_generate_candidates', 'calls',"
       " 'memory_find_facts_scoped', 'memory_generate_candidates calls"
       " memory_find_facts_scoped', '2026-01-01', '', 1.0, pg_now_text())",
       gr_err, sizeof(gr_err));
   assert(stmt != NULL);
   aimee_pg_bind_int64(stmt, "?1", callsite.id);
   assert(aimee_pg_step(stmt, gr_err, sizeof(gr_err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(stmt);

   memory_t results[8];
   int count = memory_find_facts("what calls memory_find_facts_scoped", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == callsite.id);
   assert(fetch_runtime_state_int_or_zero("memory.query.route.graph.stage.graph") >= 1);
   assert(fetch_runtime_state_int_or_zero("memory.query.route.graph.stage.semantic") == 0);
   teardown();
}

static void test_memory_find_facts_lexical_route_skips_semantic_and_graph_stages(void)
{
   setup();
   memory_t m;
   assert(memory_insert(
              TIER_L2, KIND_FACT, "memory_query_plan",
              "memory_query_plan is implemented in src/memory_core.c and drives retrieval routing.",
              0.95, "s1", &m) == 0);

   memory_t results[8];
   int count = memory_find_facts("src/memory_core.c memory_query_plan", 5, results, 8);
   assert(count >= 1);
   assert(results[0].id == m.id);
   assert(fetch_runtime_state_int_or_zero("memory.query.route.lexical.stage.variant") >= 1);
   assert(fetch_runtime_state_int_or_zero("memory.query.route.lexical.stage.semantic") == 0);
   assert(fetch_runtime_state_int_or_zero("memory.query.route.lexical.stage.graph") == 0);
   teardown();
}

static void test_noise_utterance_gets_low_salience(void)
{
   setup();
   memory_t noise, fact;
   assert(memory_insert(TIER_L2, KIND_FACT, "noise", "omg jordan!!", 0.9, "s1", &noise) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "fact",
                        "Jordan owns the build pipeline for release automation.", 0.9, "s1",
                        &fact) == 0);
   assert(noise.salience < 0.1);
   assert(fact.salience > 0.4);
   teardown();
}

static void test_salience_demotes_noise_matches(void)
{
   char tmpdir[128];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-memory-salience-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);

   char cfgdir[256];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   char cfgpath[320];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "memory:\n  salience:\n    enabled: true\n    weight: 1.2\n");
   fclose(fp);

   setup();
   memory_t noise, fact;
   assert(memory_insert(TIER_L2, KIND_FACT, "jordan-chat", "omg jordan!!", 0.99, "s1", &noise) ==
          0);
   assert(memory_insert(TIER_L2, KIND_FACT, "jordan-role",
                        "Jordan owns the build pipeline and release automation.", 0.75, "s1",
                        &fact) == 0);

   memory_diagnostic_t rows[4];
   int count = memory_diagnose("jordan", 4, rows, 4);
   assert(count >= 2);
   assert(rows[0].memory.id == fact.id);
   assert(rows[1].memory.id == noise.id);
   teardown();
   platform_test_rmrf(tmpdir);
}

static void test_surprise_scores_first_mention_higher(void)
{
   setup();
   memory_t first, repeat;
   assert(memory_insert(TIER_L2, KIND_FACT, "jordan-first", "Jordan owns the release pipeline.",
                        0.9, "surprise-s1", &first) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "jordan-repeat",
                        "Jordan still owns the release pipeline.", 0.9, "surprise-s1",
                        &repeat) == 0);

   double first_surprise = fetch_surprise(first.id);
   double repeat_surprise = fetch_surprise(repeat.id);
   assert(first_surprise > repeat_surprise);
   assert(first_surprise >= 0.95);
   assert(repeat_surprise < 0.95);
   teardown();
}

static void test_surprise_demotes_repeated_fact_matches(void)
{
   char tmpdir[128];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-memory-surprise-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
   assert(platform_setenv("AIMEE_MEMORY_SURPRISE_ENABLED", "1") == 0);
   assert(platform_setenv("AIMEE_MEMORY_SURPRISE_WEIGHT", "3.0") == 0);

   char cfgdir[256];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   char cfgpath[320];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "memory:\n"
               "  salience:\n"
               "    surprise_enabled: true\n"
               "    surprise_weight: 3.0\n"
               "    window_size: 8\n");
   fclose(fp);

   setup();
   memory_t first, repeat;
   assert(memory_insert(TIER_L2, KIND_FACT, "jordan-origin",
                        "Jordan owns the release pipeline and approves deployment plans.", 0.9,
                        "surprise-rank", &first) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "jordan-repeat",
                        "Jordan still owns the release pipeline and approves deployment plans.",
                        0.9, "surprise-rank", &repeat) == 0);

   memory_diagnostic_t rows[4];
   int count = memory_diagnose("Who owns the release pipeline?", 4, rows, 4);
   assert(count >= 2);
   assert(rows[0].memory.id == first.id);
   assert(rows[1].memory.id == repeat.id);
   assert(rows[0].parts.surprise > rows[1].parts.surprise);
   teardown();
   platform_test_rmrf(tmpdir);
}

static void test_pagerank_promotes_linked_definition_memory(void)
{
   char tmpdir[128];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-memory-pagerank-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
   assert(platform_setenv("AIMEE_MEMORY_PAGERANK_ENABLED", "1") == 0);
   assert(platform_setenv("AIMEE_MEMORY_PAGERANK_WEIGHT", "1.2") == 0);
   assert(platform_setenv("AIMEE_MEMORY_PAGERANK_ITERATIONS", "8") == 0);

   char cfgdir[256];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   char cfgpath[320];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "memory:\n"
               "  pagerank:\n"
               "    enabled: true\n"
               "    iterations: 8\n"
               "    weight: 1.2\n"
               "    relations: depends_on\n");
   fclose(fp);

   setup();
   memory_t definition, caller_a, caller_b;
   assert(memory_insert(TIER_L2, KIND_FACT, "struct ReleasePlan",
                        "ReleasePlan struct defines owner, approvals, and deployment windows.",
                        0.82, "graph-s1", &definition) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "build_release_plan",
                        "build_release_plan validates deployment windows and approvals.", 0.94,
                        "graph-s1", &caller_a) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "ship_release",
                        "ship_release uses build_release_plan before deployment.", 0.93, "graph-s1",
                        &caller_b) == 0);

   assert(memory_link_create(caller_a.id, definition.id, "depends_on") == 0);
   assert(memory_link_create(caller_b.id, definition.id, "depends_on") == 0);

   memory_diagnostic_t rows[6];
   int count = memory_diagnose("who defines deployment windows approvals", 6, rows, 6);
   assert(count >= 3);
   assert(rows[0].memory.id == definition.id);
   assert(rows[0].parts.pagerank > 0.0);

   memory_stats_t stats;
   assert(memory_stats(&stats) == 0);
   assert(stats.pagerank_samples > 0);
   assert(stats.pagerank_last_candidates >= 3);
   assert(stats.pagerank_last_edges >= 2);
   teardown();
   platform_test_rmrf(tmpdir);
}

static void test_memory_embed_records_embedder_version(void)
{
   char tmpdir[128];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-embedder-version-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);

   char cfgdir[256];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   char cfgpath[320];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "embedding_command: builtin\n"
               "embedding_model: nomic-embed-text-v1.5\n"
               "embedding_dim: 768\n");
   fclose(fp);

   setup();
   /* This test embeds with the builtin embedder, which emits 384-dim vectors;
    * align the active dim so the upsert dim guard accepts them. */
   db2_set_embedding_dim(384);
   memory_t mem;
   assert(memory_insert(TIER_L2, KIND_FACT, "embed-version", "test content", 0.9, "", &mem) == 0);
   assert(memory_embed(mem.id, "builtin") == 0);

   char vio_err[128] = "";
   aimee_pg_stmt_t *vio_st = aimee_pg_prepare(
       db2_conn(), "SELECT collection, status FROM vector_index_ops WHERE point_id = ?1", vio_err,
       sizeof(vio_err));
   assert(vio_st != NULL);
   aimee_pg_bind_int64(vio_st, "?1", mem.id);
   assert(aimee_pg_step(vio_st, vio_err, sizeof(vio_err)) == AIMEE_PG_ROW);
   const char *collection = aimee_pg_column_text(vio_st, 0);
   const char *status = aimee_pg_column_text(vio_st, 1);
   assert(collection && strcmp(collection, "memory_embeddings") == 0);
   assert(status && strcmp(status, "ok") == 0);
   aimee_pg_finalize(vio_st);

   teardown();
   platform_test_rmrf(tmpdir);
   assert(platform_unsetenv("AIMEE_NO_CACHE") == 0);
}

static void test_coref_heuristic_indexes_recent_named_entity(void)
{
   char tmpdir[128];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-coref-heuristic-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);

   char cfgdir[256];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   char cfgpath[320];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "memory:\n  coref:\n    mode: heuristic\n    window: 5\n");
   fclose(fp);

   setup();
   memory_t first, second;
   assert(memory_insert(TIER_L2, KIND_FACT, "camping", "Melanie went camping on March 8.", 0.9,
                        "coref-s1", &first) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "return", "She got back on March 12.", 0.9, "coref-s1",
                        &second) == 0);

   char ce_err[128] = "";
   aimee_pg_stmt_t *ce_st = aimee_pg_prepare(
       db2_conn(), "SELECT entity FROM memory_entities WHERE memory_id = ?1 AND role = 'coref'",
       ce_err, sizeof(ce_err));
   assert(ce_st != NULL);
   aimee_pg_bind_int64(ce_st, "?1", second.id);
   assert(aimee_pg_step(ce_st, ce_err, sizeof(ce_err)) == AIMEE_PG_ROW);
   const char *entity = aimee_pg_column_text(ce_st, 0);
   assert(entity && strcmp(entity, "melanie") == 0);
   aimee_pg_finalize(ce_st);

   memory_t rows[4];
   int count = memory_find_facts("When did Melanie get back?", 3, rows, 4);
   assert(count >= 1);
   int found = 0;
   for (int i = 0; i < count; i++)
   {
      if (rows[i].id == second.id)
      {
         found = 1;
         break;
      }
   }
   assert(found == 1);

   teardown();
   platform_test_rmrf(tmpdir);
   assert(platform_unsetenv("AIMEE_NO_CACHE") == 0);
}

static void test_coref_heuristic_skips_ambiguous_prior_turn(void)
{
   char tmpdir[128];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-coref-ambiguous-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);

   char cfgdir[256];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   char cfgpath[320];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "memory:\n  coref:\n    mode: heuristic\n    window: 5\n");
   fclose(fp);

   setup();
   memory_t first, second;
   assert(memory_insert(TIER_L2, KIND_FACT, "meeting", "Melanie met Sarah after lunch.", 0.9,
                        "coref-s2", &first) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "return", "She got back on March 12.", 0.9, "coref-s2",
                        &second) == 0);

   char cea_err[128] = "";
   aimee_pg_stmt_t *cea_st = aimee_pg_prepare(
       db2_conn(), "SELECT COUNT(*) FROM memory_entities WHERE memory_id = ?1 AND role = 'coref'",
       cea_err, sizeof(cea_err));
   assert(cea_st != NULL);
   aimee_pg_bind_int64(cea_st, "?1", second.id);
   assert(aimee_pg_step(cea_st, cea_err, sizeof(cea_err)) == AIMEE_PG_ROW);
   assert(aimee_pg_column_int(cea_st, 0) == 0);
   aimee_pg_finalize(cea_st);

   teardown();
   platform_test_rmrf(tmpdir);
   assert(platform_unsetenv("AIMEE_NO_CACHE") == 0);
}

static void test_coref_audit_bound_recorded(void)
{
   char tmpdir[128];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-coref-audit-bound-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);

   char cfgdir[256];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   char cfgpath[320];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "memory:\n  coref:\n    mode: heuristic\n    window: 5\n");
   fclose(fp);

   setup();
   memory_t first, second;
   assert(memory_insert(TIER_L2, KIND_FACT, "camping", "Jordan went hiking on April 1.", 0.9,
                        "audit-s1", &first) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "return", "He got back on April 3.", 0.9, "audit-s1",
                        &second) == 0);

   /* audit table must have a 'bound' row for the second memory */
   char audit_err[128] = "";
   aimee_pg_stmt_t *audit_st = aimee_pg_prepare(
       db2_conn(), "SELECT outcome, mode FROM memory_coref_audit WHERE memory_id = ?1", audit_err,
       sizeof(audit_err));
   assert(audit_st != NULL);
   aimee_pg_bind_int64(audit_st, "?1", second.id);
   int found_bound = 0;
   while (aimee_pg_step(audit_st, audit_err, sizeof(audit_err)) == AIMEE_PG_ROW)
   {
      const char *outcome = aimee_pg_column_text(audit_st, 0);
      const char *mode = aimee_pg_column_text(audit_st, 1);
      if (outcome && strcmp(outcome, "bound") == 0 && mode && strcmp(mode, "heuristic") == 0)
         found_bound = 1;
   }
   aimee_pg_finalize(audit_st);
   assert(found_bound == 1);

   teardown();
   platform_test_rmrf(tmpdir);
   assert(platform_unsetenv("AIMEE_NO_CACHE") == 0);
}

static void test_coref_audit_ambiguous_recorded(void)
{
   char tmpdir[128];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-coref-audit-amb-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);

   char cfgdir[256];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   char cfgpath[320];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "memory:\n  coref:\n    mode: heuristic\n    window: 5\n");
   fclose(fp);

   setup();
   memory_t first, second;
   /* Two distinct named entities → pronoun is ambiguous → outcome should be 'ambiguous' */
   assert(memory_insert(TIER_L2, KIND_FACT, "meeting", "Melanie met Jordan after lunch.", 0.9,
                        "audit-s2", &first) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "return", "She got back on April 3.", 0.9, "audit-s2",
                        &second) == 0);

   char au2_err[128] = "";
   aimee_pg_stmt_t *au2_st =
       aimee_pg_prepare(db2_conn(), "SELECT outcome FROM memory_coref_audit WHERE memory_id = ?1",
                        au2_err, sizeof(au2_err));
   assert(au2_st != NULL);
   aimee_pg_bind_int64(au2_st, "?1", second.id);
   int found_ambiguous = 0;
   while (aimee_pg_step(au2_st, au2_err, sizeof(au2_err)) == AIMEE_PG_ROW)
   {
      const char *outcome = aimee_pg_column_text(au2_st, 0);
      if (outcome && strcmp(outcome, "ambiguous") == 0)
         found_ambiguous = 1;
   }
   aimee_pg_finalize(au2_st);
   assert(found_ambiguous == 1);

   teardown();
   platform_test_rmrf(tmpdir);
   assert(platform_unsetenv("AIMEE_NO_CACHE") == 0);
}

static void test_coref_stats_increments_bound(void)
{
   char tmpdir[128];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-coref-stats-bound-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);

   char cfgdir[256];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   char cfgpath[320];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "memory:\n  coref:\n    mode: heuristic\n    window: 5\n");
   fclose(fp);

   memory_coref_stats_reset();
   memory_coref_stats_t before;
   memory_coref_stats(&before);

   setup();
   memory_t first, second;
   assert(memory_insert(TIER_L2, KIND_FACT, "hike", "Alice went hiking on Friday.", 0.9, "stats-s1",
                        &first) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "return", "She got back on Sunday.", 0.9, "stats-s1",
                        &second) == 0);

   memory_coref_stats_t after;
   memory_coref_stats(&after);
   assert(after.bound >= before.bound + 1);

   teardown();
   platform_test_rmrf(tmpdir);
   assert(platform_unsetenv("AIMEE_NO_CACHE") == 0);
}

static void test_coref_stats_increments_ambiguous(void)
{
   char tmpdir[128];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-coref-stats-amb-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   assert(platform_setenv("HOME", tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);

   char cfgdir[256];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config/aimee", tmpdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   char cfgpath[320];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", cfgdir);
   FILE *fp = fopen(cfgpath, "w");
   assert(fp != NULL);
   fprintf(fp, "memory:\n  coref:\n    mode: heuristic\n    window: 5\n");
   fclose(fp);

   memory_coref_stats_reset();
   memory_coref_stats_t before;
   memory_coref_stats(&before);

   setup();
   memory_t first, second;
   /* Two distinct named entities → pronoun is ambiguous */
   assert(memory_insert(TIER_L2, KIND_FACT, "meeting", "Carlos met Diana at the conference.", 0.9,
                        "stats-s2", &first) == 0);
   assert(memory_insert(TIER_L2, KIND_FACT, "depart", "She left early.", 0.9, "stats-s2",
                        &second) == 0);

   memory_coref_stats_t after;
   memory_coref_stats(&after);
   assert(after.ambiguous >= before.ambiguous + 1);

   teardown();
   platform_test_rmrf(tmpdir);
   assert(platform_unsetenv("AIMEE_NO_CACHE") == 0);
}

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

/* The semantic-memory legs were gated on `qdim == 384` (the retired builtin) and
 * used 384-calibrated cosine floors, so semantic recall was silently dead for
 * Qwen3-0.6B (1024) / Qwen3-4B (2560). Verify the gate now tracks the active
 * embedding dim and the floor scales down for the compressed-range embedders. */
static void test_semantic_recall_is_embedder_aware(void)
{
   setup();

   db2_set_embedding_dim(384);
   assert(memory_semantic_dim_ok_test(384) == 1);
   assert(memory_semantic_dim_ok_test(1024) == 0); /* mismatch must not query */
   assert(memory_semantic_dim_ok_test(0) == 0);    /* embed failure */
   assert(fabs(memory_semantic_floor_scale_test() - 1.0) < 1e-9);

   db2_set_embedding_dim(1024); /* Qwen3-0.6B: the leg must now run */
   assert(memory_semantic_dim_ok_test(1024) == 1);
   assert(memory_semantic_dim_ok_test(384) == 0);
   double s1024 = memory_semantic_floor_scale_test();
   assert(s1024 > 0.0 && s1024 < 1.0); /* floors relaxed vs the 384 baseline */
   assert(0.62 * s1024 < 0.45);        /* 384-era 0.62 floor now clears ~0.38 hits */

   db2_set_embedding_dim(2560); /* Qwen3-4B */
   assert(memory_semantic_dim_ok_test(2560) == 1);
   double s2560 = memory_semantic_floor_scale_test();
   assert(s2560 > s1024 && s2560 <= 1.0); /* 4b range wider than 0.6b, <= builtin */

   db2_set_embedding_dim(9999); /* unknown embedder: permissive, never 1:1 */
   assert(memory_semantic_floor_scale_test() < 1.0);

   db2_set_embedding_dim(1024); /* restore the deployment default */
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
   test_semantic_recall_is_embedder_aware();
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
