/* test_memory_assemble_util.c: unit tests for the pure string helpers
 * extracted from memory_assemble.c (xml_escape_text, context_xml_tag_for_header).
 * Header is static inline → this test links nothing extra. */
#include "modules/memory/memory_assemble_util.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_xml_escape(void)
{
   char out[256];

   /* each special char → entity */
   xml_escape_text("a&b<c>d\"e", out, sizeof(out));
   assert(strcmp(out, "a&amp;b&lt;c&gt;d&quot;e") == 0);

   /* no specials → passthrough */
   xml_escape_text("plain text 123", out, sizeof(out));
   assert(strcmp(out, "plain text 123") == 0);

   /* leading/standalone specials */
   xml_escape_text("<>&\"", out, sizeof(out));
   assert(strcmp(out, "&lt;&gt;&amp;&quot;") == 0);

   /* NULL / empty src → empty dst */
   xml_escape_text(NULL, out, sizeof(out));
   assert(out[0] == '\0');
   xml_escape_text("", out, sizeof(out));
   assert(out[0] == '\0');

   /* zero-length / NULL dst tolerated (no crash) */
   xml_escape_text("x", out, 0);
   xml_escape_text("x", NULL, sizeof(out));

   /* plain truncation: 4-char buffer holds 3 chars + NUL */
   char small[4];
   xml_escape_text("abcdef", small, sizeof(small));
   assert(strcmp(small, "abc") == 0);

   /* entity that would not fit is dropped whole (no partial "&am") */
   char tiny[4];
   xml_escape_text("&", tiny, sizeof(tiny)); /* "&amp;" is 5 > 3 free */
   assert(tiny[0] == '\0');

   /* entity exactly fits: 6-byte buffer fits "&amp;" (5) + NUL */
   char fit[6];
   xml_escape_text("&", fit, sizeof(fit));
   assert(strcmp(fit, "&amp;") == 0);

   printf("  xml_escape_text: ok\n");
}

static void test_xml_tag_for_header(void)
{
   assert(strcmp(context_xml_tag_for_header("Key Facts"), "historical_fact") == 0);
   assert(strcmp(context_xml_tag_for_header("Mental Models"), "mental_model") == 0);
   assert(strcmp(context_xml_tag_for_header("Constraints"), "constraint") == 0);
   assert(strcmp(context_xml_tag_for_header("Procedures"), "procedure_memory") == 0);
   assert(strcmp(context_xml_tag_for_header("Active Tasks"), "active_task") == 0);
   assert(strcmp(context_xml_tag_for_header("Recent Context"), "recent_event") == 0);
   /* unknown + NULL → generic fallback */
   assert(strcmp(context_xml_tag_for_header("Nope"), "memory_item") == 0);
   assert(strcmp(context_xml_tag_for_header(""), "memory_item") == 0);
   assert(strcmp(context_xml_tag_for_header(NULL), "memory_item") == 0);
   printf("  context_xml_tag_for_header: ok\n");
}

/* Near-duplicate suppression on the read path.
 *
 * The asymmetric risk drives every case here: wrongly suppressing a DISTINCT
 * fact silently loses evidence, while admitting one redundant line merely wastes
 * budget. So the false-positive cases matter more than the true-positive one,
 * and there are more of them. */
static void test_near_duplicate(void)
{
   /* Identical text is the floor case. */
   assert(assemble_texts_near_duplicate("deploy with the staging matrix first",
                                        "deploy with the staging matrix first") == 1);

   /* A genuine restatement: same content, trivially reworded. This is what the
    * suppression exists for -- the same fact stored twice. */
   assert(assemble_texts_near_duplicate("the deploy uses the staging matrix before production",
                                        "the deploy uses the staging matrix before production!") ==
          1);

   /* Distinct facts that SHARE vocabulary must survive. Both mention deploy,
    * staging and production; they say different things. Suppressing either would
    * lose evidence, which is the failure mode worth guarding hardest. */
   assert(assemble_texts_near_duplicate("deploy to staging before production",
                                        "never deploy to production on a Friday") == 0);

   /* Unrelated text. */
   assert(assemble_texts_near_duplicate("the embedder runs inside the kb container",
                                        "rate limits reset at midnight UTC") == 0);

   /* Degenerate inputs must not claim similarity. Empty and NULL carry no
    * tokens, and "no information" is not "the same information". */
   assert(assemble_texts_near_duplicate("", "") == 0);
   assert(assemble_texts_near_duplicate(NULL, "anything at all here") == 0);
   assert(assemble_texts_near_duplicate("anything at all here", NULL) == 0);

   /* Short tokens alone carry no signal: two texts made only of stopword-length
    * words must not collide just because they are both mostly noise. */
   assert(assemble_texts_near_duplicate("a an is to be", "of by in on at") == 0);

   /* Symmetry -- the order candidates arrive in must not change the verdict. */
   const char *x = "vault rotation requires the operator latch";
   const char *y = "the operator latch is required for vault rotation";
   assert(assemble_texts_near_duplicate(x, y) == assemble_texts_near_duplicate(y, x));

   printf("near_duplicate OK\n");
}

static void test_recency_curve(void)
{
   /* Inside the plateau nothing decays: a fact three months old must not be
    * outranked on age alone, which is the whole point of the flat zone. */
   assert(context_recency_from_age_days(0.0) == 1.0);
   assert(context_recency_from_age_days(1.0) == 1.0);
   assert(context_recency_from_age_days(MEMORY_RECENCY_PLATEAU_DAYS) == 1.0);

   /* An unknown age is not evidence of staleness. */
   assert(context_recency_from_age_days(-1.0) == 1.0);

   /* Exactly one half-life past the plateau halves the factor. */
   double half =
       context_recency_from_age_days(MEMORY_RECENCY_PLATEAU_DAYS + MEMORY_RECENCY_HALFLIFE_DAYS);
   assert(fabs(half - 0.5) < 1e-9);

   /* Monotonically decreasing past the plateau, and never zero -- an old fact
    * sinks but stays reachable. */
   double prev = 1.0;
   for (double d = MEMORY_RECENCY_PLATEAU_DAYS + 1.0; d < 8000.0; d += 137.0)
   {
      double r = context_recency_from_age_days(d);
      assert(r < prev);
      assert(r > 0.0);
      prev = r;
   }

   /* The tail is deliberately gentle: a year past the plateau must still be
    * worth more than half its original score, or correct old facts get buried
    * under fresher but less relevant ones. */
   assert(context_recency_from_age_days(MEMORY_RECENCY_PLATEAU_DAYS + 365.0) > 0.8);

   printf("recency_curve OK\n");
}

static void test_apply_recency(void)
{
   /* recency_weight 0 means the intent does not care about age; the score must
    * come through untouched rather than picking up a near-1.0 multiplier. */
   assert(context_apply_recency(0.75, 0.25, 0.0) == 0.75);
   assert(context_apply_recency(0.75, 0.25, -1.0) == 0.75);

   /* Full weight applies the whole decay. */
   assert(fabs(context_apply_recency(0.8, 0.5, 1.0) - 0.4) < 1e-9);

   /* Out-of-range weights clamp rather than amplifying the decay. */
   assert(fabs(context_apply_recency(0.8, 0.5, 4.0) - 0.4) < 1e-9);

   /* Partial weight interpolates: half weight on a 0.5 factor gives 0.75x. */
   assert(fabs(context_apply_recency(1.0, 0.5, 0.5) - 0.75) < 1e-9);

   /* A fresh candidate is never penalised at any weight. */
   for (double w = 0.0; w <= 1.0; w += 0.1)
      assert(fabs(context_apply_recency(0.6, 1.0, w) - 0.6) < 1e-9);

   /* Ordering: given equal base scores, the fresher candidate wins whenever
    * recency actually carries weight. */
   double fresh = context_apply_recency(0.5, context_recency_from_age_days(10.0), 0.7);
   double stale = context_apply_recency(0.5, context_recency_from_age_days(4000.0), 0.7);
   assert(fresh > stale);

   printf("apply_recency OK\n");
}

int main(void)
{
   test_xml_escape();
   test_xml_tag_for_header();
   test_near_duplicate();
   test_recency_curve();
   test_apply_recency();
   printf("memory_assemble_util: all tests passed\n");
   return 0;
}
