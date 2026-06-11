/* test_roundtable_pipeline_eval.c: pure outer-loop decision logic (P1).
 * Covers the single canonical validity predicate (#41), the three done-bars
 * (#38/#14), chunk-aggregate (#28), and the loop-decide state machine
 * (pass/revise/retry/escalate, including pass-ceiling + cost backstops). */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "roundtable_pipeline.h"
#include "roundtable_pipeline_eval.h"

/* A baseline valid REVIEW envelope: captured, parsed, converged, no blocking. */
static rtp_envelope_t base_valid(void)
{
   rtp_envelope_t e;
   memset(&e, 0, sizeof(e));
   e.present = 1;
   e.parse_ok = 1;
   e.converged = 1;
   e.items_round = 2;
   e.artifact_round = 2;
   e.best_round = 2;
   e.rounds_run = 2;
   e.cost_usd = 0.10;
   e.cost_known = 1;
   return e;
}

static void test_validity_predicate(void)
{
   rtp_envelope_t e = base_valid();
   assert(roundtable_terminal_envelope_valid(&e) == 1);

   /* Each invalidity flag, tripped in turn, must make the predicate false (#41). */
   struct
   {
      const char *name;
      int *flag;
   } flags[] = {
       {"truncated", &e.truncated},   {"items_truncated", &e.items_truncated},
       {"degraded", &e.degraded},     {"cost_capped", &e.cost_capped},
       {"deadline_hit", &e.deadline_hit}, {"cancelled", &e.cancelled},
       {"lost_result", &e.lost_result},
   };
   for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
   {
      e = base_valid();
      *(flags[i].flag) = 1;
      /* re-resolve pointer after reset: recompute against a fresh copy */
      rtp_envelope_t t = base_valid();
      /* set the same named flag on t */
      if (strcmp(flags[i].name, "truncated") == 0)
         t.truncated = 1;
      else if (strcmp(flags[i].name, "items_truncated") == 0)
         t.items_truncated = 1;
      else if (strcmp(flags[i].name, "degraded") == 0)
         t.degraded = 1;
      else if (strcmp(flags[i].name, "cost_capped") == 0)
         t.cost_capped = 1;
      else if (strcmp(flags[i].name, "deadline_hit") == 0)
         t.deadline_hit = 1;
      else if (strcmp(flags[i].name, "cancelled") == 0)
         t.cancelled = 1;
      else if (strcmp(flags[i].name, "lost_result") == 0)
         t.lost_result = 1;
      assert(roundtable_terminal_envelope_valid(&t) == 0);
   }

   /* not present / parse failure / error */
   e = base_valid();
   e.present = 0;
   assert(roundtable_terminal_envelope_valid(&e) == 0);
   e = base_valid();
   e.parse_ok = 0;
   assert(roundtable_terminal_envelope_valid(&e) == 0);
   e = base_valid();
   e.has_error = 1;
   assert(roundtable_terminal_envelope_valid(&e) == 0);

   /* items/artifact round provenance mismatch (#41) */
   e = base_valid();
   e.artifact_round = 3; /* != items_round 2 */
   assert(roundtable_terminal_envelope_valid(&e) == 0);

   /* draft: valid iff artifact present */
   e = base_valid();
   e.is_draft = 1;
   e.artifact_present = 0;
   assert(roundtable_terminal_envelope_valid(&e) == 0);
   e.artifact_present = 1;
   assert(roundtable_terminal_envelope_valid(&e) == 1);
   printf("  validity predicate: ok\n");
}

static void test_done_bars(void)
{
   /* zero_blocking: met when converged + no blocking. */
   rtp_envelope_t e = base_valid();
   assert(rtp_donebar_eval(RTP_DONEBAR_ZERO_BLOCKING, &e, 0) == RTP_DONEBAR_MET);
   e.blocking_count = 1;
   assert(rtp_donebar_eval(RTP_DONEBAR_ZERO_BLOCKING, &e, 0) == RTP_DONEBAR_BLOCKED);

   /* not converged -> blocked even with no items. */
   e = base_valid();
   e.converged = 0;
   assert(rtp_donebar_eval(RTP_DONEBAR_ZERO_BLOCKING, &e, 0) == RTP_DONEBAR_BLOCKED);

   /* suggestions block under bar 2 but not bar 1. */
   e = base_valid();
   e.suggestion_count = 2;
   assert(rtp_donebar_eval(RTP_DONEBAR_ZERO_BLOCKING, &e, 0) == RTP_DONEBAR_MET);
   assert(rtp_donebar_eval(RTP_DONEBAR_ZERO_BLOCKING_SUGGESTIONS, &e, 0) == RTP_DONEBAR_BLOCKED);

   /* questions-answered bar (#14). */
   e = base_valid();
   e.answered_count = 3;
   e.coverage_gap_count = 0;
   assert(rtp_donebar_eval(RTP_DONEBAR_ZERO_BLOCKING_QUESTIONS, &e, 3) == RTP_DONEBAR_MET);
   /* unanswered */
   assert(rtp_donebar_eval(RTP_DONEBAR_ZERO_BLOCKING_QUESTIONS, &e, 4) == RTP_DONEBAR_BLOCKED);
   /* coverage gap blocks */
   e.coverage_gap_count = 1;
   assert(rtp_donebar_eval(RTP_DONEBAR_ZERO_BLOCKING_QUESTIONS, &e, 3) == RTP_DONEBAR_BLOCKED);
   /* >16 accepted questions is invalid, never silently answered (#14). */
   e = base_valid();
   e.answered_count = 17;
   assert(rtp_donebar_eval(RTP_DONEBAR_ZERO_BLOCKING_QUESTIONS, &e, 17) == RTP_DONEBAR_INVALID);

   /* invalid envelope -> INVALID regardless of bar. */
   e = base_valid();
   e.degraded = 1;
   assert(rtp_donebar_eval(RTP_DONEBAR_ZERO_BLOCKING, &e, 0) == RTP_DONEBAR_INVALID);
   printf("  done-bars: ok\n");
}

static void test_chunk_aggregate(void)
{
   assert(rtp_chunk_aggregate_done(3, 3, 1, 0) == 1);
   assert(rtp_chunk_aggregate_done(3, 2, 1, 0) == 0); /* not all chunks done */
   assert(rtp_chunk_aggregate_done(3, 3, 0, 0) == 0); /* no synthesis */
   assert(rtp_chunk_aggregate_done(3, 3, 1, 1) == 0); /* a chunk invalid */
   assert(rtp_chunk_aggregate_done(0, 0, 1, 0) == 0); /* no chunks */
   printf("  chunk aggregate: ok\n");
}

static void test_loop_decide(void)
{
   rtp_loop_cfg_t cfg = {RTP_DONEBAR_ZERO_BLOCKING, 0, 2, 0.0};
   rtp_loop_state_t st = {1, 1, 0.0, 0};

   /* met -> pass */
   rtp_envelope_t e = base_valid();
   assert(rtp_loop_decide(&cfg, &st, &e) == RTP_ACT_PASS);

   /* blocked + unbounded passes -> revise */
   e.blocking_count = 1;
   assert(rtp_loop_decide(&cfg, &st, &e) == RTP_ACT_REVISE);

   /* infra fault, attempts remaining -> retry */
   e = base_valid();
   e.present = 0; /* nothing captured */
   st.attempt_no = 1;
   assert(rtp_loop_decide(&cfg, &st, &e) == RTP_ACT_RETRY);
   /* attempts exhausted -> escalate */
   st.attempt_no = 2;
   assert(rtp_loop_decide(&cfg, &st, &e) == RTP_ACT_ESCALATE);

   /* captured-but-invalid engine evidence -> escalate (not an infra retry). */
   e = base_valid();
   e.degraded = 1;
   st.attempt_no = 1;
   assert(rtp_loop_decide(&cfg, &st, &e) == RTP_ACT_ESCALATE);

   /* pass ceiling reached while blocked -> escalate, never auto-pass. */
   rtp_loop_cfg_t capped = {RTP_DONEBAR_ZERO_BLOCKING, 3, 2, 0.0};
   rtp_loop_state_t at_cap = {3, 1, 0.0, 0};
   e = base_valid();
   e.blocking_count = 1;
   assert(rtp_loop_decide(&capped, &at_cap, &e) == RTP_ACT_ESCALATE);
   /* below the ceiling -> revise. */
   rtp_loop_state_t below = {2, 1, 0.0, 0};
   assert(rtp_loop_decide(&capped, &below, &e) == RTP_ACT_REVISE);

   /* phase cost backstop while blocked -> escalate. */
   rtp_loop_cfg_t costcap = {RTP_DONEBAR_ZERO_BLOCKING, 0, 2, 1.0};
   rtp_loop_state_t spent = {1, 1, 1.5, 0};
   assert(rtp_loop_decide(&costcap, &spent, &e) == RTP_ACT_ESCALATE);
   printf("  loop decide: ok\n");
}

int main(void)
{
   test_validity_predicate();
   test_done_bars();
   test_chunk_aggregate();
   test_loop_decide();
   printf("test_roundtable_pipeline_eval: all passed\n");
   return 0;
}
