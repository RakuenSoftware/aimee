/* test_token_tracker.c: unit tests for token usage tracking and cost estimation */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "token_tracker.h"

#define PASS(name) printf("  %s: ok\n", name)

/* Floating-point near-equality with a generous epsilon for cost values */
static int near_equal(double a, double b)
{
   double diff = a - b;
   if (diff < 0)
      diff = -diff;
   return diff < 1e-9;
}

/* --- Cost estimation tests --- */

static void test_known_anthropic_model(void)
{
   /* claude-3-5-sonnet: $3.00 input, $15.00 output per million */
   token_usage_t u = {.input_tokens = 1000, .output_tokens = 500};
   double cost = token_estimate_cost("claude-3-5-sonnet-20241022", &u);
   /* Expected: 1000 * 3.00/1e6 + 500 * 15.00/1e6 = 0.003 + 0.0075 = 0.0105 */
   assert(near_equal(cost, 0.0105));
   PASS("cost: claude-3-5-sonnet");
}

static void test_cache_tokens_anthropic(void)
{
   /* claude-3-5-sonnet: cache write $3.75/M, cache read $0.30/M */
   token_usage_t u = {
       .input_tokens = 0,
       .output_tokens = 0,
       .cache_write_tokens = 1000000,
       .cache_read_tokens = 1000000,
   };
   double cost = token_estimate_cost("claude-3-5-sonnet", &u);
   /* Expected: 1e6 * 3.75/1e6 + 1e6 * 0.30/1e6 = 3.75 + 0.30 = 4.05 */
   assert(near_equal(cost, 4.05));
   PASS("cost: cache tokens");
}

static void test_openai_model(void)
{
   /* gpt-4o: $2.50 input, $10.00 output per million */
   token_usage_t u = {.input_tokens = 2000, .output_tokens = 1000};
   double cost = token_estimate_cost("gpt-4o-2024-11-20", &u);
   /* Expected: 2000 * 2.50/1e6 + 1000 * 10.00/1e6 = 0.005 + 0.01 = 0.015 */
   assert(near_equal(cost, 0.015));
   PASS("cost: gpt-4o");
}

static void test_unknown_model(void)
{
   token_usage_t u = {.input_tokens = 1000, .output_tokens = 500};
   double cost = token_estimate_cost("some-unknown-model-xyz", &u);
   assert(near_equal(cost, 0.0));
   PASS("cost: unknown model returns 0");
}

static void test_null_usage(void)
{
   double cost = token_estimate_cost("claude-3-5-sonnet", NULL);
   assert(near_equal(cost, 0.0));
   PASS("cost: null usage returns 0");
}

static void test_null_model(void)
{
   token_usage_t u = {.input_tokens = 1000, .output_tokens = 500};
   double cost = token_estimate_cost(NULL, &u);
   assert(near_equal(cost, 0.0));
   PASS("cost: null model returns 0");
}

static void test_zero_tokens(void)
{
   token_usage_t u = {0};
   double cost = token_estimate_cost("claude-3-5-sonnet", &u);
   assert(near_equal(cost, 0.0));
   PASS("cost: zero tokens = zero cost");
}

static void test_specific_match_wins(void)
{
   /* Both "gpt-4o-mini" and "gpt-4o" are substrings of a gpt-4o-mini id.
    * The lookup must pick the more specific (longer) match regardless of
    * table order — mini is $0.15/M in, not gpt-4o's $2.50/M. */
   token_usage_t u = {.input_tokens = 1000000, .output_tokens = 0};
   double mini = token_estimate_cost("gpt-4o-mini-2024-07-18", &u);
   assert(near_equal(mini, 0.15)); /* gpt-4o-mini input, not gpt-4o */

   /* o3-mini ($1.10/M) must not resolve to o3 ($10.00/M). */
   double o3mini = token_estimate_cost("o3-mini", &u);
   assert(near_equal(o3mini, 1.10));

   /* Plain gpt-4o still resolves to gpt-4o. */
   double base = token_estimate_cost("gpt-4o-2024-11-20", &u);
   assert(near_equal(base, 2.50));
   PASS("cost: most-specific model match wins");
}

static void test_compound_id_no_false_match(void)
{
   /* Realistic unrelated/compound model ids must not be priced by an accidental
    * substring hit against the static table. Anthropic/OpenAI keys are absent
    * from these ids, so they resolve to 0 here (the registry fallback, when
    * linked, prices known ids by exact lookup instead). NOTE: token_tracker's
    * static table is still substring-matched, so a short OpenAI key embedded in
    * an id (e.g. "...-o1-...") can still false-positive; eliminating that fully
    * is the registry exact-lookup path's job (a documented follow-up). */
   token_usage_t u = {.input_tokens = 1000000, .output_tokens = 1000000};
   assert(near_equal(token_estimate_cost("acme-internal-summarizer-v2", &u), 0.0));
   assert(near_equal(token_estimate_cost("my-llama3-finetune", &u), 0.0));
   PASS("cost: compound id no false match");
}

/* --- Main --- */

int main(void)
{
   printf("token_tracker: unit tests\n");

   test_known_anthropic_model();
   test_cache_tokens_anthropic();
   test_openai_model();
   test_unknown_model();
   test_null_usage();
   test_null_model();
   test_zero_tokens();
   test_specific_match_wins();
   test_compound_id_no_false_match();

   printf("All token_tracker tests passed.\n");
   return 0;
}
