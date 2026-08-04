#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aimee.h"
#include "db.h"
#include "db1.h"
#include "db2.h"
#include "db2_test_shim.h"
#include "modules/learning/learning_signal_policy.h"
#include "platform_test_util.h"

static char g_learning_tmpdir[PATH_MAX];

static int test_signal_classifier(const char *signal, uint32_t *sink_mask)
{
   return learning_signal_policy_sink_mask(signal, sink_mask);
}

static void learning_test_cleanup_tmpdir(void)
{
   if (g_learning_tmpdir[0] && strchr(g_learning_tmpdir, 'X') == NULL)
      platform_test_rmrf(g_learning_tmpdir);
}

static void learning_test_isolate_home(void)
{
   static int initialized = 0;
   if (initialized)
      return;
   snprintf(g_learning_tmpdir, sizeof(g_learning_tmpdir), "%s/aimee-learning-test-XXXXXX",
            platform_tmpdir());
   assert(platform_mkdtemp(g_learning_tmpdir) != NULL);
   assert(platform_setenv("HOME", g_learning_tmpdir) == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
   atexit(learning_test_cleanup_tmpdir);
   initialized = 1;
}

static void test_db_open(void)
{
   db2_test_shim_close();
   assert(db1_init(":memory:") == 0);
   db2_test_shim_open();
}

static void test_db_close(void)
{
   db1_stmt_cache_clear();
   db2_test_shim_close();
   db1_shutdown();
}

int main(void)
{
   printf("feedback: ");
   learning_router_register_signal_classifier(test_signal_classifier);

   /* --- db2_feedback_parse_polarity --- */
   {
      assert(strcmp(db2_feedback_parse_polarity("+"), "positive") == 0);
      assert(strcmp(db2_feedback_parse_polarity("-"), "negative") == 0);
      assert(strcmp(db2_feedback_parse_polarity("positive"), "positive") == 0);
      assert(strcmp(db2_feedback_parse_polarity("negative"), "negative") == 0);
      assert(strcmp(db2_feedback_parse_polarity("principle"), "principle") == 0);
      assert(db2_feedback_parse_polarity(NULL) == NULL);
      assert(db2_feedback_parse_polarity("") == NULL);
      assert(db2_feedback_parse_polarity("unknown_value") == NULL);
   }

   /* --- feedback_record: create new rule --- */
   {
      test_db_open();

      int reinforced = 0;

      int id =
          db2_feedback_record("negative", "do not edit .env", "security risk", -1, &reinforced);
      assert(id > 0);
      assert(reinforced == 0);

      /* Verify rule exists */
      rule_t r;
      int rc = db2_rules_get(id, &r);
      assert(rc == 0);
      assert(strcmp(r.polarity, "negative") == 0);
      assert(strstr(r.title, "do not edit .env") != NULL);

      test_db_close();
   }

   /* --- feedback_record: reinforce existing rule --- */
   {
      test_db_open();

      int reinforced = 0;

      int id1 = db2_feedback_record("negative", "avoid force push", "", -1, &reinforced);
      assert(id1 > 0);
      assert(reinforced == 0);

      /* Record same feedback again */
      int id2 = db2_feedback_record("negative", "avoid force push", "", -1, &reinforced);
      assert(id2 == id1);
      assert(reinforced == 1);

      /* Weight should have increased */
      rule_t r;
      db2_rules_get(id1, &r);
      assert(r.weight > 0);

      test_db_close();
   }

   /* --- feedback_record: weight override --- */
   {
      test_db_open();

      int reinforced = 0;

      int id = db2_feedback_record("positive", "always run tests", "", 75, &reinforced);
      assert(id > 0);

      rule_t r;
      db2_rules_get(id, &r);
      assert(r.weight == 75);

      test_db_close();
   }

   /* --- learning router: first correction opens proposals only --- */
   {
      learning_test_isolate_home();
      test_db_open();

      memory_t old_fact;
      assert(memory_insert(TIER_L2, KIND_FACT, "capital:france", "Paris", 1.0, "sess-a",
                           &old_fact) == 0);

      learning_signal_input_t input;
      learning_dispatch_result_t dispatch;
      memset(&input, 0, sizeof(input));
      memset(&dispatch, 0, sizeof(dispatch));
      snprintf(input.signal_type, sizeof(input.signal_type), "%s", "correction");
      snprintf(input.source, sizeof(input.source), "%s", "explicit");
      snprintf(input.title, sizeof(input.title), "%s", "capital of france");
      snprintf(input.target_key, sizeof(input.target_key), "%s", "capital:france");
      input.target_memory_id = old_fact.id;
      snprintf(input.correction_text, sizeof(input.correction_text), "%s", "Lyon");
      input.evidence_refs_json = "[]";

      assert(learning_router_record_signal(&input, &dispatch) > 0);
      assert(dispatch.proposal_count >= 3);
      assert(dispatch.committed_count == 0);

      learning_proposal_t pending[8];
      int count = learning_list_proposals("pending", NULL, 8, pending, 8);
      int saw_reranker = 0, saw_supersede = 0, saw_rule = 0;
      assert(count >= 3);
      for (int i = 0; i < count; i++)
      {
         if (strcmp(pending[i].sink, "reranker") == 0)
            saw_reranker = 1;
         else if (strcmp(pending[i].sink, "supersede") == 0)
            saw_supersede = 1;
         else if (strcmp(pending[i].sink, "rule") == 0)
            saw_rule = 1;
         assert(strcmp(pending[i].state, "pending") == 0);
      }
      assert(saw_reranker && saw_supersede && saw_rule);

      test_db_close();
   }

   /* --- learning router: second correction corroborates and commits supersede --- */
   {
      learning_test_isolate_home();
      test_db_open();

      memory_t old_fact;
      assert(memory_insert(TIER_L2, KIND_FACT, "capital:spain", "Madrid", 1.0, "sess-a",
                           &old_fact) == 0);

      learning_signal_input_t input;
      memset(&input, 0, sizeof(input));
      snprintf(input.signal_type, sizeof(input.signal_type), "%s", "correction");
      snprintf(input.source, sizeof(input.source), "%s", "explicit");
      snprintf(input.target_key, sizeof(input.target_key), "%s", "capital:spain");
      input.target_memory_id = old_fact.id;
      snprintf(input.correction_text, sizeof(input.correction_text), "%s", "Barcelona");
      input.evidence_refs_json = "[]";

      learning_dispatch_result_t dispatch;
      memset(&dispatch, 0, sizeof(dispatch));
      assert(learning_router_record_signal(&input, &dispatch) > 0);
      memset(&dispatch, 0, sizeof(dispatch));
      assert(learning_router_record_signal(&input, &dispatch) > 0);
      assert(dispatch.committed_count >= 1);

      learning_proposal_t committed[8];
      int count = learning_list_proposals("committed", NULL, 8, committed, 8);
      int saw_supersede = 0;
      assert(count >= 1);
      for (int i = 0; i < count; i++)
         if (strcmp(committed[i].sink, "supersede") == 0)
            saw_supersede = 1;
      assert(saw_supersede);

      test_db_close();
   }

   /* --- learning accept/reject remain idempotent --- */
   {
      learning_test_isolate_home();
      test_db_open();

      learning_signal_input_t input;
      learning_dispatch_result_t dispatch;
      memset(&input, 0, sizeof(input));
      memset(&dispatch, 0, sizeof(dispatch));
      snprintf(input.signal_type, sizeof(input.signal_type), "%s", "preference_statement");
      snprintf(input.source, sizeof(input.source), "%s", "explicit");
      snprintf(input.description, sizeof(input.description), "%s",
               "Prefer concise commit messages");

      assert(learning_router_record_signal(&input, &dispatch) > 0);
      assert(dispatch.proposal_count == 1);

      learning_proposal_t proposal;
      assert(learning_reject_proposal(dispatch.proposal_ids[0], &proposal) == 0);
      assert(strcmp(proposal.state, "archived") == 0);
      assert(learning_reject_proposal(dispatch.proposal_ids[0], &proposal) == 0);
      assert(strcmp(proposal.state, "archived") == 0);

      memset(&input, 0, sizeof(input));
      memset(&dispatch, 0, sizeof(dispatch));
      snprintf(input.signal_type, sizeof(input.signal_type), "%s", "mark_rule");
      snprintf(input.source, sizeof(input.source), "%s", "explicit");
      snprintf(input.description, sizeof(input.description), "%s", "Always ask before rebasing");
      input.high_confidence = 1;

      assert(learning_router_record_signal(&input, &dispatch) > 0);
      assert(dispatch.proposal_count == 1);
      assert(learning_accept_proposal(dispatch.proposal_ids[0], &proposal) == 0);
      assert(strcmp(proposal.state, "committed") == 0);
      assert(learning_accept_proposal(dispatch.proposal_ids[0], &proposal) == 0);
      assert(strcmp(proposal.state, "committed") == 0);

      test_db_close();
   }

   printf("all tests passed\n");
   return 0;
}
