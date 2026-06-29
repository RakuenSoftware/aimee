/* fold_budget.h: per-model context-fold budget resolver (fold §7, P1.5).
 *
 * Turns a model choice into the mechanical fold knobs deterministically — a pure
 * function of (model_id, config). It is a PREREQUISITE for the P2 rolling fold and
 * fold-freeze: byte-identical folding cannot be validated without a deterministic
 * budget. No model call, no clock, no randomness.
 *
 * The window comes from model_registry's model_context_window(); unknown models
 * fall back to a configured (or built-in default) window. The band/tail/pressure/
 * saturation knobs are fixed percentages of the window (operator-overridable). */
#ifndef DEC_FOLD_BUDGET_H
#define DEC_FOLD_BUDGET_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Resolved fold parameters for a model (all token counts). */
   typedef struct
   {
      int context_window_tokens;    /* total model window */
      int retained_band_tokens;     /* most-recent turns kept at full fidelity */
      int tail_cap_tokens;          /* rolling-tail size before the fold advances */
      int pressure_ceiling_tokens;  /* hard-fold (epoch) trigger */
      int prefix_saturation_tokens; /* freeze recompute trigger */
      int closet_budget_tokens;     /* Coordinate Closet (§2) budget, <= window */
      int is_known;                 /* int 0/1: 1 if model_id resolved to a known window */
   } fold_budget_t;

   /* Operator overrides. Convention: a field <= 0 (including a negative typo)
    * means "use the built-in default" — there is no presence mask, so a literal
    * 0% cannot be expressed (none of the knobs has a meaningful 0% today). A
    * percentage > 100 is clamped to the whole window (not rejected). Percentages
    * are of the resolved context window. closet_budget_tokens is clamped to the
    * window. This resolver does mechanical model+pct -> token translation only; it
    * does NOT enforce cross-field ordering (e.g. retained <= pressure) — the P2
    * fold consumer validates policy consistency before use. */
   typedef struct
   {
      int context_window_tokens; /* fallback window for unknown models */
      int retained_band_pct;
      int tail_cap_pct;
      int pressure_ceiling_pct;
      int prefix_saturation_pct;
      int closet_budget_tokens; /* absolute */
   } fold_budget_config_t;

#define FOLD_BUDGET_DEFAULT_WINDOW         200000
#define FOLD_BUDGET_DEFAULT_RETAINED_PCT   25
#define FOLD_BUDGET_DEFAULT_TAIL_CAP_PCT   15
#define FOLD_BUDGET_DEFAULT_PRESSURE_PCT   85
#define FOLD_BUDGET_DEFAULT_SATURATION_PCT 50
#define FOLD_BUDGET_DEFAULT_CLOSET_TOKENS  512

   /* Resolve the fold budget for model_id. cfg may be NULL (all defaults).
    * model_id may be NULL/empty (treated as an unknown model -> fallback window).
    * Returns 0 on success, -1 if out is NULL. Deterministic: identical
    * (model_id, cfg) always yields identical *out. */
   int fold_budget_resolve(const char *model_id, const fold_budget_config_t *cfg,
                           fold_budget_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_FOLD_BUDGET_H */
