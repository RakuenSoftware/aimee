/* fold_budget.c: per-model context-fold budget resolver (fold §7, P1.5).
 * See fold_budget.h for the contract. Pure: model_registry table lookup only. */
#include "fold_budget.h"
#include "model_registry.h"

#include <stdint.h>
#include <string.h>

/* pct of window, clamped to [0, window]. The intermediate product uses a
 * fixed-width 64-bit type: `long` is 32-bit on LLP64 (Windows) and ILP32, where
 * window*pct would overflow (signed UB) before any clamp could run. */
static int pct_of(int window, int pct)
{
   if (window <= 0 || pct <= 0)
      return 0;
   int64_t v = (int64_t)window * (int64_t)pct / 100;
   if (v > window)
      v = window; /* pct > 100 clamps to the whole window (see header contract) */
   if (v < 0)
      v = 0; /* defensive: unreachable given the guards above */
   return (int)v;
}

int fold_budget_resolve(const char *model_id, const fold_budget_config_t *cfg, fold_budget_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));

   int window = 0;
   if (model_id && model_id[0])
      window = model_context_window(model_id); /* 0 if unknown */
   if (window > 0)
   {
      out->is_known = 1;
   }
   else
   {
      out->is_known = 0;
      window = (cfg && cfg->context_window_tokens > 0) ? cfg->context_window_tokens
                                                       : FOLD_BUDGET_DEFAULT_WINDOW;
   }
   out->context_window_tokens = window;

   int rb = (cfg && cfg->retained_band_pct > 0) ? cfg->retained_band_pct
                                                : FOLD_BUDGET_DEFAULT_RETAINED_PCT;
   int tc = (cfg && cfg->tail_cap_pct > 0) ? cfg->tail_cap_pct : FOLD_BUDGET_DEFAULT_TAIL_CAP_PCT;
   int pc = (cfg && cfg->pressure_ceiling_pct > 0) ? cfg->pressure_ceiling_pct
                                                   : FOLD_BUDGET_DEFAULT_PRESSURE_PCT;
   int ps = (cfg && cfg->prefix_saturation_pct > 0) ? cfg->prefix_saturation_pct
                                                    : FOLD_BUDGET_DEFAULT_SATURATION_PCT;

   out->retained_band_tokens = pct_of(window, rb);
   out->tail_cap_tokens = pct_of(window, tc);
   out->pressure_ceiling_tokens = pct_of(window, pc);
   out->prefix_saturation_tokens = pct_of(window, ps);
   out->closet_budget_tokens = (cfg && cfg->closet_budget_tokens > 0)
                                   ? cfg->closet_budget_tokens
                                   : FOLD_BUDGET_DEFAULT_CLOSET_TOKENS;
   if (out->closet_budget_tokens > window) /* §7 single enforcement point */
      out->closet_budget_tokens = window;
   return 0;
}
