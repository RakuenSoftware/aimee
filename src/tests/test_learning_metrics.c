/* test_learning_metrics.c: commit_ratio + per-sink-cap + ingest-
 * latency telemetry for the phase-2 learning-router metrics. Uses an
 * in-memory DB seeded through the public learning API so the
 * assertions exercise the same path operator tooling sees.
 *
 * See docs/proposals/done/learning-signals-router-phase-2.md. */

#include <assert.h>
#include "db.h"
#include "db1.h"
#include "db2.h"
#include "db2_test_shim.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "aimee.h"
#include <aimee/learning/learning.h>
#include "modules/learning/learning_signal_policy.h"

static int test_signal_classifier(const char *signal, uint32_t *sink_mask)
{
   return learning_signal_policy_sink_mask(signal, sink_mask);
}

static int failing_signal_classifier(const char *signal, uint32_t *sink_mask)
{
   (void)signal;
   (void)sink_mask;
   return -1;
}

static void seed_signal(const char *signal_type, const char *title, const char *description)
{
   learning_signal_input_t input;
   memset(&input, 0, sizeof(input));
   snprintf(input.signal_type, sizeof(input.signal_type), "%s", signal_type);
   snprintf(input.source, sizeof(input.source), "%s", "test");
   snprintf(input.polarity, sizeof(input.polarity), "%s", "positive");
   snprintf(input.title, sizeof(input.title), "%s", title);
   snprintf(input.description, sizeof(input.description), "%s", description);
   input.high_confidence = 1;
   learning_dispatch_result_t result = {0};
   int rc = learning_router_record_signal(&input, &result);
   assert(rc > 0);
}

int main(void)
{
   printf("learning_metrics: ");

   assert(db1_init(":memory:") == 0);
   db2_test_shim_open();

   /* The production router must not classify or persist a signal without the
    * separately supervised learning stage. */
   {
      learning_signal_input_t input = {0};
      learning_dispatch_result_t result;
      memset(&result, 0x7f, sizeof(result));
      snprintf(input.signal_type, sizeof(input.signal_type), "%s", "thumb_up");
      learning_router_register_signal_classifier(NULL);
      assert(learning_router_record_signal(&input, &result) == -1);
      assert(result.signal_id == 0 && result.proposal_count == 0 && result.committed_count == 0);
      memset(&result, 0x7f, sizeof(result));
      learning_router_register_signal_classifier(failing_signal_classifier);
      assert(learning_router_record_signal(&input, &result) == -1);
      assert(result.signal_id == 0 && result.proposal_count == 0 && result.committed_count == 0);
   }
   learning_router_register_signal_classifier(test_signal_classifier);

   /* --- empty DB: commit_ratio is 0, per-sink utilization all 0 --- */
   {
      learning_commit_ratio_t cr = {0};
      assert(learning_metrics_commit_ratio(7, &cr) == 0);
      assert(cr.window_days == 7);
      assert(cr.proposals_terminal == 0);
      assert(cr.proposals_committed == 0);
      assert(cr.commit_ratio == 0.0);

      learning_sink_cap_t caps[LEARNING_SINK_COUNT] = {{{0}, 0, 0, 0.0}};
      int rows = learning_metrics_per_sink_caps(caps, LEARNING_SINK_COUNT);
      assert(rows == LEARNING_SINK_COUNT);
      /* Canonical order: reranker, supersede, rule, workflow. */
      assert(strcmp(caps[0].sink, "reranker") == 0);
      assert(strcmp(caps[1].sink, "supersede") == 0);
      assert(strcmp(caps[2].sink, "rule") == 0);
      assert(strcmp(caps[3].sink, "workflow") == 0);
      for (int i = 0; i < LEARNING_SINK_COUNT; i++)
      {
         assert(caps[i].committed_this_week == 0);
         assert(caps[i].weekly_cap > 0);
         assert(caps[i].utilization == 0.0);
      }
   }

   /* --- default window fallback --- */
   {
      learning_commit_ratio_t cr = {0};
      assert(learning_metrics_commit_ratio(0, &cr) == 0);
      assert(cr.window_days == LEARNING_METRICS_DEFAULT_WINDOW_DAYS);
      assert(learning_metrics_commit_ratio(-5, &cr) == 0);
      assert(cr.window_days == LEARNING_METRICS_DEFAULT_WINDOW_DAYS);
   }

   /* --- reset + record signals so ingest latency has observations.
    *     Explicit `mark_rule` high-confidence signals auto-commit to
    *     the rule sink, which lets us assert the cap-utilization
    *     path covers at least one sink. --- */
   learning_router_metrics_reset();
   {
      int64_t calls_before = 0;
      learning_router_metrics(&calls_before, NULL, NULL);
      assert(calls_before == 0);
   }

   seed_signal("mark_rule", "prefer absolute paths",
               "always use absolute paths when editing files");
   seed_signal("mark_rule", "link-only in PR body",
               "PR bodies may link to proposals, never inline them");
   /* A thumb signal goes to reranker but won't auto-commit without a
    * target_memory_id, so it lands as pending and contributes to
    * neither the commit_ratio numerator nor the committed-this-week
    * count — it exists purely to exercise the second ingest branch. */
   seed_signal("thumb_up", "liked the suggestion", "");

   /* --- ingest latency metrics are populated --- */
   {
      int64_t calls = 0;
      double avg = 0.0, max = 0.0;
      learning_router_metrics(&calls, &avg, &max);
      assert(calls == 3);
      /* Latency is measured in ms; real values are always > 0 when
       * we made it through the insert + dispatch path. */
      assert(avg >= 0.0);
      assert(max >= avg);
   }

   /* --- commit_ratio: two mark_rule signals should produce two
    *     committed rule-sink proposals (high_confidence auto-commits).
    *     The thumb_up lands as pending and doesn't count toward
    *     commit_ratio (which is settled-only). --- */
   {
      learning_commit_ratio_t cr = {0};
      assert(learning_metrics_commit_ratio(7, &cr) == 0);
      assert(cr.proposals_committed == 2);
      assert(cr.proposals_terminal == 2);
      /* ratio is exactly 1.0 when all settled proposals committed. */
      assert(cr.commit_ratio > 0.99 && cr.commit_ratio <= 1.0);
   }

   /* --- per-sink caps reflect the rule-sink commits --- */
   {
      learning_sink_cap_t caps[LEARNING_SINK_COUNT] = {{{0}, 0, 0, 0.0}};
      int rows = learning_metrics_per_sink_caps(caps, LEARNING_SINK_COUNT);
      assert(rows == LEARNING_SINK_COUNT);
      /* rule sink at index 2 picked up the two commits. */
      assert(caps[2].committed_this_week == 2);
      assert(caps[2].weekly_cap > 0);
      assert(caps[2].utilization > 0.0);
      /* Other sinks untouched — reranker had a pending thumb_up but
       * nothing committed. */
      assert(caps[0].committed_this_week == 0);
      assert(caps[1].committed_this_week == 0);
      assert(caps[3].committed_this_week == 0);
   }

   /* --- reset clears the ingest counters but not the DB rows --- */
   learning_router_metrics_reset();
   {
      int64_t calls = 0;
      double avg = 0.0, max = 0.0;
      learning_router_metrics(&calls, &avg, &max);
      assert(calls == 0);
      assert(avg == 0.0);
      assert(max == 0.0);
      /* DB-side commit_ratio survives the counter reset. */
      learning_commit_ratio_t cr = {0};
      assert(learning_metrics_commit_ratio(7, &cr) == 0);
      assert(cr.proposals_committed == 2);
   }

   /* --- NULL guards --- */
   assert(learning_metrics_commit_ratio(7, NULL) == -1);
   assert(learning_metrics_per_sink_caps(NULL, 0) == -1);

   /* --- detection-latency triple starts zeroed --- */
   learning_router_detection_metrics_reset();
   {
      int64_t calls = 0;
      double avg = 0.0, max = 0.0;
      learning_router_detection_metrics(&calls, &avg, &max);
      assert(calls == 0);
      assert(avg == 0.0);
      assert(max == 0.0);
   }

   /* --- recording a detection sample populates the triple --- */
   learning_router_record_detection_ms(3.5);
   learning_router_record_detection_ms(7.0);
   {
      int64_t calls = 0;
      double avg = 0.0, max = 0.0;
      learning_router_detection_metrics(&calls, &avg, &max);
      assert(calls == 2);
      assert(avg > 5.0 && avg < 6.0); /* (3.5 + 7.0) / 2 = 5.25 */
      assert(max > 6.9 && max < 7.1);
   }

   /* --- NULL-pointer guards for detection triple --- */
   learning_router_detection_metrics(NULL, NULL, NULL);

   /* --- reset clears detection counters independently of ingest --- */
   learning_router_detection_metrics_reset();
   {
      int64_t calls = 0;
      double avg = 0.0, max = 0.0;
      learning_router_detection_metrics(&calls, &avg, &max);
      assert(calls == 0);
      assert(avg == 0.0);
      assert(max == 0.0);
   }

   db2_test_shim_close();
   db1_shutdown();
   printf("all tests passed\n");
   return 0;
}
