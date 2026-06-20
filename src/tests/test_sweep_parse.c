/* Unit tests for the deepening-sweep proposer-candidate parser (Part B PR-B3). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sweep.h"

static void test_basic(void)
{
   const char *j =
       "{\"candidates\":["
       "{\"seam_file\":\"src/foo.c\",\"seam_symbol\":\"helper_x\",\"claimed_callers\":5,"
       "\"rationale\":\"dup across callers\"},"
       "{\"seam_file\":\"src/net.c\",\"seam_symbol\":\"parse\",\"claimed_callers\":3}]}";
   sweep_candidate_t c[8];
   int n = sweep_parse_candidates(j, c, 8);
   assert(n == 2);
   assert(strcmp(c[0].seam_file, "src/foo.c") == 0);
   assert(strcmp(c[0].seam_symbol, "helper_x") == 0);
   assert(c[0].claimed_callers == 5);
   assert(strcmp(c[0].rationale, "dup across callers") == 0);
   assert(c[1].claimed_callers == 3);
   assert(c[1].rationale[0] == '\0'); /* optional, absent */
}

static void test_fenced_and_skips(void)
{
   /* fenced + an item missing seam_symbol (skipped) + a negative count (clamped 0) */
   const char *j = "```json\n{\"candidates\":["
                   "{\"seam_file\":\"a.c\"}," /* no symbol -> skip */
                   "{\"seam_file\":\"b.c\",\"seam_symbol\":\"g\",\"claimed_callers\":-2}]}\n```";
   sweep_candidate_t c[8];
   int n = sweep_parse_candidates(j, c, 8);
   assert(n == 1);
   assert(strcmp(c[0].seam_symbol, "g") == 0);
   assert(c[0].claimed_callers == 0); /* negative clamped */
}

static void test_cap_and_errors(void)
{
   const char *j = "{\"candidates\":["
                   "{\"seam_file\":\"a.c\",\"seam_symbol\":\"x\"},"
                   "{\"seam_file\":\"b.c\",\"seam_symbol\":\"y\"}]}";
   sweep_candidate_t c[1];
   assert(sweep_parse_candidates(j, c, 1) == 1); /* capped at max */

   assert(sweep_parse_candidates("not json", c, 1) == -1);
   assert(sweep_parse_candidates("{\"items\":[]}", c, 1) == -1); /* wrong key */
   assert(sweep_parse_candidates(NULL, c, 1) == -1);
   assert(sweep_parse_candidates(j, c, 0) == -1);

   /* empty candidates array -> 0 (valid, honest "nothing found") */
   assert(sweep_parse_candidates("{\"candidates\":[]}", c, 1) == 0);
}

int main(void)
{
   test_basic();
   test_fenced_and_skips();
   test_cap_and_errors();
   printf("sweep_parse: all tests passed\n");
   return 0;
}
