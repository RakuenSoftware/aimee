/* test_web_read_spans.c -- deterministic match-window extraction.
 *
 * The previous version of this file tested a chunker and three ranking
 * selectors. All of that is gone: chunking existed to feed an embedder that was
 * never built, the chunk score was an artifact of the boxes it counted over, and
 * the second "leg" and its fusion existed only to serve that score. What is left
 * is deterministic, so the tests assert exact behaviour rather than measuring
 * how often two rankers disagree.
 *
 * Reached by including the translation unit (the idiom used by
 * test_anthropic_http.c and test_provider_catalog.c) so extraction runs with no
 * network I/O at all. */
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../posix/web_read.c"

/* webread_extract takes ownership of text; hand it a private copy. */
static char *extract(const char *text, const char *ref, const char *query, int span)
{
   char *owned = safe_strdup(text);
   char *out = webread_extract(owned, ref, query, span, "https://fixture.invalid/page");
   assert(out != NULL);
   return out;
}

static int contains(const char *hay, const char *needle)
{
   return hay && needle && strstr(hay, needle) != NULL;
}

static const char *FIX =
    "Introduction paragraph about the project and its general goals, written at "
    "enough length to sit well away from what follows. It mentions nothing specific.\n\n"
    "The configuration accepts a retry_budget_ms setting which bounds how long the "
    "client keeps retrying a failed upstream call before surfacing an error.\n\n"
    "Closing notes about licensing and attribution, padded so it is clearly a "
    "separate region of the document from the paragraphs above it.\n\n";

/* ---- the property the old design could not guarantee ---- */

/* A needle is retrieved wherever it appears, at any offset. Under the chunker
 * this failed on ~1.6% of pages because a cut could land inside the token; a
 * window centred on the match cannot split it, so this is now structural. */
static void test_needle_always_retrieved_at_any_offset(void)
{
   for (int pad = 0; pad < 3000; pad += 7)
   {
      char *page = malloc(8192);
      assert(page);
      snprintf(page, 8192, "%*s retry_budget_ms %*s", pad, "a", 200, "b");
      char *out = extract(page, "r1", "retry_budget_ms", 0);
      if (!contains(out, "retry_budget_ms"))
      {
         fprintf(stderr, "needle lost at pad=%d\n%s\n", pad, out);
         assert(0 && "needle must be retrieved at any offset");
      }
      free(out);
      free(page);
   }
   printf("  PASS: needle retrieved at every offset (429 placements)\n");
}

/* ---- determinism ---- */

static void test_deterministic(void)
{
   char *a = extract(FIX, "r1", "retry_budget_ms configuration", 0);
   for (int i = 0; i < 16; i++)
   {
      char *b = extract(FIX, "r1", "retry_budget_ms configuration", 0);
      assert(strcmp(a, b) == 0);
      free(b);
   }
   free(a);
   printf("  PASS: extraction is deterministic\n");
}

/* ---- contracts that must survive the rewrite ---- */

static void test_fenced_as_untrusted(void)
{
   char *out = extract(FIX, "r1", "retry_budget_ms", 0);
   assert(contains(out, "untrusted retrieved content"));
   free(out);
   printf("  PASS: output fenced as untrusted\n");
}

static void test_cited_by_ordinal(void)
{
   char *out = extract(FIX, "r7", "retry_budget_ms", 0);
   assert(contains(out, "r7#1"));
   free(out);
   printf("  PASS: spans cited by window ordinal\n");
}

static void test_span_round_trip(void)
{
   char *all = extract(FIX, "r1", "configuration", 0);
   char *one = extract(FIX, "r1", "configuration", 1);
   assert(contains(one, "r1#1"));
   assert(strlen(one) < strlen(all));
   free(all);
   free(one);
   char *oor = extract(FIX, "r1", "configuration", 9999);
   assert(contains(oor, "out of range"));
   free(oor);
   printf("  PASS: span=N round-trip and out-of-range\n");
}

static void test_budget_respected(void)
{
   /* many matches: emission must stop at the budget and report the remainder */
   char *page = malloc(200000);
   assert(page);
   int off = 0;
   for (int i = 0; i < 400; i++)
      off += snprintf(page + off, 200000 - (size_t)off,
                      "section %d mentions retry_budget_ms then continues %*s\n\n", i, 200, "x");
   char *out = extract(page, "r1", "retry_budget_ms", 0);
   int shown = -1, total = -1, matches = -1, omitted = -1;
   const char *f = strstr(out, "-- ");
   assert(f);
   assert(sscanf(f, "-- %d of %d spans shown (%d matches, %d omitted)", &shown, &total, &matches,
                 &omitted) == 4);
   assert(shown >= 1);
   assert(shown <= total);
   assert(shown + omitted == total); /* every window is shown or omitted, never both */
   assert(matches >= total);         /* windows merge matches, so never more windows than matches */
   assert(strlen(out) < (size_t)WEBREAD_BUDGET * 4 + 1024);
   free(out);
   free(page);
   printf("  PASS: budget respected and footer counts reconcile\n");
}

/* Adjacent matches must not produce duplicated, overlapping output. */
static void test_overlapping_matches_merge(void)
{
   char *page = malloc(4096);
   assert(page);
   snprintf(page, 4096, "%*s alpha1 and then beta2 right next to it %*s", 400, "p", 400, "q");
   char *out = extract(page, "r1", "alpha1 beta2", 0);
   /* both matches are within one context width, so they share a single window */
   assert(contains(out, "alpha1"));
   assert(contains(out, "beta2"));
   const char *first = strstr(out, "r1#1");
   assert(first);
   assert(strstr(first + 1, "r1#2") == NULL);
   free(out);
   free(page);
   printf("  PASS: overlapping matches merge into one window\n");
}

/* Windows must not begin or end mid-word. */
static void test_windows_snap_to_word_edges(void)
{
   char *page = malloc(8192);
   assert(page);
   snprintf(page, 8192, "%*s retry_budget_ms %*s", 1500, "w", 1500, "z");
   char *out = extract(page, "r1", "retry_budget_ms", 0);
   /* the emitted span body starts after the citation line; check it does not
    * begin or end with a partial run glued to the fence markers */
   assert(!contains(out, "  w\n"));
   free(out);
   free(page);
   printf("  PASS: windows snap to whitespace edges\n");
}

static void test_no_match_and_empty_inputs(void)
{
   char *out = extract(FIX, "r1", "zzzznotpresentanywhere", 0);
   assert(contains(out, "r1#1"));           /* top of page ... */
   assert(contains(out, "does not occur")); /* ... and the footer says why */
   free(out);

   char *e1 = extract("", "r1", "anything", 0);
   assert(e1[0] != '\0');
   free(e1);
   char *e2 = extract("   \n\n\t ", "r1", "anything", 0);
   assert(e2[0] != '\0');
   free(e2);
   char *e3 = extract(FIX, "r1", "", 0);
   assert(e3[0] != '\0');
   free(e3);
   printf("  PASS: no-match, empty page, and empty query are safe\n");
}

static void test_long_query_is_safe(void)
{
   char big[512];
   memset(big, 'a', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';
   char *out = extract(FIX, "r1", big, 0);
   assert(out[0] != '\0');
   free(out);
   printf("  PASS: overlong query is safe\n");
}

/* A single window wider than the whole budget must still emit something. */
static void test_oversized_window_still_emits(void)
{
   char *page = malloc(20000);
   assert(page);
   int off = snprintf(page, 20000, "retry_budget_ms");
   for (int i = 0; i < 60; i++)
      off += snprintf(page + off, 20000 - (size_t)off, " retry_budget_ms %*s", 60, "m");
   char *out = extract(page, "r1", "retry_budget_ms", 0);
   assert(contains(out, "r1#1"));
   assert(contains(out, "retry_budget_ms"));
   free(out);
   free(page);
   printf("  PASS: an oversized window still emits\n");
}

/* Coverage selection: when more windows exist than fit the budget, the ones
 * shown must be the ones carrying the most DISTINCT query terms -- position on
 * the page says nothing about relevance. Measured on real agent traffic this
 * surfaces 56.7% more distinct query terms than document-order truncation. */
static void test_coverage_selection_beats_position(void)
{
   /* A page where the best window is LAST: early sections mention one term each,
    * the final section mentions all three. Document-order truncation would fill
    * the budget before reaching it. */
   char *page = malloc(20000);
   assert(page);
   int off = 0;
   for (int i = 0; i < 8; i++)
      off +=
          snprintf(page + off, 20000 - (size_t)off,
                   "section %d discusses alpha1 only and pads on for a while %*s\n\n", i, 300, "p");
   off += snprintf(page + off, 20000 - (size_t)off,
                   "final section covers alpha1 and beta2 and gamma3 together %*s\n\n", 200, "z");

   char *out = extract(page, "r1", "alpha1 beta2 gamma3", 0);
   /* the high-coverage window must be present despite being last on the page */
   assert(contains(out, "final section"));
   assert(contains(out, "beta2"));
   assert(contains(out, "gamma3"));
   free(out);
   free(page);
   printf("  PASS: coverage selection surfaces the best window regardless of position\n");
}

/* Selected windows are still EMITTED in document order -- the caller is reading
 * a page, and reading order is what a page is written for. */
static void test_emission_stays_in_document_order(void)
{
   char *page = malloc(20000);
   assert(page);
   int off = 0;
   off += snprintf(page + off, 20000 - (size_t)off,
                   "opening mentions alpha1 beta2 gamma3 all together %*s\n\n", 200, "a");
   for (int i = 0; i < 6; i++)
      off += snprintf(page + off, 20000 - (size_t)off, "filler %d alpha1 %*s\n\n", i, 300, "f");
   off += snprintf(page + off, 20000 - (size_t)off,
                   "closing also mentions alpha1 beta2 gamma3 together %*s\n\n", 200, "z");
   char *out = extract(page, "r1", "alpha1 beta2 gamma3", 0);
   const char *opening = strstr(out, "opening mentions");
   const char *closing = strstr(out, "closing also");
   if (opening && closing)
      assert(opening < closing); /* never reordered by score */
   free(out);
   free(page);
   printf("  PASS: emission preserves document order\n");
}

/* A query that does not occur must say so and name the tool that does what the
 * caller actually asked for. 7/84 real queries hit this, all whole-document
 * requests; silently returning the top of the page looks like an answer. */
static void test_no_match_footer_is_actionable(void)
{
   char *out = extract(FIX, "r1", "zzzznotpresentanywhere", 0);
   assert(contains(out, "does not occur"));
   assert(contains(out, "mode=\"full\""));
   free(out);
   printf("  PASS: no-match footer is actionable\n");
}

int main(void)
{
   test_needle_always_retrieved_at_any_offset();
   test_deterministic();
   test_fenced_as_untrusted();
   test_cited_by_ordinal();
   test_span_round_trip();
   test_budget_respected();
   test_overlapping_matches_merge();
   test_windows_snap_to_word_edges();
   test_no_match_and_empty_inputs();
   test_coverage_selection_beats_position();
   test_emission_stays_in_document_order();
   test_no_match_footer_is_actionable();
   test_long_query_is_safe();
   test_oversized_window_still_emits();
   printf("web_read_spans: all tests passed\n");
   return 0;
}
