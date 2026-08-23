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
#include <string.h>

#include "db.h"
#include "db1.h"
#include "modules/db2/c/anti_patterns.h"
#include "modules/db2/c/approach_failures.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_test_shim.h"

#include <aimee/learning/approach_memory.h>

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
   const char *goal = "Rebuild the search index for the docs project";

   assert(learning_approach_record_failure(goal, "drop and re-ingest every document",
                                           "ran out of disk partway", "agent_job",
                                           "agent_job:41") == 0);
   assert(learning_approach_record_failure("Roll out the new TLS certificate",
                                           "restart every node at once", "quorum was lost",
                                           "agent_job", "agent_job:42") == 0);

   /* The same goal, worded differently, finds the dead end. */
   learning_approach_hit_t hits[APPROACH_MEM_MAX_RECALL];
   int n = learning_approach_recall("rebuild search index for docs project", hits,
                                    APPROACH_MEM_MAX_RECALL);
   assert(n == 1);
   assert(strcmp(hits[0].approach_text, "drop and re-ingest every document") == 0);
   assert(strcmp(hits[0].failure_mode, "ran out of disk partway") == 0);
   assert(hits[0].occurrences == 1);
   assert(hits[0].similarity >= APPROACH_MEM_MIN_SIMILARITY);

   /* An unrelated goal recalls nothing — silence is the common case and must
    * stay silent. */
   assert(learning_approach_recall("write the quarterly board report", hits,
                                   APPROACH_MEM_MAX_RECALL) == 0);

   /* The same dead end again is more evidence for one row, not a second row. */
   assert(learning_approach_record_failure(goal, "drop and re-ingest every document",
                                           "ran out of disk again", "agent_job",
                                           "agent_job:43") == 0);
   n = learning_approach_recall(goal, hits, APPROACH_MEM_MAX_RECALL);
   assert(n == 1);
   assert(hits[0].occurrences == 2);
   assert(db2_approach_failure_count() == 2); /* two distinct goals, not three rows */

   /* A second approach to the SAME goal is a distinct dead end, and both come
    * back ranked. */
   assert(learning_approach_record_failure(goal, "rebuild in place while serving",
                                           "queries timed out", "agent_job", "agent_job:44") == 0);
   n = learning_approach_recall(goal, hits, APPROACH_MEM_MAX_RECALL);
   assert(n == 2);
   assert(hits[0].similarity >= hits[1].similarity);
}

static void test_render_reports_and_never_instructs(void)
{
   char out[2048];
   int n = learning_approach_render("Rebuild the search index for the docs project", out,
                                    sizeof(out), NULL, 0);
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
   n = learning_approach_render("write the quarterly board report", out, sizeof(out), NULL, 0);
   assert(n == 0);
   assert(out[0] == '\0');
}

static void test_never_touches_the_blocking_path(void)
{
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
 * which would make plan-time recall look broken on every such install. */
static void test_absent_store_means_nothing_known(void)
{
   db2_test_shim_close();
   learning_approach_hit_t hits[APPROACH_MEM_MAX_RECALL];
   assert(learning_approach_recall("rebuild the search index", hits, APPROACH_MEM_MAX_RECALL) == 0);
   char out[512];
   assert(learning_approach_render("rebuild the search index", out, sizeof(out), NULL, 0) == 0);
   assert(out[0] == '\0');
   db2_test_shim_open();
}

int main(void)
{
   printf("approach_memory: ");

   assert(db1_init(":memory:") == 0);
   db2_test_shim_open();

   test_tokenisation();
   test_signature_follows_meaning_not_spelling();
   test_overlap_scoring();
   test_recall_surfaces_the_dead_end();
   test_render_reports_and_never_instructs();
   test_never_touches_the_blocking_path();
   test_absent_store_means_nothing_known();

   /* Bad args are refused rather than guessed. */
   assert(learning_approach_record_failure(NULL, "a", "m", "s", "r") == -1);
   assert(learning_approach_record_failure("g", "", "m", "s", "r") == -1);
   /* A goal with no topic words cannot be recalled against, so it is not stored. */
   assert(learning_approach_record_failure("a to of", "approach", "m", "s", "r") == -1);
   {
      learning_approach_hit_t hits[2];
      assert(learning_approach_recall(NULL, hits, 2) == -1);
      assert(learning_approach_recall("goal", hits, 0) == -1);
   }

   printf("ok\n");
   return 0;
}
