/* test_web_search_fuse.c -- URL dedup and cross-engine rank fusion.
 *
 * The two failures this guards against are opposite and unequal: merging two
 * DISTINCT pages silently loses a result, while failing to merge two identical
 * ones costs a duplicate line. The identity rules are deliberately conservative
 * for that reason, and the tests pin both directions. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db1_client/db1.h"
#include "web_search.h"
#include "web_search_fuse.h"
#include "support/store_module_fixture.h"

/* Build a borrowed result list; strings are literals, never freed. */
static void mk(web_search_result_t *r, const char *title, const char *url, const char *snip)
{
   r->title = (char *)title;
   r->url = (char *)url;
   r->snippet = (char *)snip;
}

static void test_dedup_within_one_engine(void)
{
   web_search_result_t a[3];
   mk(&a[0], "One", "https://example.com/p", "first");
   mk(&a[1], "Two", "https://other.com/q", "second");
   /* Same page, different spelling -- a scraped result page really does do this. */
   mk(&a[2], "One again", "https://Example.com:443/p#frag", "dup");

   const web_search_result_t *lists[1] = {a};
   int counts[1] = {3};
   web_search_result_t out[8];
   int n = web_search_fuse(lists, counts, 1, out, 8);

   assert(n == 2); /* three in, two distinct out */
   /* First-seen title wins, deterministically. */
   int seen_p = 0;
   for (int i = 0; i < n; i++)
      if (strstr(out[i].url, "/p"))
      {
         assert(strcmp(out[i].title, "One") == 0);
         seen_p = 1;
      }
   assert(seen_p);
   web_search_free_results(out, n);
   printf("  PASS: dedup works within a single engine's results\n");
}

/* Agreement across engines is the evidence RRF is here to use. */
static void test_agreement_outranks_a_single_top_hit(void)
{
   web_search_result_t e1[3], e2[3];
   /* `agreed` is 2nd for both engines; `solo` is 1st for one and absent elsewhere. */
   mk(&e1[0], "Solo", "https://solo.com/a", "");
   mk(&e1[1], "Agreed", "https://agreed.com/x", "");
   mk(&e1[2], "Filler1", "https://f1.com/", "");
   mk(&e2[0], "Other", "https://other.com/b", "");
   mk(&e2[1], "Agreed", "https://agreed.com/x", "");
   mk(&e2[2], "Filler2", "https://f2.com/", "");

   const web_search_result_t *lists[2] = {e1, e2};
   int counts[2] = {3, 3};
   web_search_result_t out[8];
   int n = web_search_fuse(lists, counts, 2, out, 8);

   assert(n > 0);
   /* Two second-places (1/(60+2) twice) beat one first-place (1/(60+1)). */
   assert(strcmp(out[0].url, "https://agreed.com/x") == 0);
   web_search_free_results(out, n);
   printf("  PASS: cross-engine agreement outranks a single engine's top hit\n");
}

/* The dangerous direction: never merge pages that merely look similar. */
static void test_distinct_pages_are_never_merged(void)
{
   web_search_result_t a[6];
   mk(&a[0], "A", "https://example.com/x?a=1", "");
   mk(&a[1], "B", "https://example.com/x?a=2", ""); /* query IS identity */
   mk(&a[2], "C", "https://example.com/X", "");     /* path case IS identity */
   mk(&a[3], "D", "https://www.example.com/x?a=1", "");
   /* www is NOT stripped: a site may serve different content there */
   mk(&a[4], "E", "http://example.com/x?a=1", "");       /* scheme IS identity */
   mk(&a[5], "F", "https://example.com:8443/x?a=1", ""); /* non-default port IS identity */

   const web_search_result_t *lists[1] = {a};
   int counts[1] = {6};
   web_search_result_t out[8];
   int n = web_search_fuse(lists, counts, 1, out, 8);
   assert(n == 6); /* nothing merged */
   web_search_free_results(out, n);
   printf("  PASS: distinct URLs are never merged\n");
}

/* A URL longer than kb_rrf's 256-byte id field must not collide with another
 * long URL sharing its first 255 bytes. This is the bug the index-id avoids. */
static void test_long_urls_do_not_collide(void)
{
   char u1[1200], u2[1200];
   size_t pad = 700;
   int o1 = snprintf(u1, sizeof(u1), "https://example.com/");
   int o2 = snprintf(u2, sizeof(u2), "https://example.com/");
   for (size_t i = 0; i < pad; i++)
   {
      u1[o1 + i] = 'a';
      u2[o2 + i] = 'a';
   }
   /* identical for 700 chars, then differ */
   snprintf(u1 + o1 + pad, sizeof(u1) - o1 - pad, "ONE");
   snprintf(u2 + o2 + pad, sizeof(u2) - o2 - pad, "TWO");

   web_search_result_t a[2];
   mk(&a[0], "L1", u1, "");
   mk(&a[1], "L2", u2, "");
   const web_search_result_t *lists[1] = {a};
   int counts[1] = {2};
   web_search_result_t out[4];
   int n = web_search_fuse(lists, counts, 1, out, 4);
   assert(n == 2); /* would be 1 if the id were a truncated URL */
   web_search_free_results(out, n);
   printf("  PASS: long URLs sharing a 255-byte prefix stay distinct\n");
}

/* A failed engine must not sink a working one. */
static void test_empty_and_null_lists_are_skipped(void)
{
   web_search_result_t good[2];
   mk(&good[0], "G1", "https://good.com/1", "");
   mk(&good[1], "G2", "https://good.com/2", "");

   const web_search_result_t *lists[3] = {NULL, good, good};
   int counts[3] = {0, 2, 0};
   web_search_result_t out[8];
   int n = web_search_fuse(lists, counts, 3, out, 8);
   assert(n == 2);
   web_search_free_results(out, n);

   /* All engines dead: zero results, not an error and not a crash. */
   const web_search_result_t *dead[2] = {NULL, NULL};
   int zero[2] = {0, 0};
   assert(web_search_fuse(dead, zero, 2, out, 8) == 0);
   printf("  PASS: failed engines are skipped, not fatal\n");
}

/* An unparseable URL must get its own identity, or every such URL would collapse
 * into one bucket and all but the first would vanish. */
static void test_unparseable_urls_stay_distinct(void)
{
   web_search_result_t a[3];
   mk(&a[0], "X", "not a url", "");
   mk(&a[1], "Y", "also not a url", "");
   mk(&a[2], "Z", "gopher://weird/", "");
   const web_search_result_t *lists[1] = {a};
   int counts[1] = {3};
   web_search_result_t out[8];
   int n = web_search_fuse(lists, counts, 1, out, 8);
   assert(n == 3);
   web_search_free_results(out, n);
   printf("  PASS: unparseable URLs do not collapse together\n");
}

static void test_bad_arguments(void)
{
   web_search_result_t out[4];
   const web_search_result_t *lists[1] = {NULL};
   int counts[1] = {0};
   assert(web_search_fuse(NULL, counts, 1, out, 4) == -1);
   assert(web_search_fuse(lists, NULL, 1, out, 4) == -1);
   assert(web_search_fuse(lists, counts, 0, out, 4) == -1);
   assert(web_search_fuse(lists, counts, WEB_SEARCH_MAX_ENGINES + 1, out, 4) == -1);
   assert(web_search_fuse(lists, counts, 1, NULL, 4) == -1);
   assert(web_search_fuse(lists, counts, 1, out, 0) == -1);
   printf("  PASS: bad arguments are rejected, never crash\n");
}

/* Same input must give the same order every time. */
static void test_deterministic(void)
{
   web_search_result_t e1[2], e2[2];
   mk(&e1[0], "A", "https://a.com/", "");
   mk(&e1[1], "B", "https://b.com/", "");
   mk(&e2[0], "B", "https://b.com/", "");
   mk(&e2[1], "A", "https://a.com/", "");
   const web_search_result_t *lists[2] = {e1, e2};
   int counts[2] = {2, 2};

   char first[2][256];
   memset(first, 0, sizeof(first)); /* memcmp below must not read uninitialised tail bytes */
   for (int run = 0; run < 2; run++)
   {
      web_search_result_t out[8];
      int n = web_search_fuse(lists, counts, 2, out, 8);
      assert(n == 2);
      for (int i = 0; i < n; i++)
         snprintf(first[run] + i * 64, 64, "%s", out[i].url);
      web_search_free_results(out, n);
   }
   assert(memcmp(first[0], first[1], sizeof(first[0])) == 0);
   printf("  PASS: fusion order is deterministic\n");
}

int main(void)
{
   /* The store is a module now. Without one attached every db1_* call below
      fails, so bring the real one up -- or skip, saying why, on a machine with
      no database to point it at. */
   if (!store_module_fixture_available())
      return 0;
   store_module_fixture_start();

   test_dedup_within_one_engine();
   test_agreement_outranks_a_single_top_hit();
   test_distinct_pages_are_never_merged();
   test_long_urls_do_not_collide();
   test_empty_and_null_lists_are_skipped();
   test_unparseable_urls_stay_distinct();
   test_bad_arguments();
   test_deterministic();
   printf("web_search_fuse: all tests passed\n");
   return 0;
}
