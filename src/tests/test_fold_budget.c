/* test_fold_budget.c: unit tests for the per-model fold budget resolver (§7, P1.5). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../headers/fold_budget.h"
#include "../headers/model_registry.h"

#define PASS(name) printf("  PASS: %s\n", name)

static void test_known_model_defaults(void)
{
   /* Derive the expected window from the registry so model-data churn does not
    * break the resolver test; assert arithmetic-relative values. */
   int w = model_context_window("claude-opus-4-8");
   assert(w > 0);
   fold_budget_t b;
   assert(fold_budget_resolve("claude-opus-4-8", NULL, &b) == 0);
   assert(b.is_known == 1);
   assert(b.context_window_tokens == w);
   assert(b.retained_band_tokens == w * 25 / 100);
   assert(b.tail_cap_tokens == w * 15 / 100);
   assert(b.pressure_ceiling_tokens == w * 85 / 100);
   assert(b.prefix_saturation_tokens == w * 50 / 100);
   assert(b.closet_budget_tokens == FOLD_BUDGET_DEFAULT_CLOSET_TOKENS);

   fold_budget_t g;
   int gw = model_context_window("gpt-4o");
   assert(gw > 0);
   assert(fold_budget_resolve("gpt-4o", NULL, &g) == 0);
   assert(g.is_known == 1 && g.context_window_tokens == gw);
   PASS("known_model_defaults");
}

static void test_unknown_model_fallback(void)
{
   fold_budget_t b;
   /* unknown, no config -> built-in default window */
   assert(fold_budget_resolve("totally-made-up-model-x", NULL, &b) == 0);
   assert(b.is_known == 0);
   assert(b.context_window_tokens == FOLD_BUDGET_DEFAULT_WINDOW);

   /* unknown, config fallback window applies */
   fold_budget_config_t cfg = {0};
   cfg.context_window_tokens = 64000;
   fold_budget_t c;
   assert(fold_budget_resolve("totally-made-up-model-x", &cfg, &c) == 0);
   assert(c.is_known == 0 && c.context_window_tokens == 64000);
   assert(c.pressure_ceiling_tokens == 54400); /* 85% of 64000 */

   /* NULL model_id is treated as unknown, still resolves */
   fold_budget_t n;
   assert(fold_budget_resolve(NULL, NULL, &n) == 0);
   assert(n.is_known == 0 && n.context_window_tokens == FOLD_BUDGET_DEFAULT_WINDOW);
   PASS("unknown_model_fallback");
}

static void test_overrides(void)
{
   fold_budget_config_t cfg = {0};
   cfg.retained_band_pct = 10;
   cfg.tail_cap_pct = 5;
   cfg.closet_budget_tokens = 1000;
   fold_budget_t b;
   assert(fold_budget_resolve("claude-opus-4-8", &cfg, &b) == 0);
   assert(b.retained_band_tokens == 20000); /* 10% of 200000 */
   assert(b.tail_cap_tokens == 10000);      /* 5% */
   assert(b.closet_budget_tokens == 1000);
   /* unset knobs still take defaults */
   assert(b.pressure_ceiling_tokens == 170000); /* 85% */
   PASS("overrides");
}

static void test_determinism(void)
{
   fold_budget_config_t cfg = {.retained_band_pct = 30, .closet_budget_tokens = 777};
   fold_budget_t a, b;
   assert(fold_budget_resolve("gemini-1.5-pro", &cfg, &a) == 0);
   assert(fold_budget_resolve("gemini-1.5-pro", &cfg, &b) == 0);
   assert(memcmp(&a, &b, sizeof(a)) == 0); /* identical inputs -> identical output */
   assert(a.context_window_tokens == 1000000);
   PASS("determinism");
}

static void test_clamping(void)
{
   int w = model_context_window("claude-opus-4-8");
   assert(w > 0);
   /* pct > 100 clamps to the whole window (documented contract) */
   fold_budget_config_t over = {.retained_band_pct = 150};
   fold_budget_t b;
   assert(fold_budget_resolve("claude-opus-4-8", &over, &b) == 0);
   assert(b.retained_band_tokens == w);
   /* closet budget larger than the window clamps to the window */
   fold_budget_config_t big = {.closet_budget_tokens = w + 1000000};
   fold_budget_t c;
   assert(fold_budget_resolve("claude-opus-4-8", &big, &c) == 0);
   assert(c.closet_budget_tokens == w);
   PASS("clamping");
}

static void test_bad_args(void)
{
   assert(fold_budget_resolve("claude-opus-4-8", NULL, NULL) == -1);
   PASS("bad_args");
}

int main(void)
{
   printf("fold_budget tests:\n");
   test_known_model_defaults();
   test_unknown_model_fallback();
   test_overrides();
   test_determinism();
   test_clamping();
   test_bad_args();
   printf("ALL PASS\n");
   return 0;
}
