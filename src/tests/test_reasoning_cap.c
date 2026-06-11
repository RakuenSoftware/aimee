/* test_reasoning_cap.c: unit tests for the §5 complexity score + effort cap. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "reasoning_cap.h"

#define PASS(name) printf("  %s: ok\n", name)

static void test_complexity_score_bounds(void)
{
   /* Trivial turn: short, single message, no tools -> minimal score. */
   assert(reasoning_complexity_score(1, 0, 0) == 0);
   /* Score is clamped to [0,10]. */
   int big = reasoning_complexity_score(100, 1000000, 1);
   assert(big == 10);
   /* Monotone-ish: more content never lowers the score. */
   assert(reasoning_complexity_score(1, 50, 0) <= reasoning_complexity_score(1, 5000, 0));
   PASS("complexity: bounds and ordering");
}

static void test_complexity_signals(void)
{
   /* Tool presence adds to a short single-message turn. */
   assert(reasoning_complexity_score(1, 50, 1) == 2);
   assert(reasoning_complexity_score(1, 50, 0) == 0);
   /* Long content pushes into the medium/high bands. */
   assert(reasoning_complexity_score(1, 2500, 0) == 5);
   assert(reasoning_complexity_score(1, 7000, 0) == 7);
   PASS("complexity: length + tool signals");
}

static void test_cap_never_raises(void)
{
   /* Low-complexity turn caps a high configured effort down to "low". */
   assert(strcmp(reasoning_effort_capped("high", 0), "low") == 0);
   assert(strcmp(reasoning_effort_capped("xhigh", 1), "low") == 0);
   /* Already at/below the ceiling -> unchanged. */
   assert(strcmp(reasoning_effort_capped("low", 0), "low") == 0);
   /* Medium band: caps high->medium but leaves low/medium. */
   assert(strcmp(reasoning_effort_capped("high", 4), "medium") == 0);
   assert(strcmp(reasoning_effort_capped("low", 4), "low") == 0);
   /* High band: caps xhigh->high but leaves high and below. */
   assert(strcmp(reasoning_effort_capped("xhigh", 7), "high") == 0);
   assert(strcmp(reasoning_effort_capped("high", 7), "high") == 0);
   /* Max complexity: no ceiling, configured returned unchanged. */
   assert(strcmp(reasoning_effort_capped("xhigh", 10), "xhigh") == 0);
   PASS("cap: only ever lowers");
}

static void test_cap_passthrough_unknown(void)
{
   /* Empty / unrecognised effort is never given a cap (could otherwise raise). */
   assert(reasoning_effort_capped("", 0) != NULL && reasoning_effort_capped("", 0)[0] == '\0');
   assert(strcmp(reasoning_effort_capped("bogus", 0), "bogus") == 0);
   const char *null_in = reasoning_effort_capped(NULL, 0);
   assert(null_in == NULL);
   PASS("cap: passthrough for empty/unknown");
}

int main(void)
{
   printf("reasoning_cap: unit tests\n");
   test_complexity_score_bounds();
   test_complexity_signals();
   test_cap_never_raises();
   test_cap_passthrough_unknown();
   printf("All reasoning_cap tests passed.\n");
   return 0;
}
