/* test_kb_rrf_purity.c -- Slice 4: establish from BEHAVIOUR that kb_rrf_fuse
 * carries no hidden state.
 *
 * WHY A SEPARATE TEST
 *
 * The proposal originally called kb_rrf "pure" on the strength of its include
 * list (math/stdio/stdlib/string only). Review rejected that: a list of headers
 * shows the module has no DB or network dependency, which is not the same as
 * showing it is free of side effects or hidden state. Slice 5 relies on the
 * stronger property in two places -- it calls fusion from a second consumer, and
 * the differential harness runs two selectors in one process -- so the property
 * has to be established, not assumed.
 *
 * WHAT "NO HIDDEN STATE" MEANS HERE, CONCRETELY
 *   - repeated identical calls return identical results (no accumulation);
 *   - interleaving calls with different inputs does not perturb either;
 *   - results do not depend on call ORDER;
 *   - the function does not write through its input pointers;
 *   - it writes nothing beyond the caller's `out` array, and nothing past `max`.
 *
 * These are the failure modes that would actually bite: a static accumulator, a
 * cached last-result, or an out-of-bounds write into a neighbouring buffer. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kb_rrf.h"

/* ---- fixtures ---- */

static kb_rrf_item_t G1[] = {{"alpha", 3}, {"beta", 1}, {"gamma", 2}};
static kb_rrf_item_t G2[] = {{"gamma", 5}, {"delta", 1}};
static kb_rrf_item_t H1[] = {{"zeta", 1}, {"eta", 4}};

static int fuse_two(kb_rrf_result_t *out, int max)
{
   kb_rrf_signal_t sigs[2] = {
       {G1, (int)(sizeof(G1) / sizeof(G1[0])), 1.0, "graph"},
       {G2, (int)(sizeof(G2) / sizeof(G2[0])), 0.5, "vector"},
   };
   return kb_rrf_fuse(sigs, 2, KB_RRF_DEFAULT_K, out, max);
}

static int fuse_other(kb_rrf_result_t *out, int max)
{
   kb_rrf_signal_t sigs[1] = {{H1, (int)(sizeof(H1) / sizeof(H1[0])), 2.0, "memory"}};
   return kb_rrf_fuse(sigs, 1, KB_RRF_DEFAULT_K, out, max);
}

static int same(const kb_rrf_result_t *a, int na, const kb_rrf_result_t *b, int nb)
{
   if (na != nb)
      return 0;
   for (int i = 0; i < na; i++)
   {
      if (strcmp(a[i].id, b[i].id) != 0)
         return 0;
      if (a[i].score != b[i].score) /* exact: fusion is documented as deterministic */
         return 0;
      if (a[i].structural_weight != b[i].structural_weight)
         return 0;
      if (a[i].signal_hits != b[i].signal_hits)
         return 0;
   }
   return 1;
}

/* ---- the properties ---- */

static void test_repeated_calls_identical(void)
{
   kb_rrf_result_t a[16], b[16];
   int na = fuse_two(a, 16);
   assert(na > 0);
   for (int i = 0; i < 25; i++)
   {
      kb_rrf_result_t c[16];
      int nc = fuse_two(c, 16);
      assert(same(a, na, c, nc));
   }
   (void)b;
   printf("  PASS: repeated identical calls return identical results\n");
}

static void test_interleaving_does_not_perturb(void)
{
   /* A static accumulator or cached-last-result would show up here: run the
    * two different fusions alternately and check each still matches the value
    * it produced in isolation. */
   kb_rrf_result_t base_two[16], base_other[16];
   int n_two = fuse_two(base_two, 16);
   int n_other = fuse_other(base_other, 16);

   for (int i = 0; i < 20; i++)
   {
      kb_rrf_result_t x[16], y[16];
      int nx = fuse_two(x, 16);
      int ny = fuse_other(y, 16);
      assert(same(base_two, n_two, x, nx));
      assert(same(base_other, n_other, y, ny));
   }
   printf("  PASS: interleaved differing calls do not perturb each other\n");
}

static void test_order_independence(void)
{
   /* Result must not depend on which fusion ran first in the process. */
   kb_rrf_result_t a1[16], a2[16];
   int n1 = fuse_other(a1, 16); /* other first */
   int n2 = fuse_two(a2, 16);

   kb_rrf_result_t b2[16], b1[16];
   int m2 = fuse_two(b2, 16); /* two first */
   int m1 = fuse_other(b1, 16);

   assert(same(a1, n1, b1, m1));
   assert(same(a2, n2, b2, m2));
   printf("  PASS: results independent of call order\n");
}

static void test_inputs_are_not_written_through(void)
{
   /* The signals are caller-owned; fusion must not mutate them. */
   kb_rrf_item_t g1_copy[sizeof(G1) / sizeof(G1[0])];
   kb_rrf_item_t g2_copy[sizeof(G2) / sizeof(G2[0])];
   memcpy(g1_copy, G1, sizeof(G1));
   memcpy(g2_copy, G2, sizeof(G2));

   kb_rrf_result_t out[16];
   (void)fuse_two(out, 16);

   assert(memcmp(g1_copy, G1, sizeof(G1)) == 0);
   assert(memcmp(g2_copy, G2, sizeof(G2)) == 0);
   printf("  PASS: input signals are not written through\n");
}

static void test_respects_out_capacity(void)
{
   /* Guard bytes on both sides of a deliberately undersized out array: an
    * overrun here is exactly the failure that would corrupt a caller running
    * two selectors in one process. */
   struct
   {
      unsigned char before[64];
      kb_rrf_result_t out[2];
      unsigned char after[64];
   } framed;
   memset(&framed, 0xAB, sizeof(framed));

   int n = fuse_two(framed.out, 2);
   assert(n <= 2);

   for (size_t i = 0; i < sizeof(framed.before); i++)
      assert(framed.before[i] == 0xAB);
   for (size_t i = 0; i < sizeof(framed.after); i++)
      assert(framed.after[i] == 0xAB);
   printf("  PASS: writes stay within the caller's out capacity\n");
}

static void test_degenerate_inputs(void)
{
   kb_rrf_result_t out[8];
   /* zero signals */
   assert(kb_rrf_fuse(NULL, 0, KB_RRF_DEFAULT_K, out, 8) <= 0);
   /* a signal with no items, and one with non-positive weight, are skipped */
   kb_rrf_item_t none[1] = {{"x", 0}};
   kb_rrf_signal_t sigs[2] = {{none, 0, 1.0, "empty"}, {G1, 3, 0.0, "zero-weight"}};
   int n = kb_rrf_fuse(sigs, 2, KB_RRF_DEFAULT_K, out, 8);
   assert(n == 0);
   /* bad argument */
   assert(kb_rrf_fuse(sigs, 2, KB_RRF_DEFAULT_K, NULL, 8) == -1);
   printf("  PASS: degenerate inputs handled without state leakage\n");
}

static void test_null_trust_matches_plain_fuse(void)
{
   /* Documented contract: passing NULL trust makes fuse_trust byte-identical to
    * fuse. Slice 5 does not use the trust variant, and this is what lets it say
    * so safely. */
   kb_rrf_signal_t sigs[2] = {
       {G1, 3, 1.0, "graph"},
       {G2, 2, 0.5, "vector"},
   };
   kb_rrf_result_t a[16], b[16];
   int na = kb_rrf_fuse(sigs, 2, KB_RRF_DEFAULT_K, a, 16);
   int nb = kb_rrf_fuse_trust(sigs, 2, KB_RRF_DEFAULT_K, NULL, 0, b, 16);
   assert(same(a, na, b, nb));
   printf("  PASS: fuse_trust with NULL trust equals plain fuse\n");
}

int main(void)
{
   test_repeated_calls_identical();
   test_interleaving_does_not_perturb();
   test_order_independence();
   test_inputs_are_not_written_through();
   test_respects_out_capacity();
   test_degenerate_inputs();
   test_null_trust_matches_plain_fuse();
   printf("kb_rrf_purity: all tests passed\n");
   return 0;
}
