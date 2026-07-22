/* test_rounds_to_resume.c: unit tests for post-compaction recovery accounting */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "rounds_to_resume.h"
#include "session_compact.h"

#define PASS(name) printf("  PASS: %s\n", name)

/* ------------------------------------------------------------------ helpers */

/* A compaction result carrying the given read-only calls as pre-boundary sigs. */
static void seed(session_compact_result_t *r, const char *const *names, const char *const *args,
                 int n)
{
   memset(r, 0, sizeof(*r));
   r->compacted = 1;
   for (int i = 0; i < n; i++)
   {
      char sig[SESSION_COMPACT_SIG_LEN];
      if (!session_compact_tool_sig(names[i], args[i], sig, sizeof(sig)))
         continue;
      snprintf(r->readonly_sigs[r->readonly_sig_count], SESSION_COMPACT_SIG_LEN, "%s", sig);
      r->readonly_sig_count++;
   }
}

static int turn(rtr_tracker_t *t, const char *name, const char *args)
{
   const char *names[1] = {name};
   const char *argv[1] = {args};
   return rtr_observe_turn(t, names, argv, 1);
}

/* ------------------------------------------------------------------ tests */

static void test_no_boundary_is_not_tracked(void)
{
   session_compact_result_t r;
   memset(&r, 0, sizeof(r));
   r.compacted = 0; /* pressure was fine; nothing was discarded */

   rtr_tracker_t t;
   rtr_begin(&t, &r);
   assert(t.active == 0);
   assert(turn(&t, "read_file", "{\"path\":\"a.c\"}") == 0);
   assert(t.rounds_to_resume == 0);
   PASS("no compaction -> nothing tracked");
}

static void test_immediate_progress_costs_zero_rounds(void)
{
   const char *n[] = {"read_file"};
   const char *a[] = {"{\"path\":\"a.c\"}"};
   session_compact_result_t r;
   seed(&r, n, a, 1);

   rtr_tracker_t t;
   rtr_begin(&t, &r);
   /* Straight back to work: the summary carried what was needed. */
   assert(turn(&t, "edit_file", "{\"path\":\"a.c\"}") == 1);
   assert(t.resolved == 1);
   assert(t.rounds_to_resume == 0);
   PASS("progress on the first post-boundary turn costs zero rounds");
}

static void test_rederivation_rounds_are_counted(void)
{
   const char *n[] = {"read_file", "grep"};
   const char *a[] = {"{\"path\":\"a.c\"}", "{\"pattern\":\"foo\"}"};
   session_compact_result_t r;
   seed(&r, n, a, 2);

   rtr_tracker_t t;
   rtr_begin(&t, &r);
   /* Two turns rebuilding what it already had... */
   assert(turn(&t, "read_file", "{\"path\":\"a.c\"}") == 0);
   assert(turn(&t, "grep", "{\"pattern\":\"foo\"}") == 0);
   /* ...then real work. */
   assert(turn(&t, "edit_file", "{\"path\":\"a.c\"}") == 1);

   assert(t.rounds_to_resume == 2);
   assert(t.rederived_calls == 2);
   PASS("re-derivation rounds are counted until progress");
}

/* THE false positive that would make the metric a lie: an agent re-running the
 * suite after a fix is working, not recovering. */
static void test_repeated_test_is_progress_not_rederivation(void)
{
   const char *n[] = {"test"};
   const char *a[] = {"{\"suite\":\"unit\"}"};
   session_compact_result_t r;
   seed(&r, n, a, 1);
   /* `test` is not read-only, so it never enters the basis in the first place. */
   assert(r.readonly_sig_count == 0);

   rtr_tracker_t t;
   rtr_begin(&t, &r);
   assert(turn(&t, "test", "{\"suite\":\"unit\"}") == 1); /* resolves as progress */
   assert(t.rounds_to_resume == 0);
   assert(t.rederived_calls == 0);
   PASS("a repeated test resolves as progress, never as a wasted round");
}

static void test_new_lookup_is_progress(void)
{
   const char *n[] = {"read_file"};
   const char *a[] = {"{\"path\":\"a.c\"}"};
   session_compact_result_t r;
   seed(&r, n, a, 1);

   rtr_tracker_t t;
   rtr_begin(&t, &r);
   /* Same tool, a file it had NOT read before: exploration, not replay. */
   assert(turn(&t, "read_file", "{\"path\":\"b.c\"}") == 1);
   assert(t.rounds_to_resume == 0);
   PASS("a read-only call the agent never made is new work, not re-derivation");
}

static void test_text_only_turn_is_not_a_round(void)
{
   const char *n[] = {"read_file"};
   const char *a[] = {"{\"path\":\"a.c\"}"};
   session_compact_result_t r;
   seed(&r, n, a, 1);

   rtr_tracker_t t;
   rtr_begin(&t, &r);
   assert(rtr_observe_turn(&t, NULL, NULL, 0) == 0); /* no tool calls */
   assert(t.rounds_to_resume == 0);
   assert(t.active == 1); /* still waiting to see what it does */
   PASS("a text-only turn is not a round");
}

static void test_mixed_turn_counts_as_progress(void)
{
   const char *n[] = {"read_file"};
   const char *a[] = {"{\"path\":\"a.c\"}"};
   session_compact_result_t r;
   seed(&r, n, a, 1);

   rtr_tracker_t t;
   rtr_begin(&t, &r);
   /* Re-reads a known file AND edits in the same turn: it did not stall. */
   const char *names[2] = {"read_file", "edit_file"};
   const char *argv[2] = {"{\"path\":\"a.c\"}", "{\"path\":\"a.c\"}"};
   assert(rtr_observe_turn(&t, names, argv, 2) == 1);
   assert(t.rounds_to_resume == 0);
   assert(t.rederived_calls == 1); /* the replay is still recorded */
   PASS("a turn mixing replay with real work counts as progress");
}

static void test_resolves_once(void)
{
   const char *n[] = {"read_file"};
   const char *a[] = {"{\"path\":\"a.c\"}"};
   session_compact_result_t r;
   seed(&r, n, a, 1);

   rtr_tracker_t t;
   rtr_begin(&t, &r);
   assert(turn(&t, "read_file", "{\"path\":\"a.c\"}") == 0);
   assert(turn(&t, "edit_file", "{\"path\":\"a.c\"}") == 1);
   /* Later turns must not move a settled number. */
   assert(turn(&t, "read_file", "{\"path\":\"a.c\"}") == 0);
   assert(t.rounds_to_resume == 1);
   PASS("rounds_to_resume settles once and stays settled");
}

static void test_dropped_sigs_are_carried(void)
{
   session_compact_result_t r;
   memset(&r, 0, sizeof(r));
   r.compacted = 1;
   r.readonly_sigs_dropped = 7; /* basis was incomplete at capture time */

   rtr_tracker_t t;
   rtr_begin(&t, &r);
   assert(t.sigs_dropped == 7); /* a report must be able to say so */
   PASS("an incomplete capture basis is carried, not hidden");
}

int main(void)
{
   printf("rounds_to_resume:\n");

   test_no_boundary_is_not_tracked();
   test_immediate_progress_costs_zero_rounds();
   test_rederivation_rounds_are_counted();
   test_repeated_test_is_progress_not_rederivation();
   test_new_lookup_is_progress();
   test_text_only_turn_is_not_a_round();
   test_mixed_turn_counts_as_progress();
   test_resolves_once();
   test_dropped_sigs_are_carried();

   printf("all rounds_to_resume tests passed\n");
   return 0;
}
