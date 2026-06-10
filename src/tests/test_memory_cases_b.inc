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
