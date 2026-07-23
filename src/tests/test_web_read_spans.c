/* test_web_read_spans.c -- Slice 2: fixtures and the differential oracle for
 * web_read's span selection.
 *
 * WHY THIS EXISTS
 *
 * Slice 3 converts the selection pipeline to express its two legs as ranked
 * candidate lists, gated on producing BYTE-IDENTICAL output. "Same selector, so
 * same bytes" is not a proof -- candidate production, ordering, and failure
 * paths all sit upstream of the selector and can drift silently. So byte
 * identity is a contract enforced by an oracle, not an inference.
 *
 * This file is the oracle. It reaches the selection pipeline by including the
 * translation unit (the idiom already used by test_anthropic_http.c and
 * test_provider_catalog.c), which lets it call the static selector directly and
 * therefore run with NO network I/O at all.
 *
 * OWNERSHIP: webread_select_spans() frees the `text` it is given on every path,
 * so every call site here hands it a fresh copy. That also gives each path of a
 * differential comparison genuinely independent input, which is required -- if
 * the two paths shared a mutable buffer, the first could perturb what the second
 * observes and the oracle would report an identity it never established. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../posix/web_read.c" /* static webread_select_spans, chunk_text, ... */

/* ---- helpers ---- */

/* webread_select_spans takes ownership of text; hand it a private copy. */
static char *select_on(const char *text, const char *ref, const char *query, int span)
{
   char *owned = safe_strdup(text);
   char *out = webread_select_spans(owned, ref, query, span, "https://fixture.invalid/page");
   assert(out != NULL);
   return out;
}

static int contains(const char *hay, const char *needle)
{
   return hay && needle && strstr(hay, needle) != NULL;
}

/* ---- fixtures ---- */

/* Paragraphs are sized so several chunks fall out of the ~480-byte chunker, and
 * so a specific identifier-shaped needle lands in a known one. */
static const char *FIX_BASIC =
    "Introduction paragraph about the project and its general goals, written at "
    "enough length that the chunker treats it as its own span rather than "
    "merging it with what follows. It mentions nothing specific.\n\n"
    "The configuration accepts a retry_budget_ms setting which bounds how long "
    "the client will keep retrying a failed upstream call before surfacing an "
    "error to the caller. This is the paragraph a query should find.\n\n"
    "Unrelated closing notes about licensing and attribution, again padded to a "
    "reasonable length so that it forms a distinct span in the output and can be "
    "distinguished from the paragraphs above it.\n\n";

/* ---- structural invariants of the current output ---- */

static void test_emits_untrusted_fence(void)
{
   char *out = select_on(FIX_BASIC, "r1", "retry_budget_ms", 0);
   /* Retrieved page content must always be fenced as untrusted. */
   assert(contains(out, "untrusted retrieved content"));
   free(out);
   printf("  PASS: output is fenced as untrusted\n");
}

static void test_cites_spans_by_chunk_index(void)
{
   char *out = select_on(FIX_BASIC, "r7", "retry_budget_ms", 0);
   /* The citation is `<ref>#<1-based chunk index>`. This is a PUBLIC contract:
    * it is the handle a caller passes back as span=N. Slice 3/5 introduce
    * internal candidate keys for fusion, and this assertion is what stops one
    * of them leaking into the citation. */
   assert(contains(out, "r7#"));
   assert(!contains(out, "w:")); /* no internal digest key in the output */
   free(out);
   printf("  PASS: spans cited by chunk index, no internal key leaks\n");
}

static void test_literal_needle_is_retrieved(void)
{
   char *out = select_on(FIX_BASIC, "r1", "retry_budget_ms", 0);
   /* The literal leg exists so an exact identifier is never ranked out. */
   assert(contains(out, "retry_budget_ms"));
   free(out);
   printf("  PASS: exact identifier needle is retrieved\n");
}

static void test_span_selects_exact_chunk(void)
{
   char *all = select_on(FIX_BASIC, "r1", "", 0);
   char *one = select_on(FIX_BASIC, "r1", "", 1);
   assert(contains(one, "r1#1"));
   /* span=N returns just that chunk, so it is shorter than a full selection. */
   assert(strlen(one) < strlen(all));
   free(all);
   free(one);
   printf("  PASS: span=N returns the requested chunk\n");
}

static void test_span_out_of_range(void)
{
   char *out = select_on(FIX_BASIC, "r1", "", 9999);
   assert(contains(out, "out of range"));
   free(out);
   printf("  PASS: span out of range is reported, not crashed\n");
}

/* ---- edge cases the design review enumerated ---- */

static void test_empty_query_still_returns_content(void)
{
   /* No query: neither leg can score, so the fallback leads with top-of-page.
    * It must still return something rather than an empty block. */
   char *out = select_on(FIX_BASIC, "r1", "", 0);
   assert(contains(out, "r1#1"));
   free(out);
   printf("  PASS: empty query falls back to top-of-page\n");
}

static void test_no_match_falls_back(void)
{
   char *out = select_on(FIX_BASIC, "r1", "zzzznotpresentanywhere", 0);
   /* Nothing matches either leg -- must still emit the fallback span. */
   assert(contains(out, "r1#1"));
   free(out);
   printf("  PASS: unmatched query falls back rather than emitting nothing\n");
}

static void test_query_longer_than_needle_buffer(void)
{
   /* query_needles uses a 64-byte needle buffer and only takes the whole query
    * as a phrase when strlen < 64. A longer query must not overflow or crash. */
   char big[512];
   memset(big, 'a', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';
   char *out = select_on(FIX_BASIC, "r1", big, 0);
   assert(out[0] != '\0');
   free(out);

   /* exactly at the boundary */
   char at63[64];
   memset(at63, 'b', 63);
   at63[63] = '\0';
   char *out2 = select_on(FIX_BASIC, "r1", at63, 0);
   assert(out2[0] != '\0');
   free(out2);
   printf("  PASS: overlong and boundary-length queries are safe\n");
}

static void test_empty_and_whitespace_page(void)
{
   char *out = select_on("", "r1", "anything", 0);
   assert(out[0] != '\0'); /* a footer is still emitted */
   free(out);
   char *out2 = select_on("   \n\n\t  \n", "r1", "anything", 0);
   assert(out2[0] != '\0');
   free(out2);
   printf("  PASS: empty and whitespace-only pages are safe\n");
}

static void test_more_chunks_than_cap(void)
{
   /* Build a page that segments into more than WEBREAD_MAX_CHUNKS spans and
    * confirm the cap holds rather than overrunning the chunk array. */
   size_t per = 600;
   size_t n = (size_t)WEBREAD_MAX_CHUNKS + 50;
   char *page = malloc(per * n + 1);
   assert(page);
   size_t off = 0;
   for (size_t i = 0; i < n; i++)
      off += (size_t)snprintf(page + off, per + 1,
                              "Paragraph %zu with padding text to force a chunk boundary and keep "
                              "each span comfortably above the chunker target size. %*s\n\n",
                              i, 380, "x");
   char *out = select_on(page, "r1", "Paragraph", 0);
   assert(out[0] != '\0');
   free(page);
   free(out);
   printf("  PASS: pages exceeding the chunk cap are bounded\n");
}

static void test_budget_is_respected(void)
{
   /* A page where many chunks match must not emit unboundedly: the footer
    * reports how many were shown versus found, and output stays bounded by the
    * budget plus per-span framing. */
   size_t n = 60;
   char *page = malloc(700 * n + 1);
   assert(page);
   size_t off = 0;
   for (size_t i = 0; i < n; i++)
      off += (size_t)snprintf(page + off, 701,
                              "Section %zu discusses retry_budget_ms in detail with enough padding "
                              "to form its own span in the chunker output. %*s\n\n",
                              i, 400, "y");
   char *out = select_on(page, "r1", "retry_budget_ms", 0);
   assert(contains(out, "spans shown"));
   /* Bounded: the inline budget is WEBREAD_BUDGET plus framing per span, and
    * nowhere near the whole page. */
   assert(strlen(out) < 40000);
   assert(strlen(out) < off);
   free(page);
   free(out);
   printf("  PASS: selection stays within budget on a page of all-matching spans\n");
}

static void test_chunk_in_both_legs_not_duplicated(void)
{
   /* A chunk the literal leg already emitted must not be emitted again by the
    * lexical leg -- the `used[]` bitmap is what prevents it. This is the case
    * rank fusion reinforces and a byte partition does not, so it is the most
    * likely place for Slice 3 to diverge. */
   char *out = select_on(FIX_BASIC, "r1", "retry_budget_ms", 0);
   const char *first = strstr(out, "r1#2");
   if (first)
      assert(strstr(first + 1, "r1#2") == NULL);
   free(out);
   printf("  PASS: a chunk matched by both legs is emitted once\n");
}

static void test_deterministic_across_runs(void)
{
   /* Determinism is a precondition for any byte-identity gate: if the selector
    * were order-unstable, the oracle could not distinguish drift from noise. */
   char *a = select_on(FIX_BASIC, "r1", "retry_budget_ms configuration", 0);
   for (int i = 0; i < 8; i++)
   {
      char *b = select_on(FIX_BASIC, "r1", "retry_budget_ms configuration", 0);
      assert(strcmp(a, b) == 0);
      free(b);
   }
   free(a);
   printf("  PASS: selection is deterministic across repeated runs\n");
}

/* ---- the differential oracle ----
 *
 * Slice 3 will introduce a second implementation. The gate is that it produces
 * IDENTICAL bytes to the retained one for every input below. Until that second
 * implementation exists, the oracle runs the current selector against itself on
 * independently-owned inputs, which proves the harness itself is sound: it
 * catches nondeterminism and input aliasing, the two ways a differential test
 * can silently pass without establishing anything.
 *
 * When Slice 3 lands, replace the second call with the new implementation. The
 * comparison below is already what the release gate requires: emitted length
 * and every output byte. */
typedef char *(*selector_fn)(char *text, const char *ref, const char *query, int span,
                             const char *url);

static void differential_case(selector_fn legacy, selector_fn candidate, const char *text,
                              const char *ref, const char *query, int span, const char *label)
{
   /* Independent copies: neither path may observe the other's mutations. */
   char *t1 = safe_strdup(text);
   char *t2 = safe_strdup(text);
   char *a = legacy(t1, ref, query, span, "https://fixture.invalid/page");
   char *b = candidate(t2, ref, query, span, "https://fixture.invalid/page");

   /* status */
   assert((a == NULL) == (b == NULL));
   if (a && b)
   {
      /* emitted length, then every byte */
      size_t la = strlen(a), lb = strlen(b);
      if (la != lb || memcmp(a, b, la) != 0)
      {
         fprintf(stderr, "differential mismatch [%s]\n--- legacy ---\n%s\n--- candidate ---\n%s\n",
                 label, a, b);
         assert(0 && "differential oracle: outputs differ");
      }
   }
   free(a);
   free(b);
}

static void test_differential_oracle(void)
{
   selector_fn legacy = webread_select_spans;         /* retained pre-refactor path */
   selector_fn candidate = webread_select_spans_legs; /* Slice 3 leg-based adapter */

   struct
   {
      const char *text, *query, *label;
      int span;
   } cases[] = {
       {FIX_BASIC, "retry_budget_ms", "literal hit", 0},
       {FIX_BASIC, "", "empty query", 0},
       {FIX_BASIC, "zzzznotpresent", "no match", 0},
       {FIX_BASIC, "configuration client error", "lexical only", 0},
       {FIX_BASIC, "retry_budget_ms configuration", "both legs", 0},
       {FIX_BASIC, "", "span 1", 1},
       {FIX_BASIC, "", "span out of range", 9999},
       {"", "anything", "empty page", 0},
       {"   \n\n  ", "anything", "whitespace page", 0},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
      differential_case(legacy, candidate, cases[i].text, "r1", cases[i].query, cases[i].span,
                        cases[i].label);

   /* Generated inputs: fixtures alone are a regression net, not an equivalence
    * argument. These sweep the boundaries where the two implementations could
    * diverge -- budget saturation, the chunk cap, tie-heavy scoring, and the
    * literal-reserve overflow rule (continue) versus the lexical one (break). */
   {
      /* many equally-scoring chunks: exercises tie ordering */
      size_t n = 40;
      char *page = malloc(700 * n + 1);
      assert(page);
      size_t off = 0;
      for (size_t i = 0; i < n; i++)
         off += (size_t)snprintf(page + off, 701,
                                 "Section discusses retry_budget_ms uniformly so every span scores "
                                 "the same and ties must break identically. %*s\n\n",
                                 400, "z");
      differential_case(legacy, candidate, page, "r1", "retry_budget_ms", 0, "tie-heavy");
      differential_case(legacy, candidate, page, "r1", "retry_budget_ms discusses uniformly", 0,
                        "tie-heavy multi-needle");
      free(page);
   }
   {
      /* one very large leading chunk: the first literal hit is emitted even
       * when it alone exceeds the reserve */
      char *page = malloc(4096);
      assert(page);
      snprintf(page, 4096,
               "retry_budget_ms appears in this deliberately oversized leading paragraph %*s\n\n"
               "retry_budget_ms again in a much shorter later paragraph.\n\n",
               2000, "q");
      differential_case(legacy, candidate, page, "r1", "retry_budget_ms", 0, "oversized first hit");
      free(page);
   }
   {
      /* beyond the chunk cap */
      size_t n = (size_t)WEBREAD_MAX_CHUNKS + 25;
      char *page = malloc(600 * n + 1);
      assert(page);
      size_t off = 0;
      for (size_t i = 0; i < n; i++)
         off += (size_t)snprintf(page + off, 601, "Para %zu retry_budget_ms padding %*s\n\n", i,
                                 380, "w");
      differential_case(legacy, candidate, page, "r1", "retry_budget_ms", 0, "beyond chunk cap");
      free(page);
   }
   {
      /* THE BREAK-vs-CONTINUE DISCRIMINATOR.
       *
       * The lexical leg BREAKS on the first candidate that does not fit; it
       * does not skip it and keep looking. Nothing above distinguishes those
       * two behaviours, and an oracle that cannot fail proves nothing -- an
       * earlier version of this file passed while the implementation had
       * `continue` substituted for `break`.
       *
       * Construction: paragraphs sized just under the ~480-byte chunk target so
       * each becomes one span. Three score-3 spans, then a score-2 span that
       * overflows the 1500-byte budget, then a SHORT score-1 span that would
       * still fit. Legacy stops at the overflow; a skipping implementation
       * reaches the short span and emits one more. */
      char *page = malloc(8192);
      assert(page);
      int off = 0;
      for (int i = 0; i < 3; i++)
         off += snprintf(page + off, 8192 - (size_t)off, "alpha1 beta2 gamma3 section %d %*s\n\n",
                         i, 400, "p");
      off +=
          snprintf(page + off, 8192 - (size_t)off, "alpha1 beta2 medium section %*s\n\n", 400, "m");
      off += snprintf(page + off, 8192 - (size_t)off, "alpha1 tiny.\n\n");
      differential_case(legacy, candidate, page, "r1", "alpha1 beta2 gamma3", 0,
                        "break-vs-continue discriminator");
      free(page);
   }
   {
      /* Same shape at the literal reserve rather than the total budget: the
       * literal leg CONTINUES past an oversized chunk, so a later smaller
       * literal hit is still emitted. Guards the opposite substitution. */
      char *page = malloc(8192);
      assert(page);
      int off = 0;
      off += snprintf(page + off, 8192, "alpha1 leading %*s\n\n", 430, "a");
      off += snprintf(page + off, 8192 - (size_t)off, "alpha1 second %*s\n\n", 430, "b");
      off += snprintf(page + off, 8192 - (size_t)off, "alpha1 short.\n\n");
      differential_case(legacy, candidate, page, "r1", "alpha1", 0,
                        "literal reserve continue discriminator");
      free(page);
   }

   {
      /* SEEDED RANDOMISED SWEEP.
       *
       * This is the part that actually establishes equivalence, and its shape
       * was arrived at empirically rather than by intuition.
       *
       * A hand-built fixture sweep with a UNIFORM paragraph size per page ran
       * 384 differential cases and still failed to catch `break` substituted
       * for `continue` in the lexical leg. A 200k-page randomised search then
       * showed that state is not merely rare but common -- about 10% of pages
       * expose it -- and the distinguishing ingredient is mixing very small and
       * large paragraphs WITHIN one page. Small chunks are cheap, so the
       * literal leg tends to absorb them all; only when the reserve is nearly
       * exhausted does a small low-scoring chunk survive into the lexical leg
       * behind a large one that overflows the budget.
       *
       * So the generator mirrors that: per-paragraph size drawn from a bimodal
       * distribution, per-paragraph needle density varied independently. The
       * PRNG is a fixed-seed LCG so the corpus is identical on every run --
       * a differential gate must not be flaky. */
      unsigned long rng = 12345UL;
#define SWEEP_RND(lo, hi)                                                                          \
   (rng = rng * 6364136223846793005UL + 1442695040888963407UL,                                     \
    (int)((lo) + (int)((rng >> 33) % (unsigned long)((hi) - (lo) + 1))))

      int cases_run = 0;
      for (int iter = 0; iter < 3000; iter++)
      {
         char *page = malloc(20000);
         assert(page);
         int off = 0;
         int np = SWEEP_RND(2, 14);
         for (int i = 0; i < np && off < 18000; i++)
         {
            int r = SWEEP_RND(0, 2);
            const char *terms = (r == 0)   ? "alpha1 beta2 gamma3"
                                : (r == 1) ? "alpha1 beta2"
                                           : "alpha1";
            /* bimodal: a third of paragraphs are tiny, the rest near a chunk */
            int pad = (SWEEP_RND(0, 2) == 0) ? SWEEP_RND(2, 60) : SWEEP_RND(200, 480);
            off += snprintf(page + off, 20000 - (size_t)off, "%s s%d %*s\n\n", terms, i, pad, "x");
         }
         const char *q = SWEEP_RND(0, 1) ? "alpha1 beta2 gamma3" : "alpha1";
         differential_case(legacy, candidate, page, "r1", q, 0, "randomised sweep");
         free(page);
         cases_run++;
      }
#undef SWEEP_RND
      printf("  PASS: seeded randomised sweep, %d differential cases\n", cases_run);
   }

   /* every span index across the basic fixture, including out of range */
   for (int sp = 0; sp <= 12; sp++)
      differential_case(legacy, candidate, FIX_BASIC, "r1", "retry_budget_ms", sp, "span sweep");

   printf("  PASS: differential oracle (legacy vs leg-based) over fixtures + generated cases\n");
}

int main(void)
{
   test_emits_untrusted_fence();
   test_cites_spans_by_chunk_index();
   test_literal_needle_is_retrieved();
   test_span_selects_exact_chunk();
   test_span_out_of_range();
   test_empty_query_still_returns_content();
   test_no_match_falls_back();
   test_query_longer_than_needle_buffer();
   test_empty_and_whitespace_page();
   test_more_chunks_than_cap();
   test_budget_is_respected();
   test_chunk_in_both_legs_not_duplicated();
   test_deterministic_across_runs();
   test_differential_oracle();
   printf("web_read_spans: all tests passed\n");
   return 0;
}
