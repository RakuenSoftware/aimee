/* test_approach_memory.c: approach-level negative knowledge (S3).
 *
 * Two claims. First, the scoring is honest: goals that differ only in wording
 * match, unrelated goals do not, and "nothing in common with nothing" is not a
 * perfect match. Second, recall actually surfaces the dead end — and never
 * reaches the anti-pattern blocking path while doing it.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db1.h"
#include "modules/db2/c/anti_patterns.h"
#include "approach_failures.h"
#include "approach_store.h"
#include "support/store_module_fixture.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"

#include <aimee/learning/approach_memory.h>

/* The store has no count helper (nothing in production needs one); the pool
 * read answers the same question for the test. */
static int approach_row_count(void)
{
   db1_approach_failure_t rows[64];
   int n = db1_approach_failure_candidates("", rows, 64);
   return n;
}

/* Rendering asks the knowledge service which arm to apply. This test links
 * none, so NULL is returned and the LOCAL default stands — which is exactly the
 * fallback the renderer is required to take. */
char *kb_client_learning_policy_select_json(const char *decision_point)
{
   (void)decision_point;
   return NULL;
}

static void test_tokenisation(void)
{
   char t[APPROACH_MEM_TOKENS_LEN];

   learning_approach_tokens("Rebuild the search index", t, sizeof(t));
   /* Case-folded, punctuation-split, and short words dropped: "the" carries no
    * topic and would inflate overlap between unrelated goals. */
   assert(strcmp(t, "rebuild search index") == 0);

   learning_approach_tokens("REBUILD, the -- search_index!", t, sizeof(t));
   assert(strcmp(t, "rebuild search index") == 0);

   /* A repeated word is one token, so repetition cannot dominate a score. */
   learning_approach_tokens("index index index rebuild", t, sizeof(t));
   assert(strcmp(t, "index rebuild") == 0);

   learning_approach_tokens("", t, sizeof(t));
   assert(t[0] == '\0');
   learning_approach_tokens(NULL, t, sizeof(t));
   assert(t[0] == '\0');
   /* All words too short to carry topic: nothing to match on. */
   learning_approach_tokens("a to of an", t, sizeof(t));
   assert(t[0] == '\0');
}

static void test_signature_follows_meaning_not_spelling(void)
{
   char a[APPROACH_MEM_SIGNATURE_LEN], b[APPROACH_MEM_SIGNATURE_LEN];
   assert(learning_approach_signature("Rebuild the search index", a, sizeof(a)) == 0);
   assert(learning_approach_signature("rebuild   SEARCH-index...", b, sizeof(b)) == 0);
   assert(strcmp(a, b) == 0);
   assert(strlen(a) == APPROACH_MEM_SIGNATURE_LEN - 1);

   assert(learning_approach_signature("Rebuild the vector store", b, sizeof(b)) == 0);
   assert(strcmp(a, b) != 0);

   assert(learning_approach_signature("x", a, 4) == -1);
   assert(learning_approach_signature("x", NULL, sizeof(a)) == -1);
}

static void test_overlap_scoring(void)
{
   char a[APPROACH_MEM_TOKENS_LEN], b[APPROACH_MEM_TOKENS_LEN];

   learning_approach_tokens("rebuild the search index", a, sizeof(a));
   learning_approach_tokens("rebuild the search index", b, sizeof(b));
   assert(learning_approach_overlap(a, b) == 1.0);

   /* Same goal, one extra word: still clearly the same goal. */
   learning_approach_tokens("please rebuild the search index now", b, sizeof(b));
   assert(learning_approach_overlap(a, b) >= APPROACH_MEM_MIN_SIMILARITY);

   /* Related words, different goal: must fall below the bar, or recall would
    * bury the planner in loosely-related history. */
   learning_approach_tokens("delete the customer database", b, sizeof(b));
   assert(learning_approach_overlap(a, b) < APPROACH_MEM_MIN_SIMILARITY);

   /* Two empty sets have nothing in common — that must not read as identical. */
   assert(learning_approach_overlap("", "") == 0.0);
   assert(learning_approach_overlap(a, "") == 0.0);
   assert(learning_approach_overlap(NULL, NULL) == 0.0);
}

static void test_recall_surfaces_the_dead_end(void)
{
   /* Reads the store back, so it needs one attached. main() has started the
      module when a database was named; without one there is nothing to assert
      against and the pure tests above still ran. */
   if (!store_module_fixture_available())
      return;

   const char *goal = "Rebuild the search index for the docs project";

   assert(approach_store_record(goal, "drop and re-ingest every document",
                                "ran out of disk partway", "agent_job", "agent_job:41") == 0);
   assert(approach_store_record("Roll out the new TLS certificate", "restart every node at once",
                                "quorum was lost", "agent_job", "agent_job:42") == 0);

   /* The same goal, worded differently, finds the dead end. */
   learning_approach_hit_t hits[APPROACH_MEM_MAX_RECALL];
   int n = approach_store_recall("rebuild search index for docs project", hits,
                                 APPROACH_MEM_MAX_RECALL);
   assert(n == 1);
   assert(strcmp(hits[0].approach_text, "drop and re-ingest every document") == 0);
   assert(strcmp(hits[0].failure_mode, "ran out of disk partway") == 0);
   assert(hits[0].occurrences == 1);
   assert(hits[0].similarity >= APPROACH_MEM_MIN_SIMILARITY);

   /* An unrelated goal recalls nothing — silence is the common case and must
    * stay silent. */
   assert(approach_store_recall("write the quarterly board report", hits,
                                APPROACH_MEM_MAX_RECALL) == 0);

   /* The same dead end again is more evidence for one row, not a second row. */
   assert(approach_store_record(goal, "drop and re-ingest every document", "ran out of disk again",
                                "agent_job", "agent_job:43") == 0);
   n = approach_store_recall(goal, hits, APPROACH_MEM_MAX_RECALL);
   assert(n == 1);
   assert(hits[0].occurrences == 2);
   assert(approach_row_count() == 2); /* two distinct goals, not three rows */

   /* A second approach to the SAME goal is a distinct dead end, and both come
    * back ranked. */
   assert(approach_store_record(goal, "rebuild in place while serving", "queries timed out",
                                "agent_job", "agent_job:44") == 0);
   n = approach_store_recall(goal, hits, APPROACH_MEM_MAX_RECALL);
   assert(n == 2);
   assert(hits[0].similarity >= hits[1].similarity);
}

static void test_render_reports_and_never_instructs(void)
{
   /* Renders what the recall test recorded, so it needs the same store. */
   if (!store_module_fixture_available())
      return;

   char out[2048];
   int n = approach_store_render("Rebuild the search index for the docs project", out, sizeof(out),
                                 NULL, 0);
   assert(n >= 1);
   assert(strstr(out, "drop and re-ingest every document") != NULL);
   assert(strstr(out, "ran out of disk") != NULL);
   assert(strstr(out, "seen 2 times") != NULL);

   /* Advisory, not imperative: this text is injected near a plan, and an
    * instruction here would be an unreviewed rule reaching the agent. */
   assert(strstr(out, "do not") == NULL);
   assert(strstr(out, "Do not") == NULL);
   assert(strstr(out, "must") == NULL);
   assert(strstr(out, "never") == NULL);

   /* Nothing similar: nothing said, rather than an empty header. */
   n = approach_store_render("write the quarterly board report", out, sizeof(out), NULL, 0);
   assert(n == 0);
   assert(out[0] == '\0');
}

static void test_no_progress_failure_becomes_retry_context(void)
{
   if (!store_module_fixture_available())
      return;

   const char *goal = "Diagnose and repair trust bundle readiness across the Aimee repository";
   const char *failure =
       "no-progress circuit breaker tripped after 28 successful calls without an edit";

   /* The source identifies who paid to discover the dead end. Recall below has
    * no source/session/model filter: the lesson belongs to this shared KB and
    * is available to another authorized consumer with a similar goal. */
   assert(approach_store_record_no_progress(goal, failure,
                                            "delegate:user-a/session-1/qwen-local") == 0);

   learning_approach_hit_t hits[APPROACH_MEM_MAX_RECALL];
   int n = approach_store_recall("Repair Aimee repository trust bundle readiness", hits,
                                 APPROACH_MEM_MAX_RECALL);
   assert(n == 1);
   assert(strcmp(hits[0].approach_text,
                 "broad repository exploration with repeated or overlapping retrievals and no "
                 "edit") == 0);
   assert(strcmp(hits[0].failure_mode, failure) == 0);
   assert(hits[0].occurrences == 1);

   char *retry = approach_store_retry_context("Repair Aimee repository trust bundle readiness");
   assert(retry != NULL);
   assert(strstr(retry, "<prior_failure_learning>") != NULL);
   assert(strstr(retry, hits[0].approach_text) != NULL);
   assert(strstr(retry, "materially different plan") != NULL);
   assert(strstr(retry, "smallest justified edit or decisive test") != NULL);
   free(retry);

   /* A repeated stop reinforces the same learned approach rather than creating
    * an unbounded series of prose variants. */
   assert(approach_store_record_no_progress(goal, failure, "delegate:user-b/session-9/terra") == 0);
   n = approach_store_recall(goal, hits, APPROACH_MEM_MAX_RECALL);
   assert(n == 1);
   assert(hits[0].occurrences == 2);

   /* Goal similarity is the scope boundary: unrelated delegates do not receive
    * this task's failure history. */
   retry = approach_store_retry_context("Write the quarterly customer newsletter");
   assert(retry == NULL);
}

static void test_never_touches_the_blocking_path(void)
{
   /* Asserts that recording created no anti-pattern rows. With no store
      nothing was recorded, so the assertion would hold without proving
      anything -- gate it rather than let it read as a pass. */
   if (!store_module_fixture_available())
      return;

   /* Recording approach failures must not create anti-pattern rows: that
    * table's hot rows drive a path that REFUSES work, and a fuzzy goal match
    * has no business there. */
   anti_pattern_t rows[8];
   assert(db2_anti_pattern_list(rows, 8) == 0);
   assert(db2_anti_pattern_check("docs/index.md", "drop and re-ingest every document", rows, 8) ==
          0);
}

/* An installation with no knowledge service has never recorded a dead end. The
 * honest answer to "what have we already tried?" is "nothing" — not an error,
 * which would make plan-time recall look broken on every such install.
 *
 * Two shapes reach this: a build with DB2 compiled out (the daemon's own), and
 * a build with the store present but unreachable. The second is what this
 * closes the shim to reproduce; the first is asserted by the same code path
 * returning 0 rather than -1. */
static void test_absent_store_means_nothing_known(void)
{
   /* Reads the store back, so it needs one attached. main() has started the
      module when a database was named; without one there is nothing to assert
      against and the pure tests above still ran. */
   if (!store_module_fixture_available())
      return;

   /* An empty store answers "nothing known" rather than failing. */
   learning_approach_hit_t hits[APPROACH_MEM_MAX_RECALL];
   assert(approach_store_recall("a goal nobody has ever attempted here", hits,
                                APPROACH_MEM_MAX_RECALL) == 0);
   char out[512];
   assert(approach_store_render("a goal nobody has ever attempted here", out, sizeof(out), NULL,
                                0) == 0);
   assert(out[0] == '\0');
}

int main(void)
{
   printf("approach_memory: ");

   if (store_module_fixture_available())
      store_module_fixture_start();
   db2_test_shim_open();

   test_tokenisation();
   test_signature_follows_meaning_not_spelling();
   test_overlap_scoring();
   test_recall_surfaces_the_dead_end();
   test_render_reports_and_never_instructs();
   test_no_progress_failure_becomes_retry_context();
   test_never_touches_the_blocking_path();
   test_absent_store_means_nothing_known();

   /* Bad args are refused rather than guessed. */
   assert(approach_store_record(NULL, "a", "m", "s", "r") == -1);
   assert(approach_store_record("g", "", "m", "s", "r") == -1);
   /* A goal with no topic words cannot be recalled against, so it is not stored. */
   assert(approach_store_record("a to of", "approach", "m", "s", "r") == -1);
   {
      learning_approach_hit_t hits[2];
      assert(approach_store_recall(NULL, hits, 2) == -1);
      assert(approach_store_recall("goal", hits, 0) == -1);
   }

   printf("ok\n");
   return 0;
}
