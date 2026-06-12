/* test_token_tracker.c: unit tests for token usage tracking and cost estimation */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
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

static void test_priced_disambiguation(void)
{
   /* token_estimate_cost_ex reports whether the model is PRICED (known), so a
    * caller can tell a free model (cost 0, priced) from an unknown one (cost 0,
    * not priced) instead of flat-rating the free one. */
   token_usage_t u = {.input_tokens = 1000, .output_tokens = 500};
   int priced = -1;

   /* A model the static table prices is known. */
   double known = token_estimate_cost_ex("gpt-4o-2024-11-20", &u, &priced);
   assert(priced == 1);
   assert(known > 0.0);

   /* An unknown model is not priced (so the delegate path flat-rates it, not the
    * cost authority). */
   priced = -1;
   double unknown = token_estimate_cost_ex("some-unknown-model-xyz", &u, &priced);
   assert(priced == 0);
   assert(near_equal(unknown, 0.0));

   /* A zero-token request against a known model is priced (0 cost, but known). */
   token_usage_t zero = {0};
   priced = -1;
   double free_known = token_estimate_cost_ex("gpt-4o-2024-11-20", &zero, &priced);
   assert(priced == 1);
   assert(near_equal(free_known, 0.0));
   PASS("cost: priced (known) vs unknown disambiguation");
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
   /* Prefix matching (on the provider-stripped id) means a short static key
    * embedded mid-string must NOT match — the false positive substring matching
    * allowed. "my-service-o1-wrapper" contains "o1" but is not o1. */
   token_usage_t u = {.input_tokens = 1000000, .output_tokens = 1000000};
   assert(near_equal(token_estimate_cost("acme-internal-summarizer-v2", &u), 0.0));
   assert(near_equal(token_estimate_cost("my-llama3-finetune", &u), 0.0));
   assert(near_equal(token_estimate_cost("my-service-o1-wrapper", &u), 0.0)); /* not o1 */
   assert(near_equal(token_estimate_cost("acme-gpt-4o-clone", &u), 0.0));     /* not gpt-4o */

   /* A provider-qualified id still resolves by stripping the prefix. */
   token_usage_t in = {.input_tokens = 1000000};
   assert(near_equal(token_estimate_cost("anthropic/claude-opus-4", &in), 15.0));
   assert(near_equal(token_estimate_cost("openrouter:openai/gpt-4o", &in), 2.50));
   PASS("cost: prefix match — no mid-string false match");
}

static int fake_registry_price(const char *model, double *in_per_mtok, double *out_per_mtok)
{
   if (strcmp(model, "claude-3-5-sonnet") == 0)
   {
      *in_per_mtok = 99.0; /* deliberately != the static table's $3 base */
      *out_per_mtok = 1.0;
      return 1;
   }
   if (strcmp(model, "registry-only-model") == 0)
   {
      *in_per_mtok = 5.0;
      *out_per_mtok = 7.0;
      return 1;
   }
   return 0;
}

static void test_registry_overrides_base_keeps_cache(void)
{
   token_tracker_set_registry_price_fn(fake_registry_price);

   /* claude-3-5-sonnet is in the static table (base $3/$15, cache $3.75/$0.30).
    * The registry is authoritative for BASE, so the override ($99 in) wins... */
   token_usage_t base_in = {.input_tokens = 1000000};
   assert(near_equal(token_estimate_cost("claude-3-5-sonnet", &base_in), 99.0));
   /* ...while cache pricing stays from the static table (registry has none). */
   token_usage_t cw = {.cache_write_tokens = 1000000};
   assert(near_equal(token_estimate_cost("claude-3-5-sonnet", &cw), 3.75));

   /* A model only the registry knows is priced by it (no static cache). */
   assert(near_equal(token_estimate_cost("registry-only-model", &base_in), 5.0));

   token_tracker_set_registry_price_fn(NULL); /* restore for other tests */
   PASS("cost: registry overrides base, keeps static cache");
}

static void test_cost_shaped_reward(void)
{
   /* Failure always scores 0 regardless of cost. */
   assert(near_equal(cost_shaped_reward(0, 0.0, 30, 500), 0.0));
   assert(near_equal(cost_shaped_reward(0, 1.0, 30, 500), 0.0));

   /* A free success scores a full 1.0. */
   assert(near_equal(cost_shaped_reward(1, 0.0, 30, 500), 1.0));

   /* lambda<=0 or ref<=0 disables shaping -> raw success. */
   assert(near_equal(cost_shaped_reward(1, 1.0, 0, 500), 1.0));
   assert(near_equal(cost_shaped_reward(1, 1.0, 30, 0), 1.0));

   /* ref = 500 milli-USD = $0.50. A success costing exactly the reference (or
    * more) is discounted by the full lambda (30% -> reward 0.70). */
   assert(near_equal(cost_shaped_reward(1, 0.50, 30, 500), 0.70));
   assert(near_equal(cost_shaped_reward(1, 5.00, 30, 500), 0.70));

   /* Half the reference cost -> half the penalty (0.15 -> reward 0.85). */
   assert(near_equal(cost_shaped_reward(1, 0.25, 30, 500), 0.85));

   PASS("cost: cost-shaped bandit reward");
}

/* --- Main --- */

int main(void)
{
   printf("token_tracker: unit tests\n");
   test_registry_overrides_base_keeps_cache();
   test_cost_shaped_reward();

   test_known_anthropic_model();
   test_cache_tokens_anthropic();
   test_openai_model();
   test_unknown_model();
   test_priced_disambiguation();
   test_null_usage();
   test_null_model();
   test_zero_tokens();
   test_specific_match_wins();
   test_compound_id_no_false_match();

   printf("All token_tracker tests passed.\n");
   return 0;
}
