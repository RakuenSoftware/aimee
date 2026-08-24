/* test_curiosity_resolve.c: draining the curiosity backlog on demand (S4).
 *
 * The interesting assertions are all about restraint: what the pass refuses to
 * close. A backlog drainer that is too eager does not clear a backlog, it
 * destroys the record of what is unknown.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "curiosity_resolve.h"
#include "db1.h"
#include "modules/db2/c/curiosity.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"

static int g_probe_calls;
static curiosity_evidence_t g_probe_answer;
static char g_last_subject[128];

static curiosity_evidence_t fake_probe(const char *gap_type, const char *subject,
                                       const char *evidence)
{
   (void)gap_type;
   (void)evidence;
   g_probe_calls++;
   snprintf(g_last_subject, sizeof(g_last_subject), "%s", subject ? subject : "");
   return g_probe_answer;
}

/* Answer only for one named subject, so a pass can be seen to close exactly
 * the item whose gap was filled and leave its neighbours alone. */
static curiosity_evidence_t selective_probe(const char *gap_type, const char *subject,
                                            const char *evidence)
{
   (void)gap_type;
   (void)evidence;
   g_probe_calls++;
   return (subject && strcmp(subject, "answered-topic") == 0) ? CURIOSITY_EVIDENCE_FOUND
                                                              : CURIOSITY_EVIDENCE_NONE;
}

static void seed(const char *gap_type, const char *topic)
{
   assert(db2_curiosity_create(gap_type, "", topic, "seeded by test", 0.5, 0.5, "s-test", NULL) ==
          0);
}

static int open_count(void)
{
   curiosity_item_t items[64];
   int n = db2_curiosity_list(CURIOSITY_STATE_OPEN, items, 64);
   return n < 0 ? -1 : n;
}

static void test_no_probe_closes_nothing(void)
{
   assert(db2_curiosity_reset() == 0);
   seed(CURIOSITY_GAP_WEAK_COVERAGE, "some-topic");
   curiosity_resolve_register_probe(NULL);

   curiosity_resolve_stats_t st;
   assert(curiosity_resolve_pass(0, &st) == 0);
   /* The important part: it says WHY rather than reporting a clean pass. An
    * absent probe means no way to tell, and closing on that basis would empty
    * the backlog by assertion. */
   assert(st.no_probe == 1);
   assert(st.resolved == 0);
   assert(st.considered == 0);
   assert(open_count() == 1);
}

static void test_only_coverage_shaped_gaps_are_touched(void)
{
   assert(db2_curiosity_reset() == 0);
   seed(CURIOSITY_GAP_UNVERIFIED_ASSUMPTION, "assumption-topic");
   seed(CURIOSITY_GAP_WEAK_COVERAGE, "coverage-topic");
   seed(CURIOSITY_GAP_CONTRADICTION, "contradiction-topic");
   seed(CURIOSITY_GAP_STALE_FACT, "stale-topic");

   g_probe_calls = 0;
   g_probe_answer = CURIOSITY_EVIDENCE_FOUND;
   curiosity_resolve_register_probe(fake_probe);

   curiosity_resolve_stats_t st;
   int resolved = curiosity_resolve_pass(0, &st);

   /* A contradiction asks which of two claims is right, and a stale fact asks
    * whether something changed. A coverage probe answers neither, so closing
    * them would silently pick a winner. They are skipped, not guessed. */
   assert(resolved == 2);
   assert(st.resolved == 2);
   assert(st.skipped == 2);
   assert(g_probe_calls == 2);
   assert(open_count() == 2);

   curiosity_item_t items[16];
   int n = db2_curiosity_list(CURIOSITY_STATE_OPEN, items, 16);
   for (int i = 0; i < n; i++)
      assert(strcmp(items[i].gap_type, CURIOSITY_GAP_CONTRADICTION) == 0 ||
             strcmp(items[i].gap_type, CURIOSITY_GAP_STALE_FACT) == 0);
}

static void test_a_gap_that_still_stands_stays_open(void)
{
   assert(db2_curiosity_reset() == 0);
   seed(CURIOSITY_GAP_WEAK_COVERAGE, "answered-topic");
   seed(CURIOSITY_GAP_WEAK_COVERAGE, "unanswered-topic");

   g_probe_calls = 0;
   curiosity_resolve_register_probe(selective_probe);

   curiosity_resolve_stats_t st;
   assert(curiosity_resolve_pass(0, &st) == 1);
   assert(st.resolved == 1);
   assert(st.still_open == 1);
   assert(open_count() == 1);

   curiosity_item_t items[8];
   assert(db2_curiosity_list(CURIOSITY_STATE_OPEN, items, 8) == 1);
   assert(strcmp(items[0].target_topic, "unanswered-topic") == 0);
}

static void test_undecided_is_not_resolved(void)
{
   assert(db2_curiosity_reset() == 0);
   seed(CURIOSITY_GAP_WEAK_COVERAGE, "murky-topic");

   g_probe_answer = CURIOSITY_EVIDENCE_UNKNOWN;
   curiosity_resolve_register_probe(fake_probe);

   curiosity_resolve_stats_t st;
   assert(curiosity_resolve_pass(0, &st) == 0);
   /* "I could not tell" is counted apart from "the gap stands", because the
    * two mean different things to an operator deciding whether to look. */
   assert(st.unknown == 1);
   assert(st.still_open == 0);
   assert(open_count() == 1);
}

static void test_the_budget_bounds_the_pass(void)
{
   assert(db2_curiosity_reset() == 0);
   for (int i = 0; i < 10; i++)
   {
      char topic[32];
      snprintf(topic, sizeof(topic), "topic-%d", i);
      seed(CURIOSITY_GAP_WEAK_COVERAGE, topic);
   }

   g_probe_calls = 0;
   g_probe_answer = CURIOSITY_EVIDENCE_FOUND;
   curiosity_resolve_register_probe(fake_probe);

   curiosity_resolve_stats_t st;
   assert(curiosity_resolve_pass(3, &st) == 3);
   assert(st.considered == 3);
   assert(st.budget == 3);
   assert(g_probe_calls == 3); /* the probe may reach a service: it is not called past the budget */
   assert(open_count() == 7);

   /* Running it again picks up where it left off, so a bounded pass is a
    * pause rather than a cap on what can ever be closed. */
   assert(curiosity_resolve_pass(3, &st) == 3);
   assert(open_count() == 4);
}

static void test_an_item_with_nothing_to_look_up(void)
{
   assert(db2_curiosity_reset() == 0);
   /* No entity and no topic: there is no question to ask, so it must not be
    * closed for the absence of one. */
   assert(db2_curiosity_create(CURIOSITY_GAP_WEAK_COVERAGE, "", "", "no subject", 0.5, 0.5,
                               "s-test", NULL) == 0);
   g_probe_calls = 0;
   g_probe_answer = CURIOSITY_EVIDENCE_FOUND;
   curiosity_resolve_register_probe(fake_probe);

   curiosity_resolve_stats_t st;
   assert(curiosity_resolve_pass(0, &st) == 0);
   assert(st.unknown == 1);
   assert(g_probe_calls == 0);
   assert(open_count() == 1);
}

int main(void)
{
   printf("curiosity_resolve: ");

   assert(db1_init(":memory:") == 0);
   db2_test_shim_open();

   test_no_probe_closes_nothing();
   test_only_coverage_shaped_gaps_are_touched();
   test_a_gap_that_still_stands_stays_open();
   test_undecided_is_not_resolved();
   test_the_budget_bounds_the_pass();
   test_an_item_with_nothing_to_look_up();

   /* An empty backlog is a no-op, not an error. */
   assert(db2_curiosity_reset() == 0);
   {
      curiosity_resolve_stats_t st;
      assert(curiosity_resolve_pass(0, &st) == 0);
      assert(st.considered == 0);
      assert(curiosity_resolve_pass(0, NULL) == 0);
   }

   curiosity_resolve_register_probe(NULL);
   printf("ok\n");
   return 0;
}
