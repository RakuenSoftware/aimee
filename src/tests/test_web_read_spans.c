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
#include <ctype.h>
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

/* ---- Slice 5: fusion is opt-in and NOT the default ---- */

/* The default must remain the original selector, and this has to exercise the
 * ACTUAL dispatch rule -- an earlier version compared two direct calls, which
 * asserted nothing about what tool_web_read would choose. */
static void test_dispatch_defaults_to_original(void)
{
   assert(webread_selector_for(NULL) == webread_select_spans);
   assert(webread_selector_for("") == webread_select_spans);
   assert(webread_selector_for("legacy") == webread_select_spans);
   assert(webread_selector_for("FUSION") == webread_select_spans);  /* case-sensitive opt-in */
   assert(webread_selector_for("fusion ") == webread_select_spans); /* no trimming */
   assert(webread_selector_for("nonsense") == webread_select_spans);
   /* only the two exact opt-in values select an alternative */
   assert(webread_selector_for("fusion") == webread_select_spans_fusion);
   assert(webread_selector_for("legs") == webread_select_spans_legs);
   printf("  PASS: dispatch defaults to the original; opt-in is exact-match only\n");
}

/* Needle retrieval across all three selectors.
 *
 * What this asserts, and what it deliberately does NOT:
 *
 * The tool's description promises "exact API/error/version needles guaranteed
 * in". Taken literally that promise is not achievable, and the measurements are
 * worth recording because they nearly justified a structural change that would
 * have fixed nothing:
 *
 *   - `chunk_text` splits a needle across a chunk boundary on ~1.6% of
 *     generated pages. When that happens NO selector can retrieve it -- the
 *     token no longer exists in any chunk. This is a pre-existing chunker
 *     limitation, independent of ranking.
 *   - When a page has more literal hits than the ~1500-byte budget can hold
 *     (roughly three spans), only the first few are emitted whatever the
 *     ordering. A page with 60+ literal hits drops a deep target under every
 *     selector.
 *
 * What IS true, and what this test pins: when the needle survives chunking and
 * the page is of ordinary size, every selector retrieves it. Measured at 0
 * failures across 49,220 eligible pages for the original selector, the legs
 * adapter, and fusion alike. Pages where the needle did not survive chunking
 * are skipped, because including them would assert something no implementation
 * can satisfy. */
static void test_needle_retrieved_when_chunking_preserves_it(void)
{
   unsigned long rng = 7UL;
#define GT_RND(lo, hi)                                                                             \
   (rng = rng * 6364136223846793005UL + 1442695040888963407UL,                                     \
    (int)((lo) + (int)((rng >> 33) % (unsigned long)((hi) - (lo) + 1))))

   webread_selector_fn sels[] = {webread_select_spans, webread_select_spans_legs,
                                 webread_select_spans_fusion};
   const char *names[] = {"original", "legs", "fusion"};

   int eligible = 0, skipped_split = 0;
   for (int it = 0; it < 400; it++)
   {
      char *page = malloc(20000);
      assert(page);
      int off = 0;
      int np = GT_RND(3, 16);
      int needle_at = GT_RND(0, np - 1);
      for (int i = 0; i < np && off < 18000; i++)
      {
         int pad = (GT_RND(0, 2) == 0) ? GT_RND(2, 80) : GT_RND(200, 470);
         if (i == needle_at)
            off += snprintf(page + off, 20000 - (size_t)off,
                            "exact token retry_budget_ms here %*s\n\n", pad, "n");
         else
            off += snprintf(page + off, 20000 - (size_t)off,
                            "configuration retry client upstream error caller topical %d %*s\n\n",
                            i, pad, "t");
      }

      /* eligibility: did the needle survive chunking? */
      span_t probe[WEBREAD_MAX_CHUNKS];
      int nc = chunk_text(page, probe, WEBREAD_MAX_CHUNKS);
      int intact = 0;
      for (int i = 0; i < nc && !intact; i++)
         if (istrcontains(probe[i].ptr, probe[i].len, "retry_budget_ms"))
            intact = 1;
      if (!intact)
      {
         skipped_split++;
         free(page);
         continue;
      }
      eligible++;

      for (size_t k = 0; k < sizeof(sels) / sizeof(sels[0]); k++)
      {
         char *t = safe_strdup(page);
         char *out = sels[k](t, "r1", "retry_budget_ms", 0, "u");
         assert(out);
         if (!contains(out, "retry_budget_ms"))
         {
            fprintf(stderr, "selector %s dropped an intact needle (iter %d):\n%s\n", names[k], it,
                    out);
            assert(0 && "selector dropped a needle that survived chunking");
         }
         free(out);
      }
      free(page);
   }
#undef GT_RND
   assert(eligible > 300); /* the corpus must be mostly eligible to mean anything */
   printf("  PASS: all selectors retrieve an intact needle (%d eligible, %d skipped as "
          "chunk-split)\n",
          eligible, skipped_split);
}

/* Footer accounting must be internally consistent: a span must never be counted
 * as both shown and omitted, literal-shown must not exceed shown, and shown must
 * not exceed the chunk count.
 *
 * Note on scope: review raised a path where fusion emits nothing (its top
 * candidate exceeding the budget) and the fallback emits chunk 0, which would
 * double-count. Measurement shows that is UNREACHABLE under the current
 * constants -- chunk_text never emits a span longer than WEBREAD_CHUNK (max
 * observed 480) and WEBREAD_BUDGET is 1500, so the first candidate always fits.
 * The accounting handles it anyway and a _Static_assert ties that guard to the
 * constants, but this test cannot exercise it and does not claim to. */
static void test_fusion_footer_is_consistent(void)
{
   /* single oversized matching chunk: fusion loop breaks immediately, fallback
    * emits chunk 0, and chunk 0 IS the fused candidate */
   char *page = malloc(8192);
   assert(page);
   snprintf(page, 8192, "retry_budget_ms %*s\n\n", 3000, "z");
   char *t = safe_strdup(page);
   char *out = webread_select_spans_fusion(t, "r1", "retry_budget_ms", 0, "u");
   assert(out);
   int shown = -1, total = -1, lit = -1, omitted = -1;
   const char *f = strstr(out, "-- ");
   assert(f);
   assert(sscanf(f, "-- %d of %d spans shown (%d literal, %d omitted)", &shown, &total, &lit,
                 &omitted) == 4);
   /* the one span was emitted, so it cannot also be omitted */
   assert(shown >= 1);
   assert(omitted == 0);
   assert(shown + omitted <= total || total == 0);
   free(out);
   free(page);

   /* and across the randomised corpus, shown+omitted must never exceed the
    * chunk count and omitted must never go negative */
   unsigned long rng = 31UL;
#define FC_RND(lo, hi)                                                                             \
   (rng = rng * 6364136223846793005UL + 1442695040888963407UL,                                     \
    (int)((lo) + (int)((rng >> 33) % (unsigned long)((hi) - (lo) + 1))))
   for (int it = 0; it < 300; it++)
   {
      char *p2 = malloc(20000);
      assert(p2);
      int off = 0;
      int np = FC_RND(1, 12);
      for (int i = 0; i < np && off < 18000; i++)
         off += snprintf(p2 + off, 20000 - (size_t)off, "alpha1 beta2 s%d %*s\n\n", i,
                         (FC_RND(0, 2) == 0) ? FC_RND(2, 60) : FC_RND(200, 470), "x");
      char *t2 = safe_strdup(p2);
      char *o2 = webread_select_spans_fusion(t2, "r1", "alpha1 beta2", 0, "u");
      assert(o2);
      const char *f2 = strstr(o2, "-- ");
      assert(f2);
      int sh = -1, tt = -1, li = -1, om = -1;
      assert(sscanf(f2, "-- %d of %d spans shown (%d literal, %d omitted)", &sh, &tt, &li, &om) ==
             4);
      assert(sh >= 0 && om >= 0 && li >= 0);
      assert(li <= sh);      /* literal shown cannot exceed spans shown */
      assert(sh <= tt);      /* cannot show more spans than exist */
      assert(sh + om <= tt); /* a span is never both shown and omitted */
      free(o2);
      free(p2);
   }
#undef FC_RND
   printf("  PASS: fusion footer accounting is self-consistent\n");
}

/* Fusion reorders by fused rank, so it is NOT held to byte-identity. What it
 * must still satisfy are the invariants that are contracts rather than
 * implementation details. */
static void test_fusion_respects_contracts(void)
{
   struct
   {
      const char *text, *query;
   } cases[] = {
       {FIX_BASIC, "retry_budget_ms"},      {FIX_BASIC, ""},  {FIX_BASIC, "zzzznotpresent"},
       {FIX_BASIC, "configuration client"}, {"", "anything"}, {"   \n\n ", "anything"},
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      char *t = safe_strdup(cases[i].text);
      char *out =
          webread_select_spans_fusion(t, "r3", cases[i].query, 0, "https://fixture.invalid/page");
      assert(out != NULL);
      assert(contains(out, "untrusted retrieved content"));
      /* cited by chunk index; the internal fusion key never reaches output. A
       * whitespace-only page chunks to nothing, so there is no span to cite. */
      int has_content = 0;
      for (const char *c = cases[i].text; *c; c++)
         if (!isspace((unsigned char)*c))
         {
            has_content = 1;
            break;
         }
      if (has_content)
         assert(contains(out, "r3#"));
      /* Bounded by the selection budget plus per-span framing and the footer.
       * This checks the budget is applied at all, not that it is applied to the
       * byte. */
      assert(strlen(out) < (size_t)WEBREAD_BUDGET * 4 + 1024);
      free(out);
   }
   /* span=N round-trip is preserved under fusion too */
   char *t = safe_strdup(FIX_BASIC);
   char *one = webread_select_spans_fusion(t, "r3", "", 1, "https://fixture.invalid/page");
   assert(contains(one, "r3#1"));
   free(one);
   printf("  PASS: fusion preserves fencing, chunk-index citation, budget, span=N\n");
}

/* Fusion is a BEHAVIOUR CHANGE, and that has to be demonstrated rather than
 * assumed: an opt-in selector that always agreed with the default would be dead
 * weight dressed up as an experiment. On one small fixture the two often
 * coincide -- few chunks, little for a reordering to do -- so this measures over
 * a seeded corpus of mixed-size pages instead. */
static void test_fusion_is_a_real_behaviour_change(void)
{
   unsigned long rng = 99UL;
#define FZ_RND(lo, hi)                                                                             \
   (rng = rng * 6364136223846793005UL + 1442695040888963407UL,                                     \
    (int)((lo) + (int)((rng >> 33) % (unsigned long)((hi) - (lo) + 1))))

   int tried = 0, differs = 0;
   for (int it = 0; it < 300; it++)
   {
      char *page = malloc(20000);
      assert(page);
      int off = 0;
      int np = FZ_RND(2, 14);
      for (int i = 0; i < np && off < 18000; i++)
      {
         int r = FZ_RND(0, 2);
         const char *terms = (r == 0)   ? "alpha1 beta2 gamma3"
                             : (r == 1) ? "alpha1 beta2"
                                        : "alpha1";
         int pad = (FZ_RND(0, 2) == 0) ? FZ_RND(2, 60) : FZ_RND(200, 480);
         off += snprintf(page + off, 20000 - (size_t)off, "%s s%d %*s\n\n", terms, i, pad, "x");
      }
      const char *q = FZ_RND(0, 1) ? "alpha1 beta2 gamma3" : "alpha1";
      char *t1 = safe_strdup(page), *t2 = safe_strdup(page);
      char *a = webread_select_spans(t1, "r1", q, 0, "u");
      char *b = webread_select_spans_fusion(t2, "r1", q, 0, "u");
      assert(a && b);
      assert(contains(a, "untrusted retrieved content"));
      assert(contains(b, "untrusted retrieved content"));
      if (strcmp(a, b) != 0)
         differs++;
      tried++;
      free(a);
      free(b);
      free(page);
   }
#undef FZ_RND
   /* measured ~74%; assert a loose floor so this tracks "the flag still does
    * something" rather than pinning a ratio */
   assert(differs * 4 > tried);
   printf("  PASS: fusion is a real behaviour change (%d/%d pages differ)\n", differs, tried);
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
   test_dispatch_defaults_to_original();
   test_needle_retrieved_when_chunking_preserves_it();
   test_fusion_respects_contracts();
   test_fusion_footer_is_consistent();
   test_fusion_is_a_real_behaviour_change();
   printf("web_read_spans: all tests passed\n");
   return 0;
}
