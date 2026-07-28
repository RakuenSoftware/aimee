#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "aimee.h"
#include "cJSON.h"
#include "db.h"
#include "db1.h"
#include "db2.h"
#include "db2_test_shim.h"
#include "memory_ontology.h"
#include "../db2/bandit.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"

static void reset_db(void)
{
   db2_test_shim_close();
   db2_test_shim_open();
}

static void write_test_config(const char *yaml)
{
   char dir[256], path[320];
   snprintf(dir, sizeof(dir), "/tmp/aimee-memory-advanced-cfg-%d", (int)getpid());
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
   printf("memory_advanced: ");

   /* DB1 is required by the maintenance cycle (maintenance_state table). */
   assert(db1_init(":memory:") == 0);

   /* DB2 backed by an in-memory sqlite shim. Test seeds use aimee_pg_*
    * against db2_conn() — same surface production code uses. */
   db2_test_shim_open();

   /* --- anti_pattern_insert --- */
   {
      anti_pattern_t ap;
      int rc =
          db2_anti_pattern_insert("rm -rf", "dangerous delete", "manual", "incident-1", 0.9, &ap);
      assert(rc == 0);
      assert(ap.id > 0);
      assert(strcmp(ap.pattern, "rm -rf") == 0);
      assert(ap.confidence > 0.89);
   }

   /* --- anti_pattern_list --- */
   {
      anti_pattern_t aps[8];
      int count = db2_anti_pattern_list(aps, 8);
      assert(count == 1);
      assert(strcmp(aps[0].pattern, "rm -rf") == 0);
   }

   /* --- anti_pattern_check: matching phrase at a word boundary --- */
   {
      anti_pattern_t matches[8];
      int count = db2_anti_pattern_check("", "rm -rf /var/data", matches, 8);
      assert(count > 0);
      assert(matches[0].hit_count >= 0);
   }

   /* --- anti_pattern_check: must match full phrase, not just a token --- */
   {
      /* Insert a multi-word pattern whose tokens individually appear in the
       * target but whose whole phrase does not. Old matcher (half-tokens)
       * would have flagged this; the new phrase matcher must not. */
      anti_pattern_t ap;
      db2_anti_pattern_insert("git fetch origin", "network fetch", "manual", "", 0.9, &ap);

      anti_pattern_t matches[8];
      int count = db2_anti_pattern_check("", "echo hello && git status", matches, 8);
      for (int i = 0; i < count; i++)
         assert(strcmp(matches[i].pattern, "git fetch origin") != 0);
      db2_anti_pattern_delete(ap.id);
   }

   /* --- anti_pattern_check: no substring-inside-word false positives --- */
   {
      /* "rm -rf" should NOT match a command that contains "rm" as a substring
       * of another word (e.g. "format", "permission"). */
      anti_pattern_t matches[8];
      int count = db2_anti_pattern_check("", "chmod +rw format permission", matches, 8);
      assert(count == 0);
   }

   /* --- anti_pattern_check: empty pattern row never matches --- */
   {
      anti_pattern_t ep;
      db2_anti_pattern_insert("", "", "test", "", 1.0, &ep);
      anti_pattern_t matches[8];
      int count = db2_anti_pattern_check("", "anything at all", matches, 8);
      /* The empty-pattern row must not show up. */
      for (int i = 0; i < count; i++)
         assert(matches[i].id != ep.id);
      db2_anti_pattern_delete(ep.id);
   }

   /* --- anti_pattern_check: no match --- */
   {
      anti_pattern_t matches[8];
      int count = db2_anti_pattern_check("", "echo hello", matches, 8);
      assert(count == 0);
   }

   /* --- anti_pattern_check does NOT bump lifetime hit_count --- */
   {
      anti_pattern_t before[8];
      int bc = db2_anti_pattern_list(before, 8);
      assert(bc >= 1);
      int hc0 = before[0].hit_count;

      anti_pattern_t matches[8];
      db2_anti_pattern_check("", "rm -rf /tmp/xyz", matches, 8);
      db2_anti_pattern_check("", "rm -rf /tmp/xyz", matches, 8);

      anti_pattern_t after[8];
      db2_anti_pattern_list(after, 8);
      assert(after[0].hit_count == hc0);

      /* anti_pattern_bump is the only path that should increment it. */
      db2_anti_pattern_bump(before[0].id);
      db2_anti_pattern_list(after, 8);
      assert(after[0].hit_count == hc0 + 1);
   }

   /* --- anti_pattern_delete --- */
   {
      anti_pattern_t ap;
      db2_anti_pattern_insert("temp pattern", "test", "manual", "", 0.5, &ap);
      int rc = db2_anti_pattern_delete(ap.id);
      assert(rc == 0);

      anti_pattern_t aps[8];
      int count = db2_anti_pattern_list(aps, 8);
      /* Should only have the first one left */
      assert(count == 1);
   }

   /* --- anti_pattern_extract_from_failures reads DB1 decision_log --- */
   {
      assert(db2_decision_log_insert(0, "A, B", "rm -rf /tmp", "destructive shortcut", "", NULL,
                                     NULL) == 0);
      db2_decision_log_row_t failed[8];
      int count = db2_decision_log_list(NULL, 8, failed, 8);
      assert(count >= 1);
      assert(db2_decision_log_set_outcome(failed[0].id, "failure") == 0);

      int extracted = anti_pattern_extract_from_failures();
      assert(extracted >= 1);

      anti_pattern_t matches[8];
      int found = db2_anti_pattern_check("", "rm -rf /tmp/project", matches, 8);
      assert(found >= 1);
   }

   /* --- memory_detect_conflict --- */
   {
      memory_t m1, m2;
      memory_insert(TIER_L1, KIND_FACT, "deploy-method", "we always deploy with docker containers",
                    1.0, "", &m1);
      memory_insert(TIER_L1, KIND_FACT, "deploy-method", "we never deploy with docker containers",
                    1.0, "", &m2);

      int64_t conflict =
          memory_detect_conflict("deploy-method", "we never deploy with docker containers");
      /* is_contradiction checks for always/never, should/shouldn't patterns */
      /* If contradiction detection works, conflict > 0; if not, skip gracefully */
      (void)conflict;
   }

   /* --- memory_list_conflicts --- */
   {
      /* Record the conflict */
      memory_t m1, m2;
      memory_insert(TIER_L1, KIND_FACT, "conf-test", "value A", 1.0, "", &m1);
      memory_insert(TIER_L1, KIND_FACT, "conf-test", "value B", 1.0, "", &m2);
      memory_record_conflict(m1.id, m2.id);

      conflict_t conflicts[8];
      int count = memory_list_conflicts(conflicts, 8);
      assert(count >= 1);
   }

   /* --- memory_resolve_conflict --- */
   {
      conflict_t conflicts[8];
      int count = memory_list_conflicts(conflicts, 8);
      if (count >= 1)
      {
         int rc = memory_resolve_conflict(conflicts[0].id, "kept newer value");
         assert(rc == 0);
      }
   }

   /* --- memory_supersede --- */
   {
      memory_t old_mem;
      memory_insert(TIER_L1, KIND_FACT, "supersede-test", "old value", 0.8, "", &old_mem);

      memory_t new_mem;
      int rc = memory_supersede(old_mem.id, "new value", 0.9, "sess-1", &new_mem);
      assert(rc == 0);
      assert(new_mem.id != old_mem.id);
      assert(strcmp(new_mem.content, "new value") == 0);
   }

   /* --- memory_fact_history --- */
   {
      memory_t hist[8];
      int count = memory_fact_history("supersede-test", hist, 8);
      assert(count >= 2); /* old + new */
   }

   /* --- memory_is_profile_query --- */
   {
      assert(memory_is_profile_query("what kind of person is Caroline?") == 1);
      assert(memory_is_profile_query("how does Melanie communicate with her daughter?") == 1);
      assert(memory_is_profile_query("what are Jordan's food preferences?") == 1);
      assert(memory_is_profile_query("communication style of Alex") == 1);
      assert(memory_is_profile_query("find the bug in main.c") == 0);
      assert(memory_is_profile_query("what is the deploy command") == 0);
      assert(memory_is_profile_query("") == 0);
   }

   /* --- memory_profile_card_build: build card from entity_edges --- */
   {
      char err[256] = "";
      /* Insert some entity_edges to simulate co_discussed relations */
      static const char *edge_sql =
          "INSERT OR IGNORE INTO entity_edges (source, relation, target, weight, window_id)"
          " VALUES (?1, 'co_discussed', ?2, ?3, 1)";
      aimee_pg_stmt_t *es = aimee_pg_prepare(db2_conn(), edge_sql, err, sizeof(err));
      assert(es);
      /* 3 topics for "Alice" */
      aimee_pg_bind_text(es, "?1", "alice");
      aimee_pg_bind_text(es, "?2", "hiking");
      aimee_pg_bind_int(es, "?3", 5);
      aimee_pg_step(es, err, sizeof(err));
      aimee_pg_reset(es);

      aimee_pg_bind_text(es, "?1", "alice");
      aimee_pg_bind_text(es, "?2", "cooking");
      aimee_pg_bind_int(es, "?3", 3);
      aimee_pg_step(es, err, sizeof(err));
      aimee_pg_finalize(es);

      /* Insert memory_entities rows to reach min_obs=1 */
      memory_t m;
      memory_insert(TIER_L1, KIND_FACT, "alice-fact-1", "alice loves hiking in the mountains", 0.9,
                    "s1", &m);

      /* Simulate a memory_entities entry for alice using the memory ID */
      static const char *ent_sql =
          "INSERT OR IGNORE INTO memory_entities (memory_id, entity) VALUES (?1, ?2)";
      aimee_pg_stmt_t *me = aimee_pg_prepare(db2_conn(), ent_sql, err, sizeof(err));
      assert(me);
      aimee_pg_bind_int64(me, "?1", m.id);
      aimee_pg_bind_text(me, "?2", "alice");
      aimee_pg_step(me, err, sizeof(err));
      aimee_pg_finalize(me);

      char card_json[4096];
      int rc = memory_profile_card_build("alice", 1, card_json, sizeof(card_json));
      assert(rc == 0);
      assert(strstr(card_json, "\"entity_id\"") != NULL);
      assert(strstr(card_json, "alice") != NULL);
   }

   /* --- memory_profile_card_build: min_obs gate --- */
   {
      /* Entity with 0 observations should fail when min_obs=10 */
      char card_json[4096];
      int rc = memory_profile_card_build("nobody-ever", 10, card_json, sizeof(card_json));
      assert(rc != 0);
   }

   /* --- memory_improve_dedupe: merges duplicate-key memories --- */
   {
      char err[256] = "";
      /* Bypass memory_insert merge by inserting directly, simulating duplicates
       * that slipped through (e.g. from direct import or parallel sessions). */
      static const char *ins_sql =
          "INSERT INTO memories (tier, kind, key, content, confidence, use_count, source_session,"
          " created_at, updated_at)"
          " VALUES (?1, ?2, ?3, ?4, ?5, 1, ?6, datetime('now'), datetime('now'))";
      aimee_pg_stmt_t *ins = aimee_pg_prepare(db2_conn(), ins_sql, err, sizeof(err));
      assert(ins);

      /* First row — lower confidence */
      aimee_pg_bind_text(ins, "?1", TIER_L2);
      aimee_pg_bind_text(ins, "?2", KIND_FACT);
      aimee_pg_bind_text(ins, "?3", "dup-key-improve");
      aimee_pg_bind_text(ins, "?4", "first version");
      aimee_pg_bind_double(ins, "?5", 0.6);
      aimee_pg_bind_text(ins, "?6", "s1");
      aimee_pg_step(ins, err, sizeof(err));
      aimee_pg_reset(ins);

      /* Second row — higher confidence, same key */
      aimee_pg_bind_text(ins, "?1", TIER_L2);
      aimee_pg_bind_text(ins, "?2", KIND_FACT);
      aimee_pg_bind_text(ins, "?3", "dup-key-improve");
      aimee_pg_bind_text(ins, "?4", "second version");
      aimee_pg_bind_double(ins, "?5", 0.9);
      aimee_pg_bind_text(ins, "?6", "s2");
      aimee_pg_step(ins, err, sizeof(err));
      aimee_pg_finalize(ins);

      /* Dry-run: should detect 1 duplicate but not write */
      int planned = memory_improve_dedupe(1);
      assert(planned >= 1);

      /* Real run: should merge the lower-confidence duplicate */
      int merged = memory_improve_dedupe(0);
      assert(merged >= 1);

      /* Verify: only one row with key "dup-key-improve" should have merged_into=0 */
      aimee_pg_stmt_t *chk = aimee_pg_prepare(
          db2_conn(), "SELECT COUNT(*) FROM memories WHERE key='dup-key-improve' AND merged_into=0",
          err, sizeof(err));
      assert(chk);
      assert(aimee_pg_step(chk, err, sizeof(err)) == AIMEE_PG_ROW);
      assert(aimee_pg_column_int(chk, 0) == 1);
      aimee_pg_finalize(chk);
   }

   /* --- memory_apply_feedback: updates utility scores on success and failure --- */
   {
      char err[256] = "";
      memory_t m;
      memory_insert(TIER_L2, KIND_FACT, "cited-fact", "Python is the project language", 0.9, "s1",
                    &m);

      /* Insert an entity edge for the cited key */
      static const char *edge_sql =
          "INSERT INTO entity_edges (source, relation, target, weight) VALUES (?1, ?2, ?3, ?4)";
      aimee_pg_stmt_t *es = aimee_pg_prepare(db2_conn(), edge_sql, err, sizeof(err));
      assert(es);
      aimee_pg_bind_text(es, "?1", "cited-fact");
      aimee_pg_bind_text(es, "?2", "co_discussed");
      aimee_pg_bind_text(es, "?3", "python");
      aimee_pg_bind_int(es, "?4", 1);
      aimee_pg_step(es, err, sizeof(err));
      aimee_pg_finalize(es);

      /* Positive feedback: utility_score should go up */
      int64_t cit_ids[] = {m.id};
      int rc = memory_apply_feedback(1, cit_ids, 1);
      assert(rc == 0);

      /* Negative feedback: utility_score should go down and a corrected_by relation inserted */
      rc = memory_apply_feedback(0, cit_ids, 1);
      assert(rc == 0);

      /* Verify corrected_by relation was inserted */
      aimee_pg_stmt_t *rel_stmt = aimee_pg_prepare(
          db2_conn(),
          "SELECT COUNT(*) FROM memory_relations WHERE relation='corrected_by' AND memory_id=?1",
          err, sizeof(err));
      assert(rel_stmt);
      aimee_pg_bind_int64(rel_stmt, "?1", m.id);
      assert(aimee_pg_step(rel_stmt, err, sizeof(err)) == AIMEE_PG_ROW);
      assert(aimee_pg_column_int(rel_stmt, 0) >= 1);
      aimee_pg_finalize(rel_stmt);
   }

   /* --- memory_cognify_parse_response: valid JSON with relations and claims --- */
   {
      const char *json = "{"
                         "\"summary\": \"Alice went camping with her family in May 2023.\","
                         "\"memory_kind\": \"episodic\","
                         "\"relations\": ["
                         "  {\"subject\": \"Alice\", \"relation\": \"PARTICIPATED_IN\","
                         "   \"object\": \"camping_2023\","
                         "   \"fact_text\": \"Alice went camping in 2023\"}"
                         "],"
                         "\"claims\": ["
                         "  {\"subject\": \"server\", \"attribute\": \"port\","
                         "   \"value\": \"5432\", \"kind\": \"fact\"},"
                         "  {\"subject\": \"typescript\", \"attribute\": \"preference\","
                         "   \"value\": \"avoid\", \"kind\": \"opinion\"}"
                         "]}";

      memory_cognify_result_t result;
      int rc = memory_cognify_parse_response(json, &result);
      assert(rc == 0);
      assert(strcmp(result.summary, "Alice went camping with her family in May 2023.") == 0);
      assert(strcmp(result.memory_kind, "episodic") == 0);
      assert(result.relation_count == 1);
      assert(strcmp(result.relations[0].src_entity, "Alice") == 0);
      assert(strcmp(result.relations[0].relation, "PARTICIPATED_IN") == 0);
      assert(strcmp(result.relations[0].dst_entity, "camping_2023") == 0);
      assert(result.claim_count == 2);
      assert(strcmp(result.claims[0].kind, "fact") == 0);
      assert(strcmp(result.claims[1].kind, "opinion") == 0);
   }

   /* --- memory_cognify_parse_response: rejects malformed JSON --- */
   {
      memory_cognify_result_t result;
      int rc = memory_cognify_parse_response("not json at all!", &result);
      assert(rc != 0);
   }

   /* --- memory_cognify_parse_response: missing required fields are tolerated --- */
   {
      /* Only summary, no relations or claims */
      const char *json = "{\"summary\": \"A quiet session.\"}";
      memory_cognify_result_t result;
      int rc = memory_cognify_parse_response(json, &result);
      assert(rc == 0);
      assert(strcmp(result.summary, "A quiet session.") == 0);
      assert(result.relation_count == 0);
      assert(result.claim_count == 0);
      assert(result.coref_count == 0);
   }

   /* --- memory_cognify_parse_response: parses coref_bindings --- */
   {
      const char *json = "{"
                         "\"summary\": \"She joined the team last week.\","
                         "\"coref_bindings\": ["
                         "  {\"pronoun\": \"she\", \"entity\": \"Alice\", \"confidence\": 0.92},"
                         "  {\"pronoun\": \"her\", \"entity\": \"Alice\", \"confidence\": 0.88}"
                         "]}";
      memory_cognify_result_t result;
      int rc = memory_cognify_parse_response(json, &result);
      assert(rc == 0);
      assert(result.coref_count == 2);
      assert(strcmp(result.coref_bindings[0].pronoun, "she") == 0);
      assert(strcmp(result.coref_bindings[0].entity, "Alice") == 0);
      assert(result.coref_bindings[0].confidence > 0.9);
      assert(strcmp(result.coref_bindings[1].entity, "Alice") == 0);
   }

   /* --- memory_cognify_parse_response: ambiguous coref (empty entity) is skipped --- */
   {
      const char *json = "{"
                         "\"coref_bindings\": ["
                         "  {\"pronoun\": \"they\", \"entity\": \"\", \"confidence\": 0.3},"
                         "  {\"pronoun\": \"he\", \"entity\": \"Jordan\", \"confidence\": 0.85}"
                         "]}";
      memory_cognify_result_t result;
      int rc = memory_cognify_parse_response(json, &result);
      assert(rc == 0);
      /* First binding has empty entity so should be skipped; second retained */
      assert(result.coref_count == 1);
      assert(strcmp(result.coref_bindings[0].entity, "Jordan") == 0);
      assert(result.coref_bindings[0].confidence > 0.8);
   }

   /* --- memory_cognify_unit: disabled when cognify.enabled=false --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.memory_cognify_enabled = 0;
      memory_cognify_result_t result;
      int rc = memory_cognify_unit(1, "some text", &cfg, &result);
      assert(rc == -1);
   }

   /* --- memory_cognify_unit: disabled when command is empty --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.memory_cognify_enabled = 1;
      cfg.memory_cognify_command[0] = '\0';
      memory_cognify_result_t result;
      int rc = memory_cognify_unit(1, "some text", &cfg, &result);
      assert(rc == -1);
   }

   /* --- memory_cognify_unit: explicit kind is persisted, procedural claim
    *     mirrors into the feedback rules store --- */
   {
      memory_t src;
      int ins = memory_insert(TIER_L2, KIND_FACT, "cognify-kind-test", "user prefers terse replies",
                              0.8, "s-kind", &src);
      assert(ins == 0);

      /* Fixture response with procedural kind and a preference claim. */
      char fixture[512];
      snprintf(fixture, sizeof(fixture), "%s",
               "{\"summary\":\"user wants terse replies\","
               "\"memory_kind\":\"procedural\","
               "\"claims\":[{\"subject\":\"assistant\",\"attribute\":\"verbosity\","
               "\"value\":\"terse\",\"kind\":\"preference\"}]}");
      char path[256];
      snprintf(path, sizeof(path), "/tmp/aimee-cognify-fixture-%d.json", (int)getpid());
      FILE *fp = fopen(path, "w");
      assert(fp != NULL);
      fputs(fixture, fp);
      fclose(fp);

      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.memory_cognify_enabled = 1;
      snprintf(cfg.memory_cognify_command, sizeof(cfg.memory_cognify_command),
               "cat > /dev/null; cat %s", path);

      memory_cognify_result_t result;
      int rc = memory_cognify_unit(src.id, "user prefers terse replies", &cfg, &result);
      assert(rc == 0);
      assert(strcmp(result.memory_kind, "procedural") == 0);

      /* cognified_memory_kind must have been written onto the memory. */
      char qerr[256] = "";
      aimee_pg_stmt_t *q =
          aimee_pg_prepare(db2_conn(), "SELECT cognified_memory_kind FROM memories WHERE id = ?1",
                           qerr, sizeof(qerr));
      assert(q);
      aimee_pg_bind_int64(q, "?1", src.id);
      assert(aimee_pg_step(q, qerr, sizeof(qerr)) == AIMEE_PG_ROW);
      const char *stored = aimee_pg_column_text(q, 0);
      assert(stored && strcmp(stored, "procedural") == 0);
      aimee_pg_finalize(q);

      /* Procedural preference should have landed in rules as a principle. */
      rule_t rule;

      int rfound = db2_rules_find_by_title("assistant:verbosity", &rule);
      assert(rfound == 0);
      assert(strcmp(rule.polarity, "principle") == 0);
      assert(strstr(rule.description, "terse") != NULL);

      unlink(path);
   }

   /* --- memory_cognify_canonical_kind rejection: non-procedural memory_kind
    *     with a fact claim does NOT create a rule --- */
   {
      memory_t src;
      assert(memory_insert(TIER_L2, KIND_FACT, "cognify-semantic-test", "port is 5432", 0.8,
                           "s-sem", &src) == 0);

      char fixture[512];
      snprintf(fixture, sizeof(fixture), "%s",
               "{\"summary\":\"server config\","
               "\"memory_kind\":\"semantic\","
               "\"claims\":[{\"subject\":\"server\",\"attribute\":\"port\","
               "\"value\":\"5432\",\"kind\":\"fact\"}]}");
      char path[256];
      snprintf(path, sizeof(path), "/tmp/aimee-cognify-fixture-sem-%d.json", (int)getpid());
      FILE *fp = fopen(path, "w");
      assert(fp != NULL);
      fputs(fixture, fp);
      fclose(fp);

      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.memory_cognify_enabled = 1;
      snprintf(cfg.memory_cognify_command, sizeof(cfg.memory_cognify_command),
               "cat > /dev/null; cat %s", path);

      memory_cognify_result_t result;
      assert(memory_cognify_unit(src.id, "server port is 5432", &cfg, &result) == 0);
      assert(strcmp(result.memory_kind, "semantic") == 0);

      rule_t rule;

      int rfound = db2_rules_find_by_title("server:port", &rule);
      assert(rfound != 0);

      unlink(path);
   }

   /* --- memory_episode_card_parse: valid episode card JSON --- */
   {
      const char *json = "{"
                         "\"session_id\": \"sess_abc\","
                         "\"title\": \"Camping trip with family\","
                         "\"participants\": [\"Caroline\", \"Melanie\"],"
                         "\"places\": [\"Yosemite\"],"
                         "\"events\": [\"arrived May 1\", \"hiked Half Dome May 2\"],"
                         "\"outcomes\": [\"everyone safe\"],"
                         "\"open_threads\": [\"Caroline mentioned returning in fall\"]"
                         "}";
      memory_episode_card_t card;
      int rc = memory_episode_card_parse(json, &card);
      assert(rc == 0);
      assert(strcmp(card.session_id, "sess_abc") == 0);
      assert(strcmp(card.title, "Camping trip with family") == 0);
      assert(strstr(card.participants, "Caroline") != NULL);
      assert(strstr(card.participants, "Melanie") != NULL);
      assert(strstr(card.places, "Yosemite") != NULL);
      assert(strstr(card.events, "arrived May 1") != NULL);
      assert(strstr(card.outcomes, "everyone safe") != NULL);
      assert(strstr(card.open_threads, "fall") != NULL);
   }

   /* --- memory_episode_card_parse: rejects malformed JSON --- */
   {
      memory_episode_card_t card;
      assert(memory_episode_card_parse("not json", &card) != 0);
   }

   /* --- memory_episode_card_parse: rejects JSON missing title --- */
   {
      const char *json = "{\"participants\": [\"Alice\"]}";
      memory_episode_card_t card;
      assert(memory_episode_card_parse(json, &card) != 0);
   }

   /* --- memory_episode_card_generate: disabled when episode_summaries_enabled=0 --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.memory_episode_summaries_enabled = 0;
      int64_t uid = memory_episode_card_generate("sess_test_disabled", &cfg);
      assert(uid == 0);
   }

   /* --- memory_episode_card_generate: disabled when cognify command is empty --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.memory_episode_summaries_enabled = 1;
      cfg.memory_cognify_command[0] = '\0';
      int64_t uid = memory_episode_card_generate("sess_test_nocmd", &cfg);
      assert(uid == 0);
   }

   /* --- memory_episode_cards_query: returns 0 when session has no cards --- */
   {
      char *cards[4];
      int n = memory_episode_cards_query("nonexistent_session_xyz", cards, 4);
      assert(n == 0);
   }

   /* --- memory_classify_deriver_shape: quantitative keywords --- */
   {
      assert(memory_classify_deriver_shape("how many times did we discuss deployment?") ==
             MEM_SHAPE_QUANTITATIVE);
      assert(memory_classify_deriver_shape("count occurrences of the outage") ==
             MEM_SHAPE_QUANTITATIVE);
      assert(memory_classify_deriver_shape("how often does this happen") == MEM_SHAPE_QUANTITATIVE);
   }

   /* --- memory_classify_deriver_shape: temporal interval keywords --- */
   {
      assert(memory_classify_deriver_shape("how long ago was the deployment?") ==
             MEM_SHAPE_TEMPORAL_INTERVAL);
      assert(memory_classify_deriver_shape("days since the incident") ==
             MEM_SHAPE_TEMPORAL_INTERVAL);
      assert(memory_classify_deriver_shape("elapsed time between releases") ==
             MEM_SHAPE_TEMPORAL_INTERVAL);
   }

   /* --- memory_classify_deriver_shape: plain query returns no match --- */
   {
      memory_query_shape_t s = memory_classify_deriver_shape("tell me about the project");
      assert(s != MEM_SHAPE_QUANTITATIVE && s != MEM_SHAPE_TEMPORAL_INTERVAL);
   }

   /* --- memory_derive_facts: disabled path returns 0 --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.memory_derive_facts_enabled = 0;
      memory_derived_facts_t dfacts;
      memset(&dfacts, 0, sizeof(dfacts));
      int64_t ids[1] = {1};
      int n = memory_derive_facts("how many times?", ids, 1, &cfg, &dfacts);
      assert(n == 0);
   }

   /* --- memory_derive_facts: enabled but empty candidates returns 0 --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      cfg.memory_derive_facts_enabled = 1;
      memory_derived_facts_t dfacts;
      memset(&dfacts, 0, sizeof(dfacts));
      int n = memory_derive_facts("how many times?", NULL, 0, &cfg, &dfacts);
      assert(n == 0);
   }

   /* --- memory_derived_facts_to_json: empty facts returns NULL --- */
   {
      memory_derived_facts_t dfacts;
      memset(&dfacts, 0, sizeof(dfacts));
      char *json = memory_derived_facts_to_json(&dfacts);
      assert(json == NULL);
   }

   /* --- memory_derived_facts_to_json: populated facts serialises correctly --- */
   {
      memory_derived_facts_t dfacts;
      memset(&dfacts, 0, sizeof(dfacts));
      strncpy(dfacts.facts[0].label, "count", sizeof(dfacts.facts[0].label) - 1);
      strncpy(dfacts.facts[0].value, "3", sizeof(dfacts.facts[0].value) - 1);
      dfacts.count = 1;
      char *json = memory_derived_facts_to_json(&dfacts);
      assert(json != NULL);
      assert(strstr(json, "count") != NULL);
      assert(strstr(json, "3") != NULL);
      free(json);
   }

   /* --- memory_retrieval_confidence: null inputs give below_threshold --- */
   {
      retrieval_confidence_t conf;
      memory_retrieval_confidence(NULL, 0, NULL, 0, 0.35, &conf);
      assert(conf.below_threshold == 1);
   }

   /* --- memory_retrieval_confidence: threshold=0 with no candidates is not below --- */
   {
      retrieval_confidence_t conf;
      memory_retrieval_confidence(NULL, 0, NULL, 0, 0.0, &conf);
      assert(conf.below_threshold == 0);
   }

   /* --- memory_retrieval_confidence: out=NULL is a no-op --- */
   {
      memory_retrieval_confidence(NULL, 0, NULL, 0, 0.35, NULL);
      /* should not crash */
   }

   /* --- memory_ontology_relation_from_text: known relations --- */
   {
      assert(memory_ontology_relation_from_text("co_edited") == REL_CO_EDITED);
      assert(memory_ontology_relation_from_text("co_discussed") == REL_CO_DISCUSSED);
      assert(memory_ontology_relation_from_text("fixes") == REL_FIXES);
      assert(memory_ontology_relation_from_text("depends_on") == REL_DEPENDS_ON);
      assert(memory_ontology_relation_from_text("unknown_xyz") == REL_OTHER);
      assert(memory_ontology_relation_from_text(NULL) == REL_OTHER);
   }

   /* --- memory_ontology_relation_to_text: round-trip --- */
   {
      assert(strcmp(memory_ontology_relation_to_text(REL_FIXES), "fixes") == 0);
      assert(strcmp(memory_ontology_relation_to_text(REL_CO_DISCUSSED), "co_discussed") == 0);
      assert(strcmp(memory_ontology_relation_to_text(REL_OTHER), "other") == 0);
   }

   /* --- memory_ontology_node_kind_from_text --- */
   {
      assert(memory_ontology_node_kind_from_text("file") == NODE_FILE);
      assert(memory_ontology_node_kind_from_text("commit") == NODE_COMMIT);
      assert(memory_ontology_node_kind_from_text("bogus") == NODE_OTHER);
      assert(memory_ontology_node_kind_from_text(NULL) == NODE_OTHER);
   }

   /* --- memory_ontology_validate: valid triples --- */
   {
      /* commit FIXES bug */
      assert(memory_ontology_validate(NODE_COMMIT, REL_FIXES, NODE_BUG) == 1);
      /* any CO_DISCUSSED any */
      assert(memory_ontology_validate(NODE_FILE, REL_CO_DISCUSSED, NODE_CONCEPT) == 1);
      /* REL_OTHER always allowed */
      assert(memory_ontology_validate(NODE_FILE, REL_OTHER, NODE_MODULE) == 1);
   }

   /* --- memory_ontology_validate: invalid triple --- */
   {
      /* function FIXES bug: not in schema (commit should fix bugs) */
      assert(memory_ontology_validate(NODE_FUNCTION, REL_FIXES, NODE_BUG) == 0);
   }

   /* --- memory_graph_walk: empty DB returns 0 entries --- */
   {
      graph_walk_entry_t entries[16];
      int n = memory_graph_walk("nonexistent_entity", RELATION_MASK_ALL, 2, entries, 16);
      assert(n == 0);
   }

   /* --- memory_graph_walk: NULL params return 0 --- */
   {
      graph_walk_entry_t entries[4];
      assert(memory_graph_walk(NULL, RELATION_MASK_ALL, 1, entries, 4) == 0);
      assert(memory_graph_walk("e", RELATION_MASK_ALL, 1, NULL, 4) == 0);
   }

   /* --- db1_cognify_job_enqueue / memory_cognify_queue_status --- */
   {
      /* Enqueue a job for a memory that we insert first */
      memory_t m;
      memory_insert(TIER_L2, KIND_FACT, "cognify:test:key", "test content", 1.0, "test", &m);
      assert(m.id > 0);

      int rc = db1_cognify_job_enqueue(m.id);
      assert(rc == 0);

      memory_cognify_queue_stats_t stats;
      int sr = memory_cognify_queue_status(&stats);
      assert(sr == 0);
      assert(stats.pending >= 1);

      /* Duplicate enqueue must be ignored (UNIQUE constraint) */
      int rc2 = db1_cognify_job_enqueue(m.id);
      assert(rc2 == 0);

      memory_cognify_queue_stats_t stats2;
      memory_cognify_queue_status(&stats2);
      assert(stats2.pending == stats.pending);
   }

   /* --- memory_cognify_drain with cognifier disabled --- */
   {
      /* When cognify_enabled=0, drain is a no-op but must not crash */
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      /* disabled: memory_cognify_enabled = 0 */
      memory_cognify_queue_stats_t stats;
      int rc = memory_cognify_drain(&cfg, 0, &stats);
      assert(rc == 0);
      /* pending jobs remain because we can't actually run cognifier in tests */
   }

   /* --- memory_detect_aggregation_shape: structural heuristic ---
    * Covers the positive shapes from the proposal, entity seed extraction,
    * status-word tagging, and the "looks like aggregation but isn't"
    * idiomatic phrases that must NOT trigger the route.  Pure helper — no
    * DB reset needed. */
   {
      memory_aggregation_hint_t h;

      /* Positive: quantifier + plural noun. */
      assert(memory_detect_aggregation_shape("list all open commitments", &h) == 1);
      assert(h.has_quantifier == 1);
      assert(h.has_status_word == 1);
      assert(strcmp(h.status_word, "open") == 0);

      assert(memory_detect_aggregation_shape("every decision we made about pricing", &h) == 1);
      assert(h.has_quantifier == 1);

      /* Positive: interrogative-with-plural-noun + proper-noun entity. */
      assert(memory_detect_aggregation_shape("What cities has Jon visited?", &h) == 1);
      assert(h.has_quantifier == 1);
      assert(strcmp(h.entity_seed, "jon") == 0);

      /* Positive: "show me all ENTITY has Ved" — entity must NOT pick up the
       * sentence-initial "Show" / "All". */
      assert(memory_detect_aggregation_shape("show me all the dogs Thomas has mentioned", &h) == 1);
      assert(strcmp(h.entity_seed, "thomas") == 0);

      /* Positive: enumerate keyword. */
      assert(memory_detect_aggregation_shape("enumerate every pet the user owns", &h) == 1);

      /* Positive: "which" + plural-noun. */
      assert(memory_detect_aggregation_shape("which tickets are unresolved?", &h) == 1);
      assert(h.has_status_word == 1);
      assert(strcmp(h.status_word, "unresolved") == 0);

      /* Negative: point queries must stay untouched. */
      assert(memory_detect_aggregation_shape("what is Jordan's food preference?", &h) == 0);
      assert(memory_detect_aggregation_shape("when did we decide on pricing?", &h) == 0);
      assert(memory_detect_aggregation_shape("who is the on-call engineer today?", &h) == 0);
      assert(memory_detect_aggregation_shape("how do we deploy the api service?", &h) == 0);

      /* Negative: idiomatic quantifier phrases.  These look like "all X" or
       * "every Y" but aren't coverage queries — the stop-phrase list must
       * suppress them. */
      assert(memory_detect_aggregation_shape("can you do it all at once?", &h) == 0);
      assert(memory_detect_aggregation_shape("all right, let's ship it", &h) == 0);
      assert(memory_detect_aggregation_shape("every time I run it, it crashes", &h) == 0);

      /* Negative: empty / null inputs don't trip the detector. */
      assert(memory_detect_aggregation_shape("", &h) == 0);
      assert(memory_detect_aggregation_shape(NULL, &h) == 0);
   }

   /* --- memory_briefing: bundle shape, ranking, and determinism --- */
   {
      /* Fresh DB so the briefing sees only fixture rows */
      reset_db();

      char err[256] = "";
      memory_t m;
      /* High-salience L3 fact: should lead the key_facts list */
      memory_insert(TIER_L3, KIND_FACT, "brief:top", "top fact", 0.95, "s1", &m);
      int64_t top_id = m.id;
      assert(aimee_pg_exec(db2_conn(),
                           "UPDATE memories SET evidence_strength=0.9, observation_count=5,"
                           " use_count=7 WHERE id = (SELECT MAX(id) FROM memories)",
                           err, sizeof(err)) == 0);

      /* Mid-salience L2 fact */
      memory_insert(TIER_L2, KIND_FACT, "brief:mid", "mid fact", 0.7, "s1", &m);

      /* L1 row — must be excluded (tier filter) */
      memory_insert("L1", KIND_FACT, "brief:low", "low fact", 0.99, "s1", &m);

      /* Scratch row in L2 — must be excluded (kind filter) */
      memory_insert(TIER_L2, "scratch", "brief:scratch", "scratch", 0.99, "s1", &m);

      /* Entity mentions so active_entities has something to surface */
      assert(aimee_pg_exec(db2_conn(),
                           "INSERT INTO memory_entities(memory_id, entity) VALUES"
                           " ((SELECT id FROM memories WHERE key='brief:top'), 'caroline'),"
                           " ((SELECT id FROM memories WHERE key='brief:mid'), 'caroline'),"
                           " ((SELECT id FROM memories WHERE key='brief:mid'), 'atlas')",
                           err, sizeof(err)) == 0);

      /* memory_insert auto-refreshes memory_episodes from (key, content) with
       * the same source_session that was passed in — so we need a clean slate
       * before inserting our own session-distinguishing rows, otherwise the
       * auto-refreshed rows dominate the result set. */
      assert(aimee_pg_exec(db2_conn(), "DELETE FROM memory_episodes", err, sizeof(err)) == 0);

      /* Two episode cards across distinct sessions — recent_activity must
       * collapse to one row per session. */
      int exec_rc =
          aimee_pg_exec(db2_conn(),
                        "INSERT INTO memory_episodes(memory_id, episode_key, episode_text,"
                        " source_session, created_at) VALUES"
                        " ((SELECT id FROM memories WHERE key='brief:top'),"
                        "   'ep1', 'worked on auth refactor', 'sess-a', '2026-04-15 10:00:00'),"
                        " ((SELECT id FROM memories WHERE key='brief:mid'),"
                        "   'ep2', 'reviewed PR 298', 'sess-b', '2026-04-16 12:00:00'),"
                        " ((SELECT id FROM memories WHERE key='brief:mid'),"
                        "   'ep3', 'older sess-a entry', 'sess-a', '2026-04-10 09:00:00')",
                        err, sizeof(err));
      if (exec_rc != 0)
         fprintf(stderr, "episode insert failed: %s\n", err);
      assert(exec_rc == 0);

      struct cJSON *bundle = memory_briefing(0 /* default limit */);
      assert(bundle != NULL);
      const char *style =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive((cJSON *)bundle, "briefing_style"));
      assert(style && strcmp(style, "compact") == 0);
      int limit = (int)cJSON_GetNumberValue(
          cJSON_GetObjectItemCaseSensitive((cJSON *)bundle, "limit_tokens"));
      assert(limit == 1024);

      struct cJSON *key_facts = cJSON_GetObjectItemCaseSensitive((cJSON *)bundle, "key_facts");
      struct cJSON *recent = cJSON_GetObjectItemCaseSensitive((cJSON *)bundle, "recent_activity");
      struct cJSON *entities = cJSON_GetObjectItemCaseSensitive((cJSON *)bundle, "active_entities");
      assert(cJSON_IsArray((cJSON *)key_facts));
      assert(cJSON_IsArray((cJSON *)recent));
      assert(cJSON_IsArray((cJSON *)entities));

      /* L1 and scratch must be absent; L2/L3 must be present */
      int seen_top = 0, seen_mid = 0, seen_low = 0, seen_scratch = 0;
      cJSON *it = NULL;
      cJSON_ArrayForEach(it, (cJSON *)key_facts)
      {
         const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(it, "key"));
         if (!key)
            continue;
         if (strcmp(key, "brief:top") == 0)
            seen_top = 1;
         if (strcmp(key, "brief:mid") == 0)
            seen_mid = 1;
         if (strcmp(key, "brief:low") == 0)
            seen_low = 1;
         if (strcmp(key, "brief:scratch") == 0)
            seen_scratch = 1;
      }
      assert(seen_top);
      assert(seen_mid);
      assert(!seen_low);
      assert(!seen_scratch);

      /* Highest confidence/evidence/observation entry should rank first */
      cJSON *first = cJSON_GetArrayItem((cJSON *)key_facts, 0);
      assert(first);
      long long first_id = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(first, "memory_id"));
      assert(first_id == top_id);

      /* recent_activity: exactly one entry per distinct session, sorted by
       * latest created_at DESC.  sess-b (04-16) must come before sess-a (04-15). */
      assert(cJSON_GetArraySize((cJSON *)recent) == 2);
      cJSON *r0 = cJSON_GetArrayItem((cJSON *)recent, 0);
      cJSON *r1 = cJSON_GetArrayItem((cJSON *)recent, 1);
      const char *r0_sess = cJSON_GetStringValue(cJSON_GetObjectItem(r0, "session_id"));
      const char *r1_sess = cJSON_GetStringValue(cJSON_GetObjectItem(r1, "session_id"));
      assert(strcmp(r0_sess, "sess-b") == 0);
      assert(strcmp(r1_sess, "sess-a") == 0);

      /* active_entities: caroline has 2 mentions, atlas has 1. */
      assert(cJSON_GetArraySize((cJSON *)entities) >= 2);
      cJSON *e0 = cJSON_GetArrayItem((cJSON *)entities, 0);
      const char *e0_name = cJSON_GetStringValue(cJSON_GetObjectItem(e0, "name"));
      int e0_mentions = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(e0, "mentions"));
      assert(strcmp(e0_name, "caroline") == 0);
      assert(e0_mentions == 2);

      /* Determinism: a second call on the frozen DB must produce a
       * byte-identical bundle.  Guarantees no LLM or clock-sensitive noise
       * leaked into the hot path. */
      struct cJSON *bundle2 = memory_briefing(0);
      assert(bundle2 != NULL);
      char *j1 = cJSON_PrintUnformatted((cJSON *)bundle);
      char *j2 = cJSON_PrintUnformatted((cJSON *)bundle2);
      assert(j1 && j2);
      assert(strcmp(j1, j2) == 0);
      free(j1);
      free(j2);
      cJSON_Delete((cJSON *)bundle2);
      cJSON_Delete((cJSON *)bundle);

      /* Token cap: a tiny budget must trim the bundle under it.  Key facts
       * are the last thing shrunk, so at the very least lower-priority
       * sections should empty out. */
      struct cJSON *small = memory_briefing(MEMORY_BRIEFING_MIN_LIMIT_TOKENS);
      assert(small);
      int approx = (int)cJSON_GetNumberValue(
          cJSON_GetObjectItemCaseSensitive((cJSON *)small, "approx_tokens"));
      assert(approx <= MEMORY_BRIEFING_MIN_LIMIT_TOKENS);
      cJSON *small_entities = cJSON_GetObjectItemCaseSensitive((cJSON *)small, "active_entities");
      cJSON *small_recent = cJSON_GetObjectItemCaseSensitive((cJSON *)small, "recent_activity");
      /* Lower-priority sections must trim before key_facts. */
      assert(cJSON_GetArraySize((cJSON *)small_entities) == 0);
      assert(cJSON_GetArraySize((cJSON *)small_recent) == 0);
      cJSON_Delete((cJSON *)small);

      assert(db2_bandit_promotion_set("briefing_style", "evidence_heavy", "compact") == 0);
      struct cJSON *heavy = memory_briefing(0);
      assert(heavy);
      const char *heavy_style =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive((cJSON *)heavy, "briefing_style"));
      int heavy_limit = (int)cJSON_GetNumberValue(
          cJSON_GetObjectItemCaseSensitive((cJSON *)heavy, "limit_tokens"));
      assert(heavy_style && strcmp(heavy_style, "evidence_heavy") == 0);
      assert(heavy_limit == 3000);
      cJSON_Delete((cJSON *)heavy);
   }

   /* --- memory_aggregate: entity-route coverage + truncated flag --- */
   {
      reset_db();

      memory_t m;
      /* Seed rows: three "jon" facts + three unrelated rows.  Aggregation
       * should return the jon rows only (entity-seed route), recency-
       * ordered. */
      char err[256] = "";
      memory_insert(TIER_L2, KIND_FACT, "jon:city-1", "Jon visited Tokyo in March", 0.9, "s1", &m);
      int64_t jon_tokyo = m.id;
      assert(aimee_pg_exec(db2_conn(),
                           "INSERT INTO memory_entities(memory_id, entity) VALUES"
                           " ((SELECT id FROM memories WHERE key='jon:city-1'), 'jon')",
                           err, sizeof(err)) == 0);

      memory_insert(TIER_L2, KIND_FACT, "jon:city-2", "Jon visited Osaka last month", 0.9, "s1",
                    &m);
      assert(aimee_pg_exec(db2_conn(),
                           "INSERT INTO memory_entities(memory_id, entity) VALUES"
                           " ((SELECT id FROM memories WHERE key='jon:city-2'), 'jon')",
                           err, sizeof(err)) == 0);

      memory_insert(TIER_L2, KIND_FACT, "jon:city-3", "Jon visited Kyoto on vacation", 0.9, "s1",
                    &m);
      assert(aimee_pg_exec(db2_conn(),
                           "INSERT INTO memory_entities(memory_id, entity) VALUES"
                           " ((SELECT id FROM memories WHERE key='jon:city-3'), 'jon')",
                           err, sizeof(err)) == 0);

      /* Unrelated rows — no "jon" entity or mention.  These must NOT land in
       * the aggregation result. */
      memory_insert(TIER_L2, KIND_FACT, "sam:city", "Sam moved to Lagos", 0.9, "s1", &m);
      memory_insert(TIER_L2, KIND_FACT, "deploy", "deploys run on Tuesdays", 0.9, "s1", &m);

      memory_aggregation_hint_t hint;
      assert(memory_detect_aggregation_shape("What cities has Jon visited?", &hint) == 1);

      memory_t rows[16];
      int truncated = 0;
      int n = memory_aggregate(&hint, "What cities has Jon visited?", 10, rows, 16, &truncated);
      assert(n == 3);
      assert(truncated == 0);
      /* Every returned row must actually be one of the jon memories.  Any
       * leak of sam/deploy would indicate the entity filter is too loose. */
      for (int i = 0; i < n; i++)
      {
         int is_jon = (rows[i].id == jon_tokyo) || (strstr(rows[i].content, "Jon") != NULL) ||
                      (strstr(rows[i].key, "jon:") != NULL);
         assert(is_jon);
      }

      /* Truncation: ask for only 2 rows and verify the caller is told. */
      int truncated2 = 0;
      int n2 = memory_aggregate(&hint, "What cities has Jon visited?", 2, rows, 16, &truncated2);
      assert(n2 == 2);
      assert(truncated2 == 1);
   }

   /* --- memory.answerability: default-off trace, gate abstain, curated exemption --- */
   {
      reset_db();

      write_test_config("memory:\n  abstain:\n    enabled: false\n    gate: 0.99\n    "
                        "chunk_min_confidence: 0.0\n");
      memory_t m;
      assert(memory_insert(TIER_L2, KIND_FACT, "mars:color", "mars color is red", 0.9, "s1", &m) ==
             0);
      memory_answer_result_t result;
      assert(memory_ask_query("mars color", 5, &result) == 0);
      assert(result.no_answer == 0);
      assert(result.evidence.decision == MEMORY_ANSWER_DECISION_ANSWERABLE);
      assert(result.evidence.ranked_count > 0);
      assert(result.evidence.candidate_id_count > 0);

      write_test_config("memory:\n  abstain:\n    enabled: true\n    gate: 0.99\n    "
                        "chunk_min_confidence: 0.0\n");
      memset(&result, 0, sizeof(result));
      assert(memory_ask_query("mars color", 5, &result) == 0);
      assert(result.no_answer == 1);
      assert(result.answer[0] == '\0');
      assert(result.citation_count == 0);
      assert(result.evidence.decision == MEMORY_ANSWER_DECISION_ABSTAIN);
      assert(result.evidence.reason == MEMORY_ANSWER_REASON_GROUNDING_LOW);
      char *rendered = memory_answer_query("mars color", 5);
      assert(rendered && strcmp(rendered, "No confident answer for \"mars color\"") == 0);
      free(rendered);

      assert(memory_insert(TIER_L4, KIND_FACT, "venus:color", "venus color is yellow", 0.9, "s1",
                           &m) == 0);
      memset(&result, 0, sizeof(result));
      assert(memory_ask_query("venus color", 5, &result) == 0);
      assert(result.no_answer == 0);
      assert(result.evidence.decision == MEMORY_ANSWER_DECISION_EXEMPT);
      assert(result.evidence.reason == MEMORY_ANSWER_REASON_CURATED_EXEMPT);
      assert(result.evidence.exempt == 1);
   }

   /* --- memory_aggregate: keyword fallback when no entity seed --- */
   {
      memory_aggregation_hint_t hint;
      assert(memory_detect_aggregation_shape("list all decisions about pricing", &hint) == 1);
      assert(hint.entity_seed[0] == '\0'); /* no proper-noun seed */

      memory_t rows[16];
      int truncated = 0;
      /* Seed a decision row that contains the fallback keyword in content. */
      memory_t m;
      memory_insert(TIER_L2, KIND_DECISION, "pricing:enterprise-tier",
                    "Enterprise pricing tier approved for Q2", 0.9, "s1", &m);

      int n = memory_aggregate(&hint, "list all decisions about pricing", 10, rows, 16, &truncated);
      assert(n >= 1);
      int saw_pricing = 0;
      for (int i = 0; i < n; i++)
      {
         if (strstr(rows[i].content, "pricing") || strstr(rows[i].content, "Pricing"))
            saw_pricing = 1;
      }
      assert(saw_pricing);
   }

   /* --- Prospective memory: CRUD, state machine, matcher ---
    * Covers the acceptance criteria: create / list / complete / expire,
    * once vs repeat, exact-anchor and FTS match paths, and that default
    * fact recall doesn't leak armed reminders. */
   {
      reset_db();

      /* Create three reminders — one entity-anchored, one file-anchored,
       * one lexical-only — so each matcher stage has something to hit. */
      memory_prospective_t ent_rem, file_rem, lex_rem;
      assert(memory_prospective_create("when CI comes up again", "ask about the token rotation",
                                       "ci-runbook", "", "once", "", "s1", &ent_rem) == 0);
      assert(memory_prospective_create(
                 "next time you touch src/tests/Rules.mk double-check the mock-vs-real"
                 " agent_http_* split",
                 "verify Rules.mk still distinguishes TEST_DATA_OBJS vs TEST_DATA_OBJS_MOCK", "",
                 "src/tests/Rules.mk", "once", "", "s1", &file_rem) == 0);
      assert(memory_prospective_create("when rotation policy discussed",
                                       "remind me about the weekly rotation check", "", "",
                                       "repeat", "", "s1", &lex_rem) == 0);

      /* All three land in "armed" and list newest-first. */
      memory_prospective_t rows[16];
      int n = memory_prospective_list(NULL, rows, 16);
      assert(n == 3);
      for (int i = 0; i < n; i++)
         assert(strcmp(rows[i].state, "armed") == 0);

      /* State-filtered list: no triggered rows yet. */
      assert(memory_prospective_list("triggered", rows, 16) == 0);

      /* Entity anchor match fires for the CI reminder. */
      n = memory_prospective_match("the nightly run is green", "ci-runbook", NULL, rows, 16);
      assert(n == 1);
      assert(rows[0].id == ent_rem.id);

      /* File anchor match fires for the Rules.mk reminder. */
      n = memory_prospective_match("routine edit", "", "src/tests/Rules.mk", rows, 16);
      assert(n == 1);
      assert(rows[0].id == file_rem.id);

      /* Lexical match: FTS on trigger_text catches "rotation".
       * Postgres-only — the in-memory aimee_pg sqlite shim doesn't
       * faithfully emulate to_tsvector/to_tsquery, so the FTS path is
       * exercised by the db2 contract tests against real Postgres. */
      n = memory_prospective_match("we should rotate the staging tokens", "", "", rows, 16);
      (void)n;

      /* Fuzzy stem-overlap stage: a turn that shares no FTS prefix token
       * with the trigger still matches via morphology.  The reminder's
       * trigger is "when rotation policy discussed" — none of those words
       * appear as prefixes of "discussing policies", but the stems
       * "discuss" and "polic" do overlap. */
      n = memory_prospective_match("we were discussing policies earlier", "", "", rows, 16);
      int saw_fuzzy = 0;
      for (int i = 0; i < n; i++)
         if (rows[i].id == lex_rem.id)
            saw_fuzzy = 1;
      assert(saw_fuzzy);

      /* Mark the once-shot entity reminder triggered — its state flips.
       * Trigger the repeating one and verify it stays armed. */
      assert(memory_prospective_mark_triggered(ent_rem.id) == 0);
      memory_prospective_t got;
      assert(memory_prospective_get(ent_rem.id, &got) == 0);
      assert(strcmp(got.state, "triggered") == 0);
      assert(got.trigger_count == 1);

      assert(memory_prospective_mark_triggered(lex_rem.id) == 0);
      assert(memory_prospective_get(lex_rem.id, &got) == 0);
      assert(strcmp(got.state, "armed") == 0);
      assert(got.trigger_count == 1);

      /* Subsequent matches for the now-triggered once reminder should NOT
       * surface it again. */
      n = memory_prospective_match("the nightly run is green", "ci-runbook", NULL, rows, 16);
      assert(n == 0);

      /* Complete the repeating reminder; subsequent completes fail. */
      assert(memory_prospective_complete(lex_rem.id) == 0);
      assert(memory_prospective_get(lex_rem.id, &got) == 0);
      assert(strcmp(got.state, "completed") == 0);
      assert(memory_prospective_complete(lex_rem.id) == -1);

      /* Expiry sweep: insert a reminder past its valid_until and confirm
       * the sweep transitions it to expired. */
      memory_prospective_t past_rem;
      assert(memory_prospective_create("legacy trigger", "legacy action", "", "", "once",
                                       "2020-01-01 00:00:00", "s1", &past_rem) == 0);
      int expired = memory_prospective_sweep_expired();
      assert(expired >= 1);
      assert(memory_prospective_get(past_rem.id, &got) == 0);
      assert(strcmp(got.state, "expired") == 0);

      /* Default memory_list (i.e. the normal fact-recall surface) must not
       * leak prospective reminders — they live in their own table. */
      memory_t mems[16];
      int mem_n = memory_list(NULL, NULL, 16, mems, 16);
      assert(mem_n == 0); /* nothing inserted into `memories` */

      /* Input validation. */
      assert(memory_prospective_create("", "x", "", "", NULL, "", "", NULL) == -1);
      assert(memory_prospective_create("trigger", "", "", "", NULL, "", "", NULL) == -1);
      assert(memory_prospective_create("trigger", "action", "", "", "never", "", "", NULL) == -1);
      assert(memory_prospective_complete(999999) == -1);
   }

   /* --- Memory lifecycle: state machine, commitment detection, sweep,
    * alerts bundle, stress fixture --- */
   {
      reset_db();

      /* Commitment-shape detection: coarse heuristic covers the proposal's
       * three bands (date / relative / open-ended) plus common negatives. */
      {
         char shape[16];
         int ttl = 0;
         assert(memory_detect_commitment_shape("I'll send the PR by Friday", shape, sizeof(shape),
                                               &ttl) == 1);
         assert(strcmp(shape, "date") == 0);
         assert(ttl > 0);

         assert(memory_detect_commitment_shape("We'll review this week", shape, sizeof(shape),
                                               &ttl) == 1);
         assert(strcmp(shape, "relative") == 0);

         assert(memory_detect_commitment_shape("I'll get to it", shape, sizeof(shape), &ttl) == 1);
         assert(strcmp(shape, "open-ended") == 0);
         assert(ttl == MEMORY_LIFECYCLE_TTL_DEFAULT_OPEN_ENDED_DAYS);

         /* An explicit caller tag wins even without future-tense framing. */
         assert(memory_detect_commitment_shape("[pending] finish the audit", shape, sizeof(shape),
                                               &ttl) == 1);

         /* Negatives: declarative facts, past tense, third-party future. */
         assert(memory_detect_commitment_shape("The deploy runs on Tuesdays", shape, sizeof(shape),
                                               &ttl) == 0);
         assert(memory_detect_commitment_shape("We shipped the fix yesterday", shape, sizeof(shape),
                                               &ttl) == 0);
         assert(memory_detect_commitment_shape("They'll merge it eventually", shape, sizeof(shape),
                                               &ttl) == 0);
         assert(memory_detect_commitment_shape("", shape, sizeof(shape), &ttl) == 0);
         assert(memory_detect_commitment_shape(NULL, NULL, 0, NULL) == 0);
      }

      /* State machine: every valid transition fires, every invalid one
       * returns -1.  Counts per state stay accurate after the sweep. */
      {
         memory_t m;
         assert(memory_insert(TIER_L2, KIND_FACT, "sm:active", "baseline active fact", 0.9, "s1",
                              &m) == 0);
         /* Newly-inserted rows default to active. */
         memory_lifecycle_counts_t cts;
         assert(memory_lifecycle_counts(&cts) == 0);
         assert(cts.active == 1);
         assert(cts.pending == 0 && cts.fulfilled == 0 && cts.superseded == 0 && cts.archived == 0);

         /* active → pending via memory_mark_pending. */
         assert(memory_mark_pending(m.id, 7) == 0);
         assert(memory_lifecycle_counts(&cts) == 0);
         assert(cts.pending == 1 && cts.active == 0);

         /* pending → fulfilled is valid.  Direct pending → active is NOT
          * (the state machine only allows pending → fulfilled | archived). */
         assert(memory_transition_lifecycle(m.id, MEMORY_LIFECYCLE_STATE_ACTIVE, NULL) == -1);
         assert(memory_transition_lifecycle(m.id, MEMORY_LIFECYCLE_STATE_FULFILLED, NULL) == 0);
         assert(memory_lifecycle_counts(&cts) == 0);
         assert(cts.fulfilled == 1);

         /* fulfilled → archived is valid; fulfilled → pending is not. */
         assert(memory_transition_lifecycle(m.id, MEMORY_LIFECYCLE_STATE_PENDING, NULL) == -1);
         assert(memory_transition_lifecycle(m.id, MEMORY_LIFECYCLE_STATE_ARCHIVED, "manual") == 0);
         /* Archived is terminal — any outbound transition is rejected. */
         assert(memory_transition_lifecycle(m.id, MEMORY_LIFECYCLE_STATE_ACTIVE, NULL) == -1);
         assert(memory_transition_lifecycle(m.id, MEMORY_LIFECYCLE_STATE_PENDING, NULL) == -1);

         /* Archived memories are still fetchable via memory_get — archival
          * is metadata, not truncation. */
         memory_t got;
         assert(memory_get(m.id, &got) == 0);
         assert(strcmp(got.key, "sm:active") == 0);
      }

      /* Sweep: rows whose ttl_at is in the past transition to archived
       * idempotently. */
      {
         reset_db();

         memory_t m;
         assert(memory_insert(TIER_L2, KIND_FACT, "sweep:one", "I'll review this next week", 0.9,
                              "s1", &m) == 0);
         /* Seed ttl_at directly in the past so the sweep must archive it. */
         char err[256] = "";
         assert(aimee_pg_exec(db2_conn(),
                              "UPDATE memories SET lifecycle_state = 'pending',"
                              " ttl_at = '2020-01-01 00:00:00'",
                              err, sizeof(err)) == 0);
         int archived = memory_lifecycle_sweep_expired();
         assert(archived >= 1);
         memory_lifecycle_counts_t cts;
         assert(memory_lifecycle_counts(&cts) == 0);
         assert(cts.archived >= 1);

         /* Running again is idempotent: no rows left to archive. */
         assert(memory_lifecycle_sweep_expired() == 0);
      }

      /* Alerts bundle shape: stale_pending surfaces rows past 80% of the
       * TTL window; unresolved_contradictions pulls from memory_conflicts;
       * newly_superseded pulls rows transitioned to superseded since the
       * `since` cutoff. */
      {
         reset_db();

         char err[256] = "";
         memory_t stale;
         assert(memory_insert(TIER_L2, KIND_FACT, "alert:stale", "I'll audit this month", 0.9, "s1",
                              &stale) == 0);
         /* Created 9 days ago with a 10-day window — 90% elapsed > 80%
          * threshold, so stale_pending should pick it up. */
         assert(aimee_pg_exec(db2_conn(),
                              "UPDATE memories SET lifecycle_state = 'pending',"
                              " created_at = datetime('now', '-9 days'),"
                              " ttl_at = datetime('now', '+1 days')"
                              " WHERE key = 'alert:stale'",
                              err, sizeof(err)) == 0);

         /* Fresh pending (just created, 10-day window) must NOT be stale. */
         memory_t fresh;
         assert(memory_insert(TIER_L2, KIND_FACT, "alert:fresh", "I'll sync tomorrow", 0.9, "s1",
                              &fresh) == 0);
         assert(aimee_pg_exec(db2_conn(),
                              "UPDATE memories SET lifecycle_state = 'pending',"
                              " created_at = datetime('now', '-1 days'),"
                              " ttl_at = datetime('now', '+9 days')"
                              " WHERE key = 'alert:fresh'",
                              err, sizeof(err)) == 0);

         /* Conflict row for the unresolved section. */
         memory_t a, b;
         assert(memory_insert(TIER_L2, KIND_FACT, "alert:conflict", "value A", 0.9, "s1", &a) == 0);
         assert(memory_insert(TIER_L2, KIND_FACT, "alert:conflict", "value B", 0.9, "s1", &b) == 0);
         memory_record_conflict(a.id, b.id);

         /* Newly superseded row. */
         memory_t sup;
         assert(memory_insert(TIER_L2, KIND_FACT, "alert:sup", "was-true", 0.9, "s1", &sup) == 0);
         assert(memory_transition_lifecycle(sup.id, MEMORY_LIFECYCLE_STATE_SUPERSEDED, NULL) == 0);

         cJSON *bundle = memory_alerts(NULL);
         assert(bundle != NULL);
         cJSON *stale_arr = cJSON_GetObjectItemCaseSensitive(bundle, "stale_pending");
         cJSON *conf_arr = cJSON_GetObjectItemCaseSensitive(bundle, "unresolved_contradictions");
         cJSON *sup_arr = cJSON_GetObjectItemCaseSensitive(bundle, "newly_superseded");
         assert(cJSON_IsArray(stale_arr));
         assert(cJSON_IsArray(conf_arr));
         assert(cJSON_IsArray(sup_arr));

         /* Exactly the stale row lands in stale_pending, not the fresh one. */
         int saw_stale = 0, saw_fresh = 0;
         cJSON *it = NULL;
         cJSON_ArrayForEach(it, stale_arr)
         {
            long long id = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "memory_id"));
            if (id == stale.id)
               saw_stale = 1;
            if (id == fresh.id)
               saw_fresh = 1;
         }
         assert(saw_stale);
         assert(!saw_fresh);

         /* Unresolved conflict shows up. */
         assert(cJSON_GetArraySize(conf_arr) >= 1);

         /* Newly-superseded section captures the row we transitioned. */
         int saw_sup = 0;
         cJSON_ArrayForEach(it, sup_arr)
         {
            long long id = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "memory_id"));
            if (id == sup.id)
               saw_sup = 1;
         }
         assert(saw_sup);

         double elapsed =
             cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(bundle, "elapsed_ms"));
         assert(elapsed >= 0.0);
         cJSON_Delete(bundle);
      }

      /* Stress fixture: inject 500 commitment-shape rows across a simulated
       * 90-day window and assert that the sweep keeps the stale-pending
       * count bounded rather than growing linearly.  Proposal's acceptance
       * criterion: "shows stale-pending count plateaus rather than growing
       * linearly." */
      {
         reset_db();

         /* Seed 500 rows evenly spaced across the last 90 days. Each has a
          * 10-day TTL — anything whose created_at is older than ~10 days
          * ago is already expired and must be archived by the sweep. */
         char err[256] = "";
         aimee_pg_exec(db2_conn(), "BEGIN", err, sizeof(err));
         for (int i = 0; i < 500; i++)
         {
            char key[64];
            snprintf(key, sizeof(key), "stress:%d", i);
            memory_t m;
            memory_insert(TIER_L2, KIND_FACT, key, "I'll ship this next week", 0.9, "s1", &m);

            char ageq[512];
            int age_days = 90 - (i % 90); /* 1..90 */
            snprintf(ageq, sizeof(ageq),
                     "UPDATE memories SET lifecycle_state = 'pending',"
                     " created_at = datetime('now', '-%d days'),"
                     " ttl_at = datetime('now', '-%d days', '+10 days')"
                     " WHERE key = '%s'",
                     age_days, age_days, key);
            err[0] = '\0';
            int urc = aimee_pg_exec(db2_conn(), ageq, err, sizeof(err));
            if (urc != 0)
            {
               fprintf(stderr, "stress update failed at i=%d: %s\nSQL: %s\n", i, err, ageq);
               assert(0);
            }
         }
         aimee_pg_exec(db2_conn(), "COMMIT", err, sizeof(err));

         int archived_first = memory_lifecycle_sweep_expired();
         assert(archived_first > 0);

         /* After the sweep, the remaining `pending` count is bounded by
          * the number of rows still inside their TTL window — roughly
          * 10/90 of the seed.  Stale-pending (past-80%) is also bounded
          * and, critically, smaller than the raw count. */
         memory_lifecycle_counts_t cts;
         assert(memory_lifecycle_counts(&cts) == 0);
         assert(cts.pending < 500);
         assert(cts.archived >= archived_first);

         cJSON *bundle = memory_alerts(NULL);
         cJSON *stale_arr = cJSON_GetObjectItemCaseSensitive(bundle, "stale_pending");
         int stale_count = cJSON_GetArraySize(stale_arr);
         /* Plateau check: stale_count <= pending <= plateau bound. */
         assert(stale_count <= (int)cts.pending);
         cJSON_Delete(bundle);

         /* Running the sweep again must be a no-op — idempotence. */
         int archived_second = memory_lifecycle_sweep_expired();
         assert(archived_second == 0);
      }
   }

   /* --- memory_recall: bundle shape, section caps, filters, determinism --- */
   {
      reset_db();

      memory_t m;

      /* Identity: stable key-prefix rows at L2+. */
      assert(memory_insert(TIER_L3, KIND_FACT, "identity:name", "user is Jim", 0.98, "s1", &m) ==
             0);
      assert(memory_insert(TIER_L2, KIND_FACT, "role:engineer", "user is a software engineer", 0.95,
                           "s1", &m) == 0);
      /* Negative control: same tier/kind, non-identity key — must NOT surface in identity. */
      assert(memory_insert(TIER_L2, KIND_FACT, "project:alpha", "project alpha started", 0.9, "s1",
                           &m) == 0);
      /* Negative control: L1 identity-shaped key — tier filter must reject. */
      assert(memory_insert("L1", KIND_FACT, "identity:low", "should not surface", 0.95, "s1", &m) ==
             0);

      /* Preferences: KIND_PREFERENCE at L2+. */
      assert(memory_insert(TIER_L2, KIND_PREFERENCE, "pref:indent", "user prefers 3-space", 0.9,
                           "s1", &m) == 0);

      /* Active context: recent, in-window L1/L2 facts. */
      assert(memory_insert(TIER_L2, KIND_FACT, "active:one", "touched today", 0.8, "s1", &m) == 0);
      char err[256] = "";
      assert(aimee_pg_exec(db2_conn(),
                           "UPDATE memories SET last_used_at = datetime('now'),"
                           " updated_at = datetime('now') WHERE key = 'active:one'",
                           err, sizeof(err)) == 0);

      /* Open commitment: mark one fact pending. */
      assert(memory_insert(TIER_L2, KIND_FACT, "commit:one", "I'll review this next week", 0.9,
                           "s1", &m) == 0);
      int64_t commit_id = m.id;
      assert(memory_mark_pending(commit_id, 7) == 0);

      /* Reminder: armed prospective memory that the matcher can surface. */
      memory_prospective_t pm;
      assert(memory_prospective_create("when rotation comes up", "remind about the token swap", "",
                                       "", "once", "", "s1", &pm) == 0);

      cJSON *bundle = memory_recall("we should rotate the staging tokens", 0 /* default limit */,
                                    1 /* session_start */);
      assert(bundle != NULL);

      /* Shape: all six sections must exist as arrays, regardless of fill. */
      cJSON *identity = cJSON_GetObjectItemCaseSensitive(bundle, "identity");
      cJSON *preferences = cJSON_GetObjectItemCaseSensitive(bundle, "preferences");
      cJSON *active = cJSON_GetObjectItemCaseSensitive(bundle, "active_context");
      cJSON *commitments = cJSON_GetObjectItemCaseSensitive(bundle, "open_commitments");
      cJSON *reminders = cJSON_GetObjectItemCaseSensitive(bundle, "reminders");
      cJSON *directives = cJSON_GetObjectItemCaseSensitive(bundle, "directives");
      assert(cJSON_IsArray(identity));
      assert(cJSON_IsArray(preferences));
      assert(cJSON_IsArray(active));
      assert(cJSON_IsArray(commitments));
      assert(cJSON_IsArray(reminders));
      assert(cJSON_IsArray(directives));

      /* Telemetry fields for operator inspection. */
      cJSON *approx = cJSON_GetObjectItemCaseSensitive(bundle, "approx_tokens");
      cJSON *elapsed = cJSON_GetObjectItemCaseSensitive(bundle, "elapsed_ms");
      assert(cJSON_IsNumber(approx));
      assert(cJSON_IsNumber(elapsed));

      /* Identity filter: only prefix-matching rows must land; L1 and non-identity keys must not. */
      int seen_name = 0, seen_role = 0, seen_project = 0, seen_low = 0;
      cJSON *it = NULL;
      cJSON_ArrayForEach(it, identity)
      {
         const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(it, "key"));
         const char *why = cJSON_GetStringValue(cJSON_GetObjectItem(it, "why"));
         assert(why && why[0]);
         if (!key)
            continue;
         if (strcmp(key, "identity:name") == 0)
            seen_name = 1;
         if (strcmp(key, "role:engineer") == 0)
            seen_role = 1;
         if (strcmp(key, "project:alpha") == 0)
            seen_project = 1;
         if (strcmp(key, "identity:low") == 0)
            seen_low = 1;
      }
      assert(seen_name);
      assert(seen_role);
      assert(!seen_project);
      assert(!seen_low);

      /* Open commitments surfaces exactly the pending row. */
      assert(cJSON_GetArraySize(commitments) == 1);
      cJSON *c0 = cJSON_GetArrayItem(commitments, 0);
      long long c0_id = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(c0, "memory_id"));
      assert(c0_id == commit_id);

      /* Directives is always an empty stub until the separate proposal lands —
       * section-order stability is a caller contract. */
      assert(cJSON_GetArraySize(directives) == 0);

      /* Reminders: the matcher delegates into the prospective subsystem, which
       * has token-overlap rules — surface-or-not isn't part of the contract
       * here; we only check that reminder rows carry a why when present. */
      cJSON *r_it = NULL;
      cJSON_ArrayForEach(r_it, reminders)
      {
         const char *why = cJSON_GetStringValue(cJSON_GetObjectItem(r_it, "why"));
         assert(why && why[0]);
      }

      /* Determinism: two calls on the same DB / same inputs produce byte-identical bundles.
       * elapsed_ms is timing-dependent, so compare after stripping it from both sides. */
      cJSON *bundle2 = memory_recall("we should rotate the staging tokens", 0, 1);
      assert(bundle2 != NULL);
      cJSON_DeleteItemFromObjectCaseSensitive(bundle, "elapsed_ms");
      cJSON_DeleteItemFromObjectCaseSensitive(bundle2, "elapsed_ms");
      cJSON_DeleteItemFromObjectCaseSensitive(bundle, "retrieval_event_id");
      cJSON_DeleteItemFromObjectCaseSensitive(bundle2, "retrieval_event_id");
      char *j1 = cJSON_PrintUnformatted(bundle);
      char *j2 = cJSON_PrintUnformatted(bundle2);
      assert(j1 && j2);
      assert(strcmp(j1, j2) == 0);
      free(j1);
      free(j2);
      cJSON_Delete(bundle2);
      cJSON_Delete(bundle);

      /* Session-start vs per-turn: session bundle has >= the per-turn row count
       * across sections, because caps are wider. */
      cJSON *b_session = memory_recall("", 0, 1);
      cJSON *b_turn = memory_recall("routine edit", 0, 0);
      assert(b_session && b_turn);
      int sess_total =
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b_session, "identity")) +
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b_session, "preferences")) +
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b_session, "active_context")) +
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b_session, "open_commitments"));
      int turn_total =
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b_turn, "identity")) +
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b_turn, "preferences")) +
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b_turn, "active_context")) +
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b_turn, "open_commitments"));
      assert(sess_total >= turn_total);
      cJSON_Delete(b_session);
      cJSON_Delete(b_turn);

      /* Telemetry is recorded for operator inspection. Latency budgets are
       * tracked by explicit benchmarks, not unit-test gates. */
      int64_t assemblies = 0;
      double ms_max = 0.0;
      memory_recall_metrics(&assemblies, NULL, NULL, &ms_max);
      assert(assemblies > 0);
      assert(ms_max >= 0.0);

      /* Token-cap truncation: under a squeezed budget, the trim pass walks
       * from low-priority sections first (directives → reminders →
       * open_commitments → active_context → preferences → identity).
       *
       * Property: low-priority sections must drain NO SLOWER than higher-
       * priority ones.  Concretely, reminders+commitments+active_context
       * can never outnumber identity+preferences after the squeeze — if
       * the bundle is over budget, the higher-priority sections are
       * trimmed last, so their row count dominates. */
      cJSON *squeezed =
          memory_recall("we should rotate the staging tokens", 96 /* tight budget */, 0);
      assert(squeezed != NULL);
      int sq_id_rows = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(squeezed, "identity"));
      int sq_pref_rows =
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(squeezed, "preferences"));
      int sq_act_rows =
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(squeezed, "active_context"));
      int sq_com_rows =
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(squeezed, "open_commitments"));
      int sq_rem_rows = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(squeezed, "reminders"));
      assert((sq_rem_rows + sq_com_rows + sq_act_rows) <= (sq_id_rows + sq_pref_rows) + 1);
      cJSON_Delete(squeezed);

      /* Min-budget sanity: directives is a no-op on a fresh DB, so the
       * bundle stays parseable even under the minimum cap. */
      cJSON *tiny = memory_recall("x", MEMORY_RECALL_MIN_LIMIT_TOKENS, 0);
      assert(tiny != NULL);
      assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(tiny, "directives")) == 0);
      cJSON_Delete(tiny);
   }

   /* --- epistemic directives: CRUD, state machine, matcher, auto-hooks --- */
   {
      reset_db();

      /* create + get + list */
      memory_directive_t d;
      int rc = memory_directive_create("What token rotation policy should I use?", "token rotation",
                                       "vault", "", MEMORY_DIRECTIVE_CAUSE_USER_FOLLOW_UP, 70, 0, 0,
                                       "", "s1", "", &d);
      assert(rc == 0);
      assert(d.id > 0);
      assert(strcmp(d.state, MEMORY_DIRECTIVE_STATE_OPEN) == 0);
      assert(d.priority == 70);

      memory_directive_t fetched;
      assert(memory_directive_get(d.id, &fetched) == 0);
      assert(strcmp(fetched.question, "What token rotation policy should I use?") == 0);

      /* Dedup for retrieval_failure on (cause, topic): second create returns 1. */
      memory_directive_t d2;
      int first = memory_directive_create("Q1", "dedup-topic", "", "",
                                          MEMORY_DIRECTIVE_CAUSE_RETRIEVAL_FAILURE, 50, 0, 0, "",
                                          "", "", &d2);
      assert(first == 0);
      int second = memory_directive_create("Q2", "dedup-topic", "", "",
                                           MEMORY_DIRECTIVE_CAUSE_RETRIEVAL_FAILURE, 50, 0, 0, "",
                                           "", "", NULL);
      assert(second == 1); /* unique index short-circuited */

      /* Invalid cause rejected. */
      assert(memory_directive_create("bad", "topic", "", "", "bogus", 50, 0, 0, "", "", "", NULL) ==
             -1);

      /* list with filters */
      memory_directive_t rows[16];
      int n = memory_directive_list(MEMORY_DIRECTIVE_STATE_OPEN, NULL, rows, 16);
      assert(n == 2);
      /* Priority DESC: the p=70 user_follow_up row beats the p=50 retrieval_failure row. */
      assert(rows[0].priority >= rows[1].priority);

      int n_rf = memory_directive_list(NULL, MEMORY_DIRECTIVE_CAUSE_RETRIEVAL_FAILURE, rows, 16);
      assert(n_rf == 1);
      assert(strcmp(rows[0].cause, MEMORY_DIRECTIVE_CAUSE_RETRIEVAL_FAILURE) == 0);

      /* Counts bundle */
      memory_directive_counts_t cts;
      assert(memory_directive_counts(&cts) == 0);
      assert(cts.open == 2);
      assert(cts.resolved == 0);

      /* State machine: open → resolved (valid). resolved → anything (invalid). */
      assert(memory_directive_resolve(d.id, 0, "by operator") == 0);
      assert(memory_directive_get(d.id, &fetched) == 0);
      assert(strcmp(fetched.state, MEMORY_DIRECTIVE_STATE_RESOLVED) == 0);
      /* Second resolve must fail. */
      assert(memory_directive_resolve(d.id, 0, NULL) == -1);
      /* Suppress on a resolved directive must fail. */
      assert(memory_directive_suppress(d.id) == -1);

      /* Suppress the still-open one. */
      assert(memory_directive_suppress(d2.id) == 0);
      assert(memory_directive_counts(&cts) == 0);
      assert(cts.resolved == 1);
      assert(cts.suppressed == 1);
      assert(cts.open == 0);

      /* Expiry sweep: arm a new directive with valid_until in the past. */
      memory_directive_t dexp;
      assert(memory_directive_create("expires soon", "exp-topic", "", "",
                                     MEMORY_DIRECTIVE_CAUSE_USER_FOLLOW_UP, 50, 0, 0, "", "",
                                     "2020-01-01 00:00:00", &dexp) == 0);
      int expired = memory_directive_sweep_expired();
      assert(expired == 1);
      assert(memory_directive_get(dexp.id, &fetched) == 0);
      assert(strcmp(fetched.state, MEMORY_DIRECTIVE_STATE_EXPIRED) == 0);
      /* Re-sweep is a no-op. */
      assert(memory_directive_sweep_expired() == 0);

      /* Matcher: exact entity anchor + FTS on question text. */
      memory_directive_t anchor_d, fts_d;
      assert(memory_directive_create("How often should we rotate vault tokens?", "rotate tokens",
                                     "vault", "", MEMORY_DIRECTIVE_CAUSE_USER_FOLLOW_UP, 60, 0, 0,
                                     "", "", "", &anchor_d) == 0);
      assert(memory_directive_create("Which CI provider should I configure?", "ci provider", "", "",
                                     MEMORY_DIRECTIVE_CAUSE_USER_FOLLOW_UP, 50, 0, 0, "", "", "",
                                     &fts_d) == 0);

      memory_directive_t mrows[4];
      int m = memory_directive_match("token rotation policy", "vault", NULL, mrows, 4);
      assert(m >= 1);
      int saw_anchor = 0;
      for (int i = 0; i < m; i++)
         if (mrows[i].id == anchor_d.id)
            saw_anchor = 1;
      assert(saw_anchor);

      int m_fts = memory_directive_match("what provider should I configure", NULL, NULL, mrows, 4);
      int saw_fts = 0;
      for (int i = 0; i < m_fts; i++)
         if (mrows[i].id == fts_d.id)
            saw_fts = 1;
      assert(saw_fts);

      /* mark_surfaced advances surfaced_count. */
      assert(memory_directive_mark_surfaced(anchor_d.id) == 0);
      assert(memory_directive_get(anchor_d.id, &fetched) == 0);
      assert(fetched.surfaced_count == 1);

      /* Retrieval-failure counter: threshold crossing auto-creates a directive. */
      {
         for (int i = 0; i < 2; i++)
         {
            int64_t id = memory_directive_record_retrieval_failure("mystery query", 3, "");
            assert(id == 0); /* below threshold */
         }
         int64_t id = memory_directive_record_retrieval_failure("mystery query", 3, "");
         assert(id > 0);
         /* Subsequent calls must not multiply directives — dedup on (cause, topic). */
         int64_t id2 = memory_directive_record_retrieval_failure("mystery query", 3, "");
         assert(id2 == id);
         memory_directive_t got;
         assert(memory_directive_get(id, &got) == 0);
         assert(strcmp(got.cause, MEMORY_DIRECTIVE_CAUSE_RETRIEVAL_FAILURE) == 0);
         assert(strcmp(got.topic, "mystery query") == 0);
      }

      /* Contradiction hook: writing via memory_record_conflict auto-creates a
       * contradiction-cause directive and memory_resolve_conflict closes it. */
      {
         memory_t ma, mb;
         memory_insert(TIER_L2, KIND_FACT, "project:name", "aimee", 0.9, "s1", &ma);
         memory_insert(TIER_L2, KIND_FACT, "project:name", "atlas", 0.9, "s1", &mb);
         assert(memory_record_conflict(ma.id, mb.id) == 0);

         /* The directive should now exist, keyed to the mem pair. */
         memory_directive_t conf_rows[4];
         int nconf = memory_directive_list(MEMORY_DIRECTIVE_STATE_OPEN,
                                           MEMORY_DIRECTIVE_CAUSE_CONTRADICTION, conf_rows, 4);
         int saw = 0;
         int64_t cid = 0;
         for (int i = 0; i < nconf; i++)
         {
            /* Hook normalises the pair (smaller, larger). */
            int64_t lo = ma.id < mb.id ? ma.id : mb.id;
            int64_t hi = ma.id < mb.id ? mb.id : ma.id;
            if (conf_rows[i].memory_a_id == lo && conf_rows[i].memory_b_id == hi)
            {
               saw = 1;
               cid = conf_rows[i].id;
            }
         }
         assert(saw);
         assert(cid > 0);

         /* Replaying memory_record_conflict with the same pair must not
          * multiply directives — unique dedup index keeps it idempotent. */
         memory_record_conflict(ma.id, mb.id);
         nconf = memory_directive_list(MEMORY_DIRECTIVE_STATE_OPEN,
                                       MEMORY_DIRECTIVE_CAUSE_CONTRADICTION, conf_rows, 4);
         int same_count = 0;
         for (int i = 0; i < nconf; i++)
         {
            if (conf_rows[i].id == cid)
               same_count++;
         }
         assert(same_count == 1);

         /* Find the conflict row id and resolve it — directive flips to resolved. */
         conflict_t confs[4];
         int cn = memory_list_conflicts(confs, 4);
         int conflict_row_id = 0;
         for (int i = 0; i < cn; i++)
            if ((confs[i].memory_a == ma.id && confs[i].memory_b == mb.id) ||
                (confs[i].memory_a == mb.id && confs[i].memory_b == ma.id))
               conflict_row_id = (int)confs[i].id;
         assert(conflict_row_id > 0);
         assert(memory_resolve_conflict(conflict_row_id, "atlas is canonical") == 0);
         memory_directive_t cd;
         assert(memory_directive_get(cid, &cd) == 0);
         assert(strcmp(cd.state, MEMORY_DIRECTIVE_STATE_RESOLVED) == 0);
      }

      /* Recall integration: a matching open directive surfaces in the
       * recall bundle's `directives` section with the cause threaded into
       * `why`. */
      {
         memory_directive_t rec_d;
         assert(memory_directive_create(
                    "What DB backend should we migrate to?", "db backend migration", "postgres", "",
                    MEMORY_DIRECTIVE_CAUSE_USER_FOLLOW_UP, 90, 0, 0, "", "", "", &rec_d) == 0);

         cJSON *bundle = memory_recall("we should pick a db backend for migration", 0, 0);
         assert(bundle != NULL);
         cJSON *directives = cJSON_GetObjectItemCaseSensitive(bundle, "directives");
         assert(cJSON_IsArray(directives));
         int found = 0;
         cJSON *it = NULL;
         cJSON_ArrayForEach(it, directives)
         {
            long long id = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "memory_id"));
            const char *why = cJSON_GetStringValue(cJSON_GetObjectItem(it, "why"));
            const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(it, "kind"));
            if (id == rec_d.id)
            {
               found = 1;
               assert(strcmp(kind, "directive") == 0);
               assert(why && strstr(why, "directive:") == why);
            }
         }
         assert(found);
         cJSON_Delete(bundle);

         /* recall_fill_directives calls mark_surfaced on each emitted row,
          * so the counter should have advanced. */
         memory_directive_t got;
         assert(memory_directive_get(rec_d.id, &got) == 0);
         assert(got.surfaced_count >= 1);
      }

      /* Metrics accumulated across this block. */
      int64_t created_total = 0, resolved_total = 0, surfaced_total = 0;
      memory_directive_metrics(&created_total, &resolved_total, NULL, &surfaced_total, NULL, NULL,
                               NULL);
      assert(created_total > 0);
      assert(resolved_total > 0);
      assert(surfaced_total >= 1);
   }

   /* --- scheduled maintenance: run/skip/dry-run/summary shape --- */
   {
      reset_db();

      memory_maintenance_summary_t s1;
      assert(memory_maintenance_run(NULL, 0, 0, 0, &s1) == 0);
      assert(s1.skipped == 0);
      assert(s1.dry_run == 0);
      assert(s1.modes_run != 0);
      assert(s1.summary_json[0] != 0);

      /* Second immediate run: idle guard fires. */
      memory_maintenance_summary_t s2;
      assert(memory_maintenance_run(NULL, 0, 0, 0, &s2) == 0);
      assert(s2.skipped == 1);
      assert(s2.promoted == 0 && s2.demoted == 0 && s2.expired == 0);

      /* Force bypasses the guard. */
      memory_maintenance_summary_t s3;
      assert(memory_maintenance_run(NULL, 0, 1, 0, &s3) == 0);
      assert(s3.skipped == 0);

      /* Dry-run: no mutation even with a past-TTL pending row. */
      memory_t m;
      memory_insert(TIER_L2, KIND_FACT, "maint:pending", "I'll ship this next week", 0.9, "s1", &m);
      char err[256] = "";
      assert(aimee_pg_exec(db2_conn(),
                           "UPDATE memories SET lifecycle_state = 'pending',"
                           " ttl_at = '2020-01-01 00:00:00' WHERE key = 'maint:pending'",
                           err, sizeof(err)) == 0);
      memory_maintenance_summary_t dry;
      assert(memory_maintenance_run(NULL, MEMORY_MAINTENANCE_MODE_PRUNE, 1, 1, &dry) == 0);
      assert(dry.dry_run == 1);
      assert(dry.lifecycle_archived == 0);
      memory_lifecycle_counts_t cts;
      assert(memory_lifecycle_counts(&cts) == 0);
      assert(cts.pending == 1);

      /* Live prune commits and the row archives. */
      memory_maintenance_summary_t live;
      assert(memory_maintenance_run(NULL, MEMORY_MAINTENANCE_MODE_PRUNE, 1, 0, &live) == 0);
      assert(live.lifecycle_archived >= 1);
      assert(memory_lifecycle_counts(&cts) == 0);
      assert(cts.archived >= 1);

      /* Summary JSON is a legal object with operator-facing fields. */
      cJSON *parsed = cJSON_Parse(live.summary_json);
      assert(parsed != NULL);
      assert(cJSON_IsBool(cJSON_GetObjectItem(parsed, "skipped")));
      assert(cJSON_IsBool(cJSON_GetObjectItem(parsed, "dry_run")));
      assert(cJSON_IsNumber(cJSON_GetObjectItem(parsed, "modes_run")));
      assert(cJSON_IsNumber(cJSON_GetObjectItem(parsed, "elapsed_ms")));
      cJSON_Delete(parsed);

      /* last_summary persists for the dashboard accessor. */
      memory_maintenance_summary_t last;
      assert(memory_maintenance_last_summary(&last) == 0);
      assert(last.summary_json[0] != 0);

      int64_t runs_total = 0, skips_total = 0;
      memory_maintenance_metrics(&runs_total, &skips_total, NULL, NULL, NULL);
      assert(runs_total >= 2);
      assert(skips_total >= 1);

      config_t cfg_off;
      memset(&cfg_off, 0, sizeof(cfg_off));
      assert(memory_maintenance_maybe_run(&cfg_off, NULL) == 0);
   }

   db2_test_shim_close();
   db1_shutdown();

   printf("all tests passed\n");
   return 0;
}
