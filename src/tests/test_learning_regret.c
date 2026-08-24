/* test_learning_regret.c: post-commit regret and the bar it moves
 * (recursive self-improvement S5).
 *
 * The claim under test is not "we can store a fate" — it is that a detector
 * whose commits keep failing to hold gets a HARDER time on its next one, and
 * that a detector nobody has judged yet is treated exactly as before.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "db.h"
#include "db1.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_learning.h"
#include "modules/db2/c/db2_test_shim.h"
#include "modules/learning/learning_signal_policy.h"

#include <aimee/learning/learning.h>

static int classifier(const char *signal, uint32_t *mask)
{
   return learning_signal_policy_sink_mask(signal, mask);
}

/* Commit one proposal from `signal_type` and give it `fate`. Returns the
 * proposal id, or 0 when the router declined to commit. */
static int commit_with_fate(const char *signal_type, const char *target_key, const char *fate)
{
   learning_signal_input_t input;
   memset(&input, 0, sizeof(input));
   snprintf(input.signal_type, sizeof(input.signal_type), "%s", signal_type);
   snprintf(input.source, sizeof(input.source), "%s", "explicit");
   snprintf(input.polarity, sizeof(input.polarity), "%s", "positive");
   snprintf(input.title, sizeof(input.title), "%s", "t");
   snprintf(input.description, sizeof(input.description), "%s", target_key);
   snprintf(input.target_key, sizeof(input.target_key), "%s", target_key);
   input.high_confidence = 1;

   learning_dispatch_result_t result = {0};
   if (learning_router_record_signal(&input, &result) <= 0)
      return 0;
   if (result.committed_count == 0)
      return 0;
   int id = result.committed_ids[0];
   if (fate)
      assert(learning_fate_record(id, fate, "test") == 0);
   return id;
}

static void test_fate_vocabulary_is_closed(void)
{
   assert(learning_fate_is_valid(LEARNING_FATE_STANDING) == 1);
   assert(learning_fate_is_valid(LEARNING_FATE_SUPERSEDED) == 1);
   assert(learning_fate_is_valid(LEARNING_FATE_CONTRADICTED) == 1);
   assert(learning_fate_is_valid(LEARNING_FATE_REVERTED) == 1);
   assert(learning_fate_is_valid("standingg") == 0);
   assert(learning_fate_is_valid("") == 0);
   assert(learning_fate_is_valid(NULL) == 0);

   /* Standing is the only outcome that is not regret, and an unrecognised
    * fate is NOT regret — a typo must not silently raise a detector's bar. */
   assert(learning_fate_is_regret(LEARNING_FATE_STANDING) == 0);
   assert(learning_fate_is_regret(LEARNING_FATE_SUPERSEDED) == 1);
   assert(learning_fate_is_regret(LEARNING_FATE_CONTRADICTED) == 1);
   assert(learning_fate_is_regret(LEARNING_FATE_REVERTED) == 1);
   assert(learning_fate_is_regret("wat") == 0);
   assert(learning_fate_is_regret(NULL) == 0);

   /* An unknown fate is refused at the write boundary too. */
   assert(learning_fate_record(1, "wat", "r") == -1);
   assert(learning_fate_record(0, LEARNING_FATE_STANDING, "r") == -1);
   assert(learning_fate_record(-4, LEARNING_FATE_STANDING, "r") == -1);
}

static void test_unmeasured_detector_keeps_the_default_bar(void)
{
   /* Nothing has been judged, so nothing may move. This is the case that
    * governs every fresh installation. */
   assert(learning_detector_corroboration_required("mark_rule") == LEARNING_CORROBORATION_DEFAULT);
   assert(learning_detector_may_fast_commit("mark_rule") == 1);
   /* An unnamed detector is not a licence to guess. */
   assert(learning_detector_corroboration_required("") == LEARNING_CORROBORATION_DEFAULT);
   assert(learning_detector_may_fast_commit(NULL) == 1);
}

static void test_regret_raises_the_bar(void)
{
   /* A detector that is agreed with: every commit holds. */
   for (int i = 0; i < LEARNING_REGRET_MIN_SAMPLE + 2; i++)
   {
      char key[64];
      snprintf(key, sizeof(key), "good-%d", i);
      assert(commit_with_fate("mark_rule", key, LEARNING_FATE_STANDING) > 0);
   }

   learning_detector_regret_t rows[LEARNING_REGRET_MAX_DETECTORS];
   int n = learning_metrics_regret(7, rows, LEARNING_REGRET_MAX_DETECTORS);
   assert(n >= 1);
   int found = 0;
   for (int i = 0; i < n; i++)
      if (strcmp(rows[i].signal_type, "mark_rule") == 0)
      {
         found = 1;
         assert(rows[i].settled >= LEARNING_REGRET_MIN_SAMPLE);
         assert(rows[i].regret == 0);
         assert(rows[i].regret_rate == 0.0);
      }
   assert(found);

   /* Being right does not buy a lower bar. */
   assert(learning_detector_corroboration_required("mark_rule") == LEARNING_CORROBORATION_DEFAULT);
   assert(learning_detector_may_fast_commit("mark_rule") == 1);

   /* Now a detector that keeps being wrong: every commit gets reverted. Note
    * this loop runs exactly MIN_SAMPLE times — one more would not commit, and
    * that is the point asserted immediately below. */
   for (int i = 0; i < LEARNING_REGRET_MIN_SAMPLE; i++)
   {
      char key[64];
      snprintf(key, sizeof(key), "bad-%d", i);
      assert(commit_with_fate("preference_statement", key, LEARNING_FATE_REVERTED) > 0);
   }

   n = learning_metrics_regret(7, rows, LEARNING_REGRET_MAX_DETECTORS);
   found = 0;
   for (int i = 0; i < n; i++)
      if (strcmp(rows[i].signal_type, "preference_statement") == 0)
      {
         found = 1;
         assert(rows[i].settled == LEARNING_REGRET_MIN_SAMPLE);
         assert(rows[i].regret == rows[i].settled);
         assert(rows[i].regret_rate > LEARNING_REGRET_NO_FAST_RATE);
      }
   assert(found);

   /* Both controls tighten, and only for the detector that earned it. */
   assert(learning_detector_corroboration_required("preference_statement") ==
          LEARNING_CORROBORATION_RAISED);
   assert(learning_detector_may_fast_commit("preference_statement") == 0);
   assert(learning_detector_corroboration_required("mark_rule") == LEARNING_CORROBORATION_DEFAULT);
   assert(learning_detector_may_fast_commit("mark_rule") == 1);

   /* The actuation, not just the metric: the NEXT high-confidence signal from
    * the throttled detector no longer commits on sight. It becomes a pending
    * proposal that must now be corroborated like any other. */
   assert(commit_with_fate("preference_statement", "bad-next", NULL) == 0);

   /* And the well-behaved detector is untouched by its neighbour's record. */
   assert(commit_with_fate("mark_rule", "good-next", LEARNING_FATE_STANDING) > 0);
}

static void test_fate_is_one_verdict_per_proposal(void)
{
   int id = commit_with_fate("mark_rule", "revisited", LEARNING_FATE_STANDING);
   assert(id > 0);
   char fate[DB2_LEARNING_FATE_LEN] = "";
   assert(db2_learning_fate_get(id, fate, sizeof(fate)) == 1);
   assert(strcmp(fate, LEARNING_FATE_STANDING) == 0);

   /* A later verdict replaces the earlier one: the question is what BECAME of
    * it, not what we thought at each step. */
   assert(learning_fate_record(id, LEARNING_FATE_CONTRADICTED, "later evidence") == 0);
   assert(db2_learning_fate_get(id, fate, sizeof(fate)) == 1);
   assert(strcmp(fate, LEARNING_FATE_CONTRADICTED) == 0);

   /* A proposal nobody has judged has no fate, and that is distinct from
    * having a good one. */
   assert(db2_learning_fate_get(999999, fate, sizeof(fate)) == 0);
}

/* The producers. Before these existed the regret controls consumed a fate that
 * nothing ever wrote — the loop was complete except that no verdict was ever
 * entered, so regret was permanently zero and the bar never moved. */
static void test_a_second_commit_supersedes_the_first(void)
{
   /* Two commits to the SAME target: the second replaces the first, and the
    * first's fate says so without anyone being asked. */
   int first = commit_with_fate("mark_rule", "same-target", NULL);
   assert(first > 0);
   char fate[DB2_LEARNING_FATE_LEN] = "";
   assert(db2_learning_fate_get(first, fate, sizeof(fate)) == 0); /* no verdict yet */

   int second = commit_with_fate("mark_rule", "same-target", NULL);
   assert(second > 0);
   assert(second != first);
   assert(db2_learning_fate_get(first, fate, sizeof(fate)) == 1);
   assert(strcmp(fate, LEARNING_FATE_SUPERSEDED) == 0);
   /* The new one is not judged by its own arrival. */
   assert(db2_learning_fate_get(second, fate, sizeof(fate)) == 0);

   /* A commit to a DIFFERENT target supersedes nothing. */
   int other = commit_with_fate("mark_rule", "other-target", NULL);
   assert(other > 0);
   assert(db2_learning_fate_get(second, fate, sizeof(fate)) == 0);
}

static void test_rejecting_a_commit_is_regret(void)
{
   int id = commit_with_fate("mark_rule", "regretted-target", NULL);
   assert(id > 0);
   char fate[DB2_LEARNING_FATE_LEN] = "";
   assert(db2_learning_fate_get(id, fate, sizeof(fate)) == 0);

   /* A human looked at what the loop did and undid it. That is the clearest
    * regret signal there is, and it must be recorded without being asked. */
   learning_proposal_t p;
   assert(learning_reject_proposal(id, &p) == 0);
   assert(db2_learning_fate_get(id, fate, sizeof(fate)) == 1);
   assert(strcmp(fate, LEARNING_FATE_REVERTED) == 0);
   assert(learning_fate_is_regret(fate) == 1);
}

int main(void)
{
   printf("learning_regret: ");

   assert(db1_init(":memory:") == 0);
   db2_test_shim_open();
   learning_router_register_signal_classifier(classifier);

   test_fate_vocabulary_is_closed();
   test_unmeasured_detector_keeps_the_default_bar();
   test_regret_raises_the_bar();
   test_fate_is_one_verdict_per_proposal();
   test_a_second_commit_supersedes_the_first();
   test_rejecting_a_commit_is_regret();

   assert(learning_metrics_regret(7, NULL, 4) == -1);
   assert(learning_metrics_regret(7, (learning_detector_regret_t *)1, 0) == -1);

   printf("ok\n");
   return 0;
}
