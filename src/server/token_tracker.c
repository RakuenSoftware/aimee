/* token_tracker.c: token usage normalisation and cost estimation */
#include "token_tracker.h"
#include <ctype.h>
#include <string.h>

/* --- Pricing table (USD per million tokens) ---
 *
 * pricing_refreshed: 2026-06-11. Static fallback prices for models the registry
 * does not cover; operator / models.dev overrides flow through the registry
 * fallback in token_estimate_cost. Re-verify against published provider pricing
 * when bumping this date. */
#define TOKEN_PRICING_REFRESHED "2026-06-11"

typedef struct
{
   const char *model_substr; /* case-insensitive substring match */
   double input_per_mtok;
   double output_per_mtok;
   double cache_write_per_mtok;
   double cache_read_per_mtok;
} model_price_t;

static const model_price_t pricing[] = {
    /* Anthropic claude-4 family */
    {"claude-opus-4", 15.00, 75.00, 18.75, 1.50},
    {"claude-sonnet-4", 3.00, 15.00, 3.75, 0.30},
    {"claude-haiku-4", 0.80, 4.00, 1.00, 0.08},

    /* Anthropic claude-3.7 / 3.5 family */
    {"claude-3-7-sonnet", 3.00, 15.00, 3.75, 0.30},
    {"claude-3-5-sonnet", 3.00, 15.00, 3.75, 0.30},
    {"claude-3-5-haiku", 0.80, 4.00, 1.00, 0.08},

    /* Anthropic claude-3 family */
    {"claude-3-opus", 15.00, 75.00, 18.75, 1.50},
    {"claude-3-sonnet", 3.00, 15.00, 3.75, 0.30},
    {"claude-3-haiku", 0.25, 1.25, 0.30, 0.03},

    /* OpenAI GPT-4o family */
    {"gpt-4o-mini", 0.15, 0.60, 0.0, 0.0},
    {"gpt-4o", 2.50, 10.00, 0.0, 0.0},

    /* OpenAI o-series */
    {"o3-mini", 1.10, 4.40, 0.0, 0.0},
    {"o3", 10.00, 40.00, 0.0, 0.0},
    {"o1-mini", 1.10, 4.40, 0.0, 0.0},
    {"o1", 15.00, 60.00, 0.0, 0.0},

    /* OpenAI GPT-4 classic */
    {"gpt-4-turbo", 10.00, 30.00, 0.0, 0.0},
    {"gpt-4", 30.00, 60.00, 0.0, 0.0},
    {"gpt-3.5-turbo", 0.50, 1.50, 0.0, 0.0},
};

#define PRICING_COUNT (int)(sizeof(pricing) / sizeof(pricing[0]))

static int token_ascii_eq_ci(char a, char b)
{
   return tolower((unsigned char)a) == tolower((unsigned char)b);
}

/* Strip a leading provider qualifier ("provider/model" or "provider:model",
 * possibly nested) by taking the segment after the last '/' or ':'. */
static const char *token_normalize_model(const char *model)
{
   const char *s = model;
   const char *sep = strrchr(s, '/');
   if (sep)
      s = sep + 1;
   sep = strrchr(s, ':');
   if (sep)
      s = sep + 1;
   return s;
}

/* Case-insensitive: does `id` begin with `prefix`? */
static int token_starts_with_ci(const char *id, const char *prefix)
{
   while (*prefix)
   {
      if (!*id || !token_ascii_eq_ci(*id, *prefix))
         return 0;
      id++;
      prefix++;
   }
   return 1;
}

/* Match the table key as a PREFIX of the (provider-stripped) model id, taking the
 * most specific (longest) match. Prefix rather than substring avoids false
 * positives from short keys appearing mid-string — e.g. "o1" must not match
 * "my-service-o1-wrapper" — while still matching dated/versioned ids like
 * "gpt-4o-2024-11-20" and provider-qualified ids like "anthropic/claude-opus-4".
 * Longest-match keeps the lookup independent of table order. */
static const model_price_t *find_price(const char *model)
{
   if (!model || !model[0])
      return NULL;
   const char *id = token_normalize_model(model);
   const model_price_t *best = NULL;
   size_t best_len = 0;
   for (int i = 0; i < PRICING_COUNT; i++)
   {
      if (token_starts_with_ci(id, pricing[i].model_substr))
      {
         size_t len = strlen(pricing[i].model_substr);
         if (len > best_len)
         {
            best = &pricing[i];
            best_len = len;
         }
      }
   }
   return best;
}

/* --- Registry-price fallback hook ---
 *
 * Installed by token_tracker_registry.c (linked into the server and the pricing
 * tests) so token_estimate_cost can price providers absent from the static table
 * (gemini, groq, mistral) and honour models.dev / operator overrides — one cost
 * authority — without a hard token_tracker -> model_registry link dependency on
 * the many unrelated binaries that link token_tracker.o. A function pointer (vs a
 * weak symbol) is used deliberately: under -flto a weak default defined in this
 * TU is inlined into token_estimate_cost and the strong override never takes
 * effect, whereas the indirect call cannot be devirtualised away. */
static token_registry_price_fn g_registry_price_fn = NULL;

void token_tracker_set_registry_price_fn(token_registry_price_fn fn)
{
   g_registry_price_fn = fn;
}

/* --- Cost estimation --- */

double token_estimate_cost(const char *model, const token_usage_t *usage)
{
   return token_estimate_cost_ex(model, usage, NULL);
}

double token_estimate_cost_ex(const char *model, const token_usage_t *usage, int *priced)
{
   if (priced)
      *priced = 0;
   if (!usage)
      return 0.0;

   /* Base input/output and cache prices ($/MTok). The static table seeds both;
    * the registry (operator / models.dev overrides + providers the table omits)
    * is then authoritative for the BASE prices, so an override actually takes
    * effect. Cache prices stay from the static table — the registry carries none.
    * A 0/0 registry entry means "unknown", not "free", and does not override.
    * `known` tracks whether any source actually prices the model: a static-table
    * match (a 0 there is an intentional free price) or a nonzero registry price.
    * It lets the delegate path tell a free model (cost 0, known) from an unknown
    * one (cost 0, not known) rather than flat-rating the free one. */
   double in_mtok = 0.0, out_mtok = 0.0, cw_mtok = 0.0, cr_mtok = 0.0;
   int known = 0;
   const model_price_t *p = find_price(model);
   if (p)
   {
      in_mtok = p->input_per_mtok;
      out_mtok = p->output_per_mtok;
      cw_mtok = p->cache_write_per_mtok;
      cr_mtok = p->cache_read_per_mtok;
      known = 1;
   }
   if (model && model[0] && g_registry_price_fn)
   {
      double r_in = 0.0, r_out = 0.0;
      if (g_registry_price_fn(model, &r_in, &r_out) && (r_in > 0.0 || r_out > 0.0))
      {
         in_mtok = r_in;
         out_mtok = r_out;
         known = 1;
      }
   }

   if (priced)
      *priced = known;
   return (double)usage->input_tokens * in_mtok / 1e6 +
          (double)usage->output_tokens * out_mtok / 1e6 +
          (double)usage->cache_write_tokens * cw_mtok / 1e6 +
          (double)usage->cache_read_tokens * cr_mtok / 1e6;
}

double cost_shaped_reward(int success, double cost_usd, int lambda_pct, int ref_usd_milli)
{
   double base = success ? 1.0 : 0.0;
   if (!success)
      return 0.0;
   if (lambda_pct <= 0 || ref_usd_milli <= 0)
      return base;
   double ref_usd = (double)ref_usd_milli / 1000.0;
   double frac = cost_usd / ref_usd;
   if (frac < 0.0)
      frac = 0.0;
   if (frac > 1.0)
      frac = 1.0;
   double reward = base - ((double)lambda_pct / 100.0) * frac;
   if (reward < 0.0)
      reward = 0.0;
   if (reward > 1.0)
      reward = 1.0;
   return reward;
}
