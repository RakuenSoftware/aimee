#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "db.h"
#include "db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"
#include "kb_service_backend.h"
#include "memory.h"
#include "../modules/db2/c/curiosity.h"
#include "../modules/db2/c/db_postgres.h"
#include "../modules/db2/c/db2_internal.h"

/* Each test block needs the curiosity_items, memories, memory_directives,
 * failed_queries tables on the *same* DB2 connection so the cross-tier
 * paths (sweep_failed_queries, route_top) see the same rows. The DB2
 * test shim helper opens one in-memory backing per block. */
static void setup(void)
{
   db2_test_shim_open();
}

/* Point config at a temp dir this test owns and write the keys the case needs.
 * memory_maintenance_maybe_run reads live config rather than taking a config_t,
 * so a case that does not write its own precondition would silently inherit the
 * developer's real aimee.yaml and stop testing what it names. Uses a per-pid
 * path rather than mkdtemp() on a static buffer — mkdtemp rewrites its XXXXXX
 * template in place, so a second call on the same buffer fails. */
static void write_test_config(const char *yaml)
{
   char dir[256], path[320];
   snprintf(dir, sizeof(dir), "/tmp/aimee-curiosity-cfg-%d", (int)getpid());
   mkdir(dir, 0755);
   setenv("AIMEE_HOME", dir, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);
   snprintf(path, sizeof(path), "%s/aimee.yaml", dir);
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(yaml, f);
   fclose(f);
}

int main(void)
{
   printf("curiosity: ");

   /* DB1 is still required by the maintenance cycle (maintenance_state). */
   assert(db1_init(":memory:") == 0);

   /* --- canonicality + state validity --- */
   {
      assert(db2_curiosity_gap_type_is_canonical(CURIOSITY_GAP_MISSING_FACT));
      assert(db2_curiosity_gap_type_is_canonical(CURIOSITY_GAP_CONTRADICTION));
      assert(db2_curiosity_gap_type_is_canonical(CURIOSITY_GAP_STALE_FACT));
      assert(db2_curiosity_gap_type_is_canonical(CURIOSITY_GAP_WEAK_COVERAGE));
      assert(db2_curiosity_gap_type_is_canonical(CURIOSITY_GAP_UNVERIFIED_ASSUMPTION));
      assert(!db2_curiosity_gap_type_is_canonical("bogus"));
      assert(!db2_curiosity_gap_type_is_canonical(""));
      assert(!db2_curiosity_gap_type_is_canonical(NULL));

      assert(db2_curiosity_state_is_valid(CURIOSITY_STATE_OPEN));
      assert(db2_curiosity_state_is_valid(CURIOSITY_STATE_IN_PROGRESS));
      assert(db2_curiosity_state_is_valid(CURIOSITY_STATE_RESOLVED));
      assert(db2_curiosity_state_is_valid(CURIOSITY_STATE_SUPPRESSED));
      assert(!db2_curiosity_state_is_valid("archived"));
   }

   /* --- create + get + list round-trip --- */
   {
      setup();
      db2_curiosity_reset();
      curiosity_item_t created;
      assert(db2_curiosity_create(CURIOSITY_GAP_WEAK_COVERAGE, "Alice", "biography",
                                  "only two facts", 0.4, 0.6, "", &created) == 0);
      assert(created.id > 0);
      assert(strcmp(created.gap_type, CURIOSITY_GAP_WEAK_COVERAGE) == 0);
      assert(strcmp(created.state, CURIOSITY_STATE_OPEN) == 0);

      curiosity_item_t got;
      assert(db2_curiosity_get(created.id, &got) == 1);
      assert(strcmp(got.target_entity, "Alice") == 0);
      assert(strcmp(got.target_topic, "biography") == 0);
      assert(got.importance > 0.39 && got.importance < 0.41);

      curiosity_item_t rows[8];
      int n = db2_curiosity_list(CURIOSITY_STATE_OPEN, rows, 8);
      assert(n == 1);
      assert(rows[0].id == created.id);
   }

   /* --- unknown gap_type rejected --- */
   {
      setup();
      db2_curiosity_reset();
      assert(db2_curiosity_create("not_a_gap_type", "", "something", "", 0, 0, "", NULL) == -1);
      assert(db2_curiosity_create("", "", "something", "", 0, 0, "", NULL) == -1);
   }

   /* --- state transitions --- */
   {
      setup();
      db2_curiosity_reset();
      curiosity_item_t c;
      assert(db2_curiosity_create(CURIOSITY_GAP_CONTRADICTION, "Bob", "role", "", 0, 0, "", &c) ==
             0);
      assert(db2_curiosity_update_state(c.id, CURIOSITY_STATE_IN_PROGRESS) == 0);
      curiosity_item_t got;
      assert(db2_curiosity_get(c.id, &got) == 1);
      assert(strcmp(got.state, CURIOSITY_STATE_IN_PROGRESS) == 0);

      assert(db2_curiosity_update_state(c.id, CURIOSITY_STATE_RESOLVED) == 0);
      assert(db2_curiosity_get(c.id, &got) == 1);
      assert(strcmp(got.state, CURIOSITY_STATE_RESOLVED) == 0);

      /* unknown state rejected */
      assert(db2_curiosity_update_state(c.id, "mystery") == -1);
   }

   /* --- state filter: list returns only matching rows --- */
   {
      setup();
      db2_curiosity_reset();
      curiosity_item_t a, b;
      db2_curiosity_create(CURIOSITY_GAP_MISSING_FACT, "", "alpha-topic", "", 0, 0, "", &a);
      db2_curiosity_create(CURIOSITY_GAP_STALE_FACT, "Carol", "", "", 0, 0, "", &b);
      db2_curiosity_update_state(b.id, CURIOSITY_STATE_SUPPRESSED);

      curiosity_item_t rows[4];
      assert(db2_curiosity_list(CURIOSITY_STATE_OPEN, rows, 4) == 1);
      assert(rows[0].id == a.id);
      assert(db2_curiosity_list(CURIOSITY_STATE_SUPPRESSED, rows, 4) == 1);
      assert(rows[0].id == b.id);
      assert(db2_curiosity_list(NULL, rows, 4) == 2);
   }

   /* --- dedup: creating an open missing_fact twice for the same topic
    *     should fail on the second attempt because of the partial
    *     unique index --- */
   {
      setup();
      db2_curiosity_reset();
      assert(db2_curiosity_create(CURIOSITY_GAP_MISSING_FACT, "", "dedupe-me", "first", 0, 0, "",
                                  NULL) == 0);
      assert(db2_curiosity_create(CURIOSITY_GAP_MISSING_FACT, "", "dedupe-me", "second", 0, 0, "",
                                  NULL) == -1);
      /* But resolving the first one frees up the slot. */
      curiosity_item_t rows[4];
      int n = db2_curiosity_list(CURIOSITY_STATE_OPEN, rows, 4);
      assert(n == 1);
      db2_curiosity_update_state(rows[0].id, CURIOSITY_STATE_RESOLVED);
      assert(db2_curiosity_create(CURIOSITY_GAP_MISSING_FACT, "", "dedupe-me", "third", 0, 0, "",
                                  NULL) == 0);
   }

   /* --- sweep failed_queries: creates items, idempotent on rerun --- */
   {
      setup();
      db2_curiosity_reset();
      /* Seed failed_queries (it's part of the baseline schema). */
      const char *seed = "INSERT INTO failed_queries (query_norm, failure_count) VALUES"
                         " ('where does alice live', 3),"
                         " ('what did bob eat last tuesday', 1)";
      char seed_err[128] = {0};
      assert(aimee_pg_exec(db2_conn(), seed, seed_err, sizeof(seed_err)) == 0);

      assert(db2_curiosity_sweep_failed_queries() == 2);
      curiosity_item_t rows[8];
      int n = db2_curiosity_list(CURIOSITY_STATE_OPEN, rows, 8);
      assert(n == 2);
      int seen_alice = 0, seen_bob = 0;
      for (int i = 0; i < n; i++)
      {
         assert(strcmp(rows[i].gap_type, CURIOSITY_GAP_MISSING_FACT) == 0);
         if (strstr(rows[i].target_topic, "alice"))
            seen_alice++;
         if (strstr(rows[i].target_topic, "bob"))
            seen_bob++;
      }
      assert(seen_alice == 1);
      assert(seen_bob == 1);

      /* Rerunning the sweep without changing failed_queries is a no-op
       * because the dedup index blocks re-insertion. */
      assert(db2_curiosity_sweep_failed_queries() == 0);
   }

   /* --- reset wipes everything --- */
   {
      setup();
      db2_curiosity_reset();
      db2_curiosity_create(CURIOSITY_GAP_MISSING_FACT, "", "gone", "", 0, 0, "", NULL);
      assert(db2_curiosity_reset() == 0);
      curiosity_item_t rows[4];
      assert(db2_curiosity_list(NULL, rows, 4) == 0);
   }

   /* --- rescore: populates scoring columns for open items --- */
   {
      setup();
      db2_curiosity_reset();
      curiosity_item_t a, b, c;
      db2_curiosity_create(CURIOSITY_GAP_CONTRADICTION, "Eve", "", "two facts conflict", 0, 0, "",
                           &a);
      db2_curiosity_create(CURIOSITY_GAP_MISSING_FACT, "", "weather last tuesday", "", 0, 0, "",
                           &b);
      db2_curiosity_create(CURIOSITY_GAP_WEAK_COVERAGE, "Frank", "", "only one fact", 0, 0, "", &c);

      int rescored = db2_curiosity_rescore_all();
      assert(rescored == 3);

      curiosity_item_t rows[8];
      int n = db2_curiosity_list(CURIOSITY_STATE_OPEN, rows, 8);
      assert(n == 3);
      for (int i = 0; i < n; i++)
      {
         /* Every column should be populated and in [0, 1]. */
         assert(rows[i].importance >= 0.0 && rows[i].importance <= 1.0);
         assert(rows[i].novelty >= 0.0 && rows[i].novelty <= 1.0);
         assert(rows[i].progress >= 0.0 && rows[i].progress <= 1.0);
         assert(rows[i].routing_score >= 0.0 && rows[i].routing_score <= 1.0);
      }
      /* Contradiction with evidence should outrank weak_coverage with
       * evidence in importance, since the gap-type base weight is
       * higher. */
      double contradiction_importance = 0.0;
      double weak_coverage_importance = 0.0;
      for (int i = 0; i < n; i++)
      {
         if (strcmp(rows[i].gap_type, CURIOSITY_GAP_CONTRADICTION) == 0)
            contradiction_importance = rows[i].importance;
         else if (strcmp(rows[i].gap_type, CURIOSITY_GAP_WEAK_COVERAGE) == 0)
            weak_coverage_importance = rows[i].importance;
      }
      assert(contradiction_importance > weak_coverage_importance);
   }

   /* --- routing: top-N open items become directives and transition
    *     to in_progress --- */
   {
      setup();
      db2_curiosity_reset();
      db2_curiosity_create(CURIOSITY_GAP_CONTRADICTION, "Eve", "role", "", 0, 0, "", NULL);
      db2_curiosity_create(CURIOSITY_GAP_MISSING_FACT, "", "weather", "", 0, 0, "", NULL);
      db2_curiosity_create(CURIOSITY_GAP_WEAK_COVERAGE, "Frank", "", "", 0, 0, "", NULL);

      int rescored = db2_curiosity_rescore_all();
      assert(rescored == 3);

      /* curiosity_route_top moved to the kb-side backend in #1038. The
       * route_top JSON helper exercises the same logic directly without
       * round-tripping through the kb_client RPC layer. */
      cJSON *route_resp = db2_kb_service_curiosity_route_top_json(2, "");
      assert(route_resp);
      cJSON *routed_j = cJSON_GetObjectItemCaseSensitive(route_resp, "routed");
      assert(cJSON_IsNumber(routed_j) && (int)routed_j->valuedouble == 2);
      cJSON_Delete(route_resp);

      /* Two items should be in_progress, one still open. */
      curiosity_item_t rows[8];
      int in_progress = db2_curiosity_list(CURIOSITY_STATE_IN_PROGRESS, rows, 8);
      int open = db2_curiosity_list(CURIOSITY_STATE_OPEN, rows, 8);
      assert(in_progress == 2);
      assert(open == 1);

      /* At least one directive was written for the top items. */
      char count_err[128] = {0};
      aimee_pg_stmt_t *q = aimee_pg_prepare(db2_conn(), "SELECT COUNT(*) FROM epistemic_directives",
                                            count_err, sizeof(count_err));
      assert(q);
      assert(aimee_pg_step(q, count_err, sizeof(count_err)) == AIMEE_PG_ROW);
      int dir_count = aimee_pg_column_int(q, 0);
      aimee_pg_finalize(q);
      assert(dir_count >= 2);

      memory_directive_t dirs[8];
      int dn = memory_directive_list(MEMORY_DIRECTIVE_STATE_OPEN, NULL, dirs, 8);
      assert(dn == 2);
      int saw_contradiction = 0;
      int saw_retrieval_failure = 0;
      for (int i = 0; i < dn; i++)
      {
         if (strcmp(dirs[i].topic, "role") == 0)
         {
            assert(strcmp(dirs[i].cause, MEMORY_DIRECTIVE_CAUSE_CONTRADICTION) == 0);
            assert(strstr(dirs[i].question, "Resolve contradiction about role") != NULL);
            saw_contradiction = 1;
         }
         else if (strcmp(dirs[i].topic, "weather") == 0)
         {
            assert(strcmp(dirs[i].cause, MEMORY_DIRECTIVE_CAUSE_RETRIEVAL_FAILURE) == 0);
            assert(strstr(dirs[i].question, "Find a fact to answer: weather") != NULL);
            saw_retrieval_failure = 1;
         }
      }
      assert(saw_contradiction);
      assert(saw_retrieval_failure);

      /* Rerunning route is idempotent because the items are no
       * longer open. */
      cJSON *route_resp2 = db2_kb_service_curiosity_route_top_json(2, "");
      assert(route_resp2);
      cJSON *routed2_j = cJSON_GetObjectItemCaseSensitive(route_resp2, "routed");
      assert(cJSON_IsNumber(routed2_j) && (int)routed2_j->valuedouble == 1);
      cJSON_Delete(route_resp2);
   }

   /* --- scoring: maturity shifts routing from dense-known gaps toward
    *     frontier gaps as the corpus grows --- */
   {
      setup();
      db2_curiosity_reset();
      assert(db2_curiosity_create(CURIOSITY_GAP_CONTRADICTION, "", "known-topic", "", 0, 0, "",
                                  NULL) == 0);
      assert(db2_curiosity_create(CURIOSITY_GAP_WEAK_COVERAGE, "", "frontier-topic",
                                  "single weak hint", 0, 0, "", NULL) == 0);

      memory_t seeded;
      for (int i = 0; i < 8; i++)
      {
         char key[64];
         char content[128];
         snprintf(key, sizeof(key), "known-topic:%d", i);
         snprintf(content, sizeof(content), "known-topic evidence %d", i);
         memory_insert(TIER_L2, KIND_FACT, key, content, 0.8, "s1", &seeded);
      }

      assert(db2_curiosity_rescore_all() == 2);
      curiosity_item_t rows[4];
      int n = db2_curiosity_list(CURIOSITY_STATE_OPEN, rows, 4);
      assert(n == 2);
      double early_known = 0.0;
      double early_frontier = 0.0;
      for (int i = 0; i < n; i++)
      {
         if (strcmp(rows[i].target_topic, "known-topic") == 0)
            early_known = rows[i].routing_score;
         else if (strcmp(rows[i].target_topic, "frontier-topic") == 0)
            early_frontier = rows[i].routing_score;
      }
      assert(early_known > early_frontier);

      memory_t bulk;
      for (int i = 0; i < 300; i++)
      {
         char key[64];
         char content[64];
         snprintf(key, sizeof(key), "bulk:%d", i);
         snprintf(content, sizeof(content), "background memory %d", i);
         memory_insert(TIER_L2, KIND_FACT, key, content, 0.6, "s1", &bulk);
      }

      assert(db2_curiosity_rescore_all() == 2);
      n = db2_curiosity_list(CURIOSITY_STATE_OPEN, rows, 4);
      assert(n == 2);
      double late_known = 0.0;
      double late_frontier = 0.0;
      for (int i = 0; i < n; i++)
      {
         if (strcmp(rows[i].target_topic, "known-topic") == 0)
            late_known = rows[i].routing_score;
         else if (strcmp(rows[i].target_topic, "frontier-topic") == 0)
            late_frontier = rows[i].routing_score;
      }
      assert(late_frontier > late_known);
   }

   /* --- acceptance 5: curiosity routing does not regress recall.
    *     memory_find_facts output before and after a route must match. */
   {
      setup();
      db2_curiosity_reset();
      /* Seed a small corpus that the hybrid retrieval path can hit
       * without pgvector (aggregation fallback + lexical). */
      memory_t m1, m2;
      memory_insert(TIER_L2, KIND_FACT, "alice:role", "Alice is the project lead", 0.9, "s1", &m1);
      memory_insert(TIER_L2, KIND_FACT, "bob:role", "Bob writes documentation", 0.9, "s1", &m2);
      /* memory_find_facts may return -1 when pgvector is unavailable
       * (which it is in a test :memory: db). That's fine for the
       * regression check — we just need identical outputs pre/post. */

      memory_t pre[8];
      int pre_n = memory_find_facts("alice lead", 5, pre, 8);

      /* Build a curiosity backlog and route it. */
      db2_curiosity_create(CURIOSITY_GAP_MISSING_FACT, "", "alice pet", "", 0, 0, "", NULL);
      db2_curiosity_create(CURIOSITY_GAP_CONTRADICTION, "Bob", "hobby", "two mentions differ", 0, 0,
                           "", NULL);
      db2_curiosity_rescore_all();
      cJSON *route_resp3 = db2_kb_service_curiosity_route_top_json(4, "");
      cJSON_Delete(route_resp3);

      memory_t post[8];
      int post_n = memory_find_facts("alice lead", 5, post, 8);

      /* memory_find_facts is read-only w.r.t. the corpus that
       * matters here. Routing creates directives (separate table),
       * curiosity items (separate table), and updates curiosity_items
       * row state — none of which touch the memories / memory_units
       * tables that memory_find_facts reads. The identity-of-results
       * assertion catches any future change that accidentally couples
       * routing to retrieval. */
      assert(pre_n == post_n);
      for (int i = 0; i < pre_n && i < post_n; i++)
         assert(pre[i].id == post[i].id);
   }

   /* --- maintenance maybe_run rescored curiosity once, then the idle
    *     guard skipped the immediate scheduler hot path --- */
   {
      setup();
      db2_curiosity_reset();
      curiosity_item_t created;
      assert(db2_curiosity_create(CURIOSITY_GAP_MISSING_FACT, "", "scheduler-topic", "", 0, 0, "",
                                  &created) == 0);
      assert(created.routing_score == 0.0);

      write_test_config("memory_maintenance:\n  enabled: true\n  interval_seconds: 3600\n");

      memory_maintenance_summary_t first;
      memset(&first, 0, sizeof(first));
      assert(memory_maintenance_maybe_run(&first) == 1);
      assert(first.skipped == 0);
      assert(first.rescored == 1);

      curiosity_item_t rescored;
      assert(db2_curiosity_get(created.id, &rescored) == 1);
      assert(rescored.routing_score > 0.0);

      memory_maintenance_summary_t second;
      memset(&second, 0, sizeof(second));
      assert(memory_maintenance_maybe_run(&second) == 0);
      assert(second.skipped == 1);
      assert(second.rescored == 0);
   }

   db1_shutdown();
   printf("all tests passed\n");
   return 0;
}
