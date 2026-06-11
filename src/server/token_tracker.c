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

static const char *token_strcasestr_local(const char *haystack, const char *needle)
{
   if (!haystack || !needle)
      return NULL;
   if (!*needle)
      return haystack;

   size_t needle_len = strlen(needle);
   for (const char *p = haystack; *p; p++)
   {
      size_t i = 0;
      while (i < needle_len && p[i] && token_ascii_eq_ci(p[i], needle[i]))
         i++;
      if (i == needle_len)
         return p;
   }
   return NULL;
}

/* Return the most specific (longest) matching entry rather than the first one
 * in table order. With first-match, the result depends on row ordering — e.g.
 * "gpt-4o-mini" must precede "gpt-4o" — which is a silent mispricing hazard if
 * the table is ever reordered. Longest-match makes the lookup order-independent
 * (the longer substring is always the more specific model) while preserving the
 * results the carefully-ordered table already produces. */
static const model_price_t *find_price(const char *model)
{
   if (!model || !model[0])
      return NULL;
   const model_price_t *best = NULL;
   size_t best_len = 0;
   for (int i = 0; i < PRICING_COUNT; i++)
   {
      if (token_strcasestr_local(model, pricing[i].model_substr))
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
   if (!usage)
      return 0.0;

   /* Base input/output and cache prices ($/MTok). The static table seeds both;
    * the registry (operator / models.dev overrides + providers the table omits)
    * is then authoritative for the BASE prices, so an override actually takes
    * effect. Cache prices stay from the static table — the registry carries none.
    * A 0/0 registry entry means "unknown", not "free", and does not override. */
   double in_mtok = 0.0, out_mtok = 0.0, cw_mtok = 0.0, cr_mtok = 0.0;
   const model_price_t *p = find_price(model);
   if (p)
   {
      in_mtok = p->input_per_mtok;
      out_mtok = p->output_per_mtok;
      cw_mtok = p->cache_write_per_mtok;
      cr_mtok = p->cache_read_per_mtok;
   }
   if (model && model[0] && g_registry_price_fn)
   {
      double r_in = 0.0, r_out = 0.0;
      if (g_registry_price_fn(model, &r_in, &r_out) && (r_in > 0.0 || r_out > 0.0))
      {
         in_mtok = r_in;
         out_mtok = r_out;
      }
   }

   return (double)usage->input_tokens * in_mtok / 1e6 +
          (double)usage->output_tokens * out_mtok / 1e6 +
          (double)usage->cache_write_tokens * cw_mtok / 1e6 +
          (double)usage->cache_read_tokens * cr_mtok / 1e6;
}
