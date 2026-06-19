
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

   int rc = memory_fold_session("fold-sess");
   assert(rc == 0);

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
