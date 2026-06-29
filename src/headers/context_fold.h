/* context_fold.h: deterministic rolling fold of old conversation turns (§1, P2a).
 *
 * Replaces a contiguous prefix of older messages with a single synthetic
 * summary: one skeleton line per tool call / per turn, plus a Coordinate Closet
 * (§2) of the exact identifiers from the folded region. No model call, no clock,
 * no randomness. The input `messages` array is never mutated — the fold returns a
 * NEW array (synthetic summary pair + a deep copy of the retained tail).
 *
 * Atomicity / validity invariants (P2a):
 *   - The fold boundary is chosen at a CLEAN USER TURN (role "user", not a
 *     tool_result message), so a tool_use and its matching tool_result are never
 *     split across the boundary, and the retained tail always begins with a user
 *     message (valid Anthropic alternation after the synthetic user+assistant
 *     pair).
 *   - If no clean boundary leaves at least `min_fold_msgs` folded, nothing folds.
 *
 * Default-off: callers gate on cfg->enabled. The live request-path wiring,
 * byte-identical freeze, and the payload_rewrite span registry are P2b. */
#ifndef DEC_CONTEXT_FOLD_H
#define DEC_CONTEXT_FOLD_H 1

#include "coord_closet.h"
#include <cJSON.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct
   {
      int enabled;       /* 0 = off (default) */
      int retained_msgs; /* trailing messages kept at full fidelity (0 -> default) */
      int min_fold_msgs; /* fold only if >= this many messages would fold (0 -> default) */
      int reasoning_excerpt_bytes;  /* per-message text excerpt kept in the skeleton (0 -> default)
                                     */
      coord_closet_config_t closet; /* identifier-conservation config (§2) */
   } fold_config_t;

#define CONTEXT_FOLD_DEFAULT_RETAINED_MSGS 8
#define CONTEXT_FOLD_DEFAULT_MIN_FOLD_MSGS 4
#define CONTEXT_FOLD_DEFAULT_EXCERPT_BYTES 160

   typedef struct
   {
      cJSON *messages;   /* NEW array the caller owns (cJSON_Delete); NULL if no fold */
      int folded;        /* 1 if a fold happened */
      int folded_msgs;   /* number of original messages folded away */
      int retained_msgs; /* number of trailing messages kept whole */
      coord_evict_t closet_evict;
   } fold_result_t;

   /* Produce a folded view of `messages` (an Anthropic-shaped role/content array).
    * On a successful fold, out->messages is a fresh array: a synthetic user
    * (preamble + skeleton + closet) + synthetic assistant ack, followed by a deep
    * copy of the retained tail. If folding is disabled, the transcript is too
    * short, or no clean boundary exists (or on OOM), out->folded = 0 and
    * out->messages = NULL (the caller uses the original). Returns 0 on success
    * (incl. no-fold), -1 on bad args. `*out` is fully overwritten — it need not be
    * initialized, but a caller reusing a prior *folded* result must
    * fold_result_free() it first (this call does not free a pre-existing
    * out->messages). Deterministic: identical (messages, cfg) -> identical
    * out->messages serialization (relies on coord_closet_render's total-order
    * rendering for the closet block). */
   int context_fold_view(const cJSON *messages, const fold_config_t *cfg, fold_result_t *out);

   /* Free any owned resources in *out (safe on a zeroed/!folded result). */
   void fold_result_free(fold_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CONTEXT_FOLD_H */
