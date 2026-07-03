/* context_reduce.c: the unified context economizer orchestrator.
 *
 * Slice 1 is MEASURE-ONLY: it computes the caching-independent baseline context
 * size and the foldable opportunity (the tokens in the fold-eligible prefix
 * region), prices the opportunity as a forecast bracket, and NEVER mutates the
 * messages. Later slices add the actual levers behind this same entry point; the
 * design contracts (idempotence, immutable prefix zone, freeze-first ordering,
 * per-format boundaries, hard bypass) live in context_reduce.h. */
#include "context_reduce.h"
#include "token_tracker.h" /* token_estimate_cost_ex, token_usage_t */
#include <stdlib.h>
#include <string.h>

/* Bytes-per-token approximation shared by every estimate here. Matches the
 * chars_to_tokens convention used elsewhere (session_compact). Forecast-grade. */
#define CHARS_PER_TOKEN_EST 4

/* chars/4 token estimate of a serialized cJSON node — the same approximation
 * session_compact_estimate_tokens uses, inlined so context_reduce does not couple
 * to the whole session_compact subsystem just to count bytes. Forecast-grade only
 * (the invoice-quality number is realized provider on/off spend). */
static int node_token_estimate(cJSON *node)
{
   if (!node)
      return 0;
   char *s = cJSON_PrintUnformatted(node);
   int t = s ? (int)(strlen(s) / CHARS_PER_TOKEN_EST) : 0;
   free(s);
   return t;
}

int reduce_freeze_favorable_rates(double input_cost, double write_cost, double read_cost,
                                  int horizon)
{
   if (horizon <= 0)
      horizon = 1; /* one future reuse is enough to justify one write */
   if (horizon > FREEZE_GUARD_MAX_HORIZON)
      horizon = FREEZE_GUARD_MAX_HORIZON;

   /* Per-reuse saving = paying the cache-READ rate instead of the FRESH input rate.
    * Checked FIRST: with no read discount (read >= input) caching can never pay,
    * so skip the freeze REGARDLESS of write cost (a free write that yields no read
    * benefit is still not worth pinning a boundary for). */
   double per_reuse_saving = input_cost - read_cost;
   if (per_reuse_saving <= 0)
      return 0;

   /* The MARGINAL cost of caching is the write PREMIUM over just sending the prefix
    * fresh once (you pay the input rate on the first turn regardless) — NOT the full
    * write cost. Providers with free cache creation (OpenAI: cache_write==0) have a
    * premium <= 0, so freezing is pure upside -> always enable. */
   double write_premium = write_cost - input_cost;
   if (write_premium <= 0)
      return 1;

   return (double)horizon * per_reuse_saving >= write_premium ? 1 : 0;
}

int reduce_freeze_cost_favorable(const char *model, int prefix_tokens, int horizon)
{
   if (prefix_tokens <= 0)
      return 1; /* nothing to cache -> no churn possible */
   if (!model || !model[0])
      return 1; /* fail-open: no model -> keep prior always-on freeze behavior */

   /* Price each cache tier separately. token_estimate_cost_ex SUMS all populated
    * token_usage_t buckets, so each struct sets EXACTLY ONE bucket to isolate that
    * tier's cost. The decision is scale-INVARIANT in prefix_tokens (all three costs
    * scale linearly with it, so it cancels in the rate comparison) — prefix_tokens
    * therefore only gates the >0 "is there anything to cache" case above; any
    * positive value yields the same verdict, so the exact cached-prefix size need
    * not be known here. */
   int priced = 0;
   token_usage_t w = {0};
   w.cache_write_tokens = prefix_tokens;
   double write_cost = token_estimate_cost_ex(model, &w, &priced);
   if (!priced)
      return 1; /* fail-open: unpriced model -> do not regress onto it */

   token_usage_t in = {0};
   in.input_tokens = prefix_tokens;
   double input_cost = token_estimate_cost_ex(model, &in, NULL);
   token_usage_t rd = {0};
   rd.cache_read_tokens = prefix_tokens;
   double read_cost = token_estimate_cost_ex(model, &rd, NULL);

   return reduce_freeze_favorable_rates(input_cost, write_cost, read_cost, horizon);
}

void context_reduce_result_free(reduce_result_t *out)
{
   if (!out)
      return;
   if (out->mutated && out->messages)
      cJSON_Delete(out->messages);
   out->messages = NULL;
   out->mutated = 0;
}

/* chars/4 token estimate of the first `count` items of an array — the fold-eligible
 * prefix region. Provider-agnostic (counts serialized bytes), matching
 * session_compact_estimate_tokens' chars_to_tokens convention. */
static int prefix_token_estimate(cJSON *messages, int count)
{
   if (!messages || count <= 0)
      return 0;
   long total = 0;
   int i = 0;
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, messages)
   {
      if (i >= count)
         break;
      char *s = cJSON_PrintUnformatted(it);
      if (s)
      {
         total += (long)strlen(s);
         free(s);
      }
      i++;
   }
   return (int)(total / CHARS_PER_TOKEN_EST);
}

int context_reduce(cJSON *messages, const char *system_prompt, const char *model,
                   const char *session_id, reduce_seam_t seam, const reduce_config_t *cfg,
                   reduce_state_t *st, reduce_result_t *out)
{
   (void)session_id; /* used by the ledger writer, not the transform (Slice 1) */

   if (!out)
      return 1; /* hard bypass: caller forwards the original request */
   memset(out, 0, sizeof(*out));

   if (!messages || !cJSON_IsArray(messages))
   {
      out->reason = REDUCE_REASON_NONE;
      return 0;
   }

   /* Per-seam gate: the economizer only engages at a seam the operator enabled.
    * cfg == NULL or this seam disabled -> a true no-op (no measurement, no cost),
    * honoring the header contract that gates actually gate. */
   int seam_on = cfg && ((seam == REDUCE_SEAM_GATEWAY && cfg->gateway_seam) ||
                         (seam == REDUCE_SEAM_DELEGATE && cfg->delegate_seam));
   if (!seam_on)
   {
      out->reason = REDUCE_REASON_NONE;
      return 0;
   }

   /* Baseline = assembled-context tokens (caching-independent). The system prompt
    * is the immutable prefix zone — counted, never reduced. The +1 mirrors the
    * existing request_prompt_token_estimate: a one-token allowance so a present
    * (even tiny) system prompt is never estimated as zero. */
   int baseline = node_token_estimate(messages);
   if (system_prompt && system_prompt[0])
      baseline += (int)(strlen(system_prompt) / CHARS_PER_TOKEN_EST) + 1;
   out->baseline_tokens = baseline;
   out->reduced_tokens = baseline; /* Slice 1: measure-only, nothing removed */
   out->removed_tokens = 0;

   /* Provenance: a request can cross both seams. If a prior seam already reduced,
    * this seam re-measures the baseline but does NOT re-account the opportunity —
    * that saving belongs to the seam that performed it (avoids double-counting). */
   if (st && st->reduced)
   {
      out->reason = REDUCE_REASON_ALREADY;
      return 0;
   }

   /* Foldable opportunity: tokens ahead of the retained tail (provider-agnostic). */
   int retained =
       (cfg->fold.retained_msgs > 0) ? cfg->fold.retained_msgs : CONTEXT_FOLD_DEFAULT_RETAINED_MSGS;
   int n = cJSON_GetArraySize(messages);
   int foldable_msgs = (n > retained) ? n - retained : 0;
   out->foldable_tokens = prefix_token_estimate(messages, foldable_msgs);

   /* `work` is the current working view fed to each lever; it starts as the caller's
    * (never-mutated) `messages` and becomes a lever-owned NEW array once a lever
    * mutates. `compressed_owned` is the compress lever's array we own and must free
    * unless it is published as out->messages or consumed as fold's input copy. */
   cJSON *work = messages;
   cJSON *compressed_owned = NULL;

   /* Reduction lever (Slice 4): boundary-free tool-result BODY compression. Runs
    * FIRST (ahead of fold) so the fold — when also enabled — sees the already-shrunk
    * bodies. Unlike fold it needs no clean-user-turn boundary, so it engages on
    * autonomous tool-loops where fold never can. Never mutates `messages`; returns a
    * NEW array. */
   if (cfg->compress && !cfg->measure_only)
   {
      fold_config_t cc;
      memset(&cc, 0, sizeof(cc));
      cc.enabled = 1;
      cc.retained_msgs = cfg->fold.retained_msgs;
      cc.reasoning_excerpt_bytes = cfg->fold.reasoning_excerpt_bytes;
      cc.compact_head_bytes = cfg->fold.compact_head_bytes; /* compact.* governs both seams */
      cc.compact_tail_bytes = cfg->fold.compact_tail_bytes;
      cc.closet = cfg->fold.closet; /* zero -> module defaults; denylist borrowed for this call */

      fold_result_t cr;
      memset(&cr, 0, sizeof(cr));
      if (context_compress_view(work, &cc, &cr) != 0)
      {
         /* hard bypass: an internal compress error -> caller forwards the original. */
         fold_result_free(&cr);
         out->messages = NULL;
         out->mutated = 0;
         out->error = REDUCE_ERR_INTERNAL_ASSERTION;
         return 1;
      }
      if (cr.folded)
      {
         compressed_owned = cr.messages; /* transfer ownership; we may publish or free it */
         cr.messages = NULL;             /* so fold_result_free does not delete it */
         work = compressed_owned;        /* fold (below) compresses-then-folds this view */
         out->mutated = 1;
         out->reason = REDUCE_REASON_REDUCED;
         out->folded_msgs = cr.folded_msgs; /* bodies compressed (may be overwritten by fold) */
         if (st)
            st->reduced = 1; /* provenance: a later seam re-measures, does not re-reduce */
      }
      fold_result_free(&cr); /* cr.messages is NULL when transferred; no-op otherwise */
   }

   /* Reduction lever (Slice 2b): when history-fold is enabled and this is neither a
    * measure-only nor an already-reduced pass, actually fold the prefix via the
    * provider-agnostic context_fold_view. The fold NEVER mutates its input and
    * NEVER touches `system_prompt` (the immutable prefix zone) — it returns a NEW
    * array we transfer ownership of into out->messages. It folds `work` (the
    * possibly-compressed view), chaining the two levers. */
   if (cfg->history_fold && !cfg->measure_only)
   {
      /* Net-gain pre-check: skip when the foldable opportunity is below the
       * operator's round-trip recovery threshold (no mutation; ledger-auditable).
       * When compress already mutated, we fall through to publish that result. */
      if (cfg->min_gain_tokens > 0 && out->foldable_tokens < cfg->min_gain_tokens)
      {
         if (!compressed_owned)
         {
            out->reason = REDUCE_REASON_SKIP_NO_GAIN;
            out->mutated = 0;
            out->messages = NULL;
            return 0;
         }
         /* compress mutated -> publish below; do not run fold */
      }
      else
      {
         fold_config_t fc;
         memset(&fc, 0, sizeof(fc));
         fc.enabled = 1;
         fc.retained_msgs = cfg->fold.retained_msgs;
         fc.min_fold_msgs = cfg->fold.min_fold_msgs;
         fc.reasoning_excerpt_bytes = cfg->fold.reasoning_excerpt_bytes;
         fc.register_enabled = cfg->fold.register_enabled;
         fc.closet = cfg->fold.closet; /* zero -> defaults; denylist borrowed for this call */

         /* Freeze cost guardrail: pin the boundary only when the cache-read savings
          * cover the cache-write churn (forecast via the prefix-region token estimate);
          * otherwise pass NULL so this turn re-derives without pinning. Disabled or
          * cost-favorable -> freeze as before. NULL state -> freeze already off. */
         fold_freeze_t *freeze_arg = st ? &st->freeze : NULL;
         if (freeze_arg && cfg->freeze_guard_enabled &&
             !reduce_freeze_cost_favorable(model, out->foldable_tokens, cfg->freeze_guard_horizon))
         {
            freeze_arg = NULL;
            out->freeze_guarded = 1;
         }

         fold_result_t fr;
         memset(&fr, 0, sizeof(fr));
         if (context_fold_view(work, &fc, freeze_arg, &fr) != 0)
         {
            /* hard bypass: an internal fold error -> caller forwards the original. */
            fold_result_free(&fr);
            if (compressed_owned)
               cJSON_Delete(compressed_owned);
            out->messages = NULL;
            out->mutated = 0;
            out->error = REDUCE_ERR_INTERNAL_ASSERTION;
            return 1;
         }
         if (fr.folded)
         {
            out->messages = fr.messages; /* transfer ownership */
            fr.messages = NULL;          /* so fold_result_free does not delete it */
            out->mutated = 1;
            out->reason = REDUCE_REASON_REDUCED;
            out->folded_msgs = fr.folded_msgs;
            out->retained_msgs = fr.retained_msgs;
            out->reused_boundary = fr.reused_boundary;
            out->epochs = st ? st->freeze.epochs : 0;
            if (st)
               st->reduced = 1;   /* provenance: a later seam re-measures, does not re-reduce */
            if (compressed_owned) /* fold built its own array from the compressed copy */
            {
               cJSON_Delete(compressed_owned);
               compressed_owned = NULL;
            }
         }
         fold_result_free(&fr); /* fr.messages is NULL when transferred; no-op otherwise */
      }
   }

   /* Publish the compress-only result when fold was disabled or no-opped (fold sets
    * out->messages itself and frees compressed_owned when it folds). */
   if (compressed_owned && !out->messages)
   {
      out->messages = compressed_owned;
      compressed_owned = NULL;
   }

   /* Recompute the reduced/removed token forecast once, over whatever reduced view
    * a lever produced (compress and/or fold). The system prompt is the immutable
    * prefix zone — counted, never reduced (mirrors the baseline). */
   if (out->mutated && out->messages)
   {
      int reduced = node_token_estimate(out->messages);
      if (system_prompt && system_prompt[0])
         reduced += (int)(strlen(system_prompt) / CHARS_PER_TOKEN_EST) + 1;
      out->reduced_tokens = reduced;
      out->removed_tokens = baseline > reduced ? baseline - reduced : 0;
   }

   /* Cost forecast bracket. Basis = the realized saving (removed_tokens) once a
    * lever runs, or the foldable OPPORTUNITY in measure-only (removed==0 here).
    * floor prices the basis at the provider CACHE-READ rate (cache-warm), ceiling
    * at the FRESH input rate (cache-cold). Forecast only — the invoice-quality
    * number is realized provider on/off spend, recorded separately by the ledger. */
   int basis = out->removed_tokens > 0 ? out->removed_tokens : out->foldable_tokens;
   if (model && model[0] && basis > 0)
   {
      int priced = 0;
      token_usage_t floor_u = {0};
      floor_u.cache_read_tokens = basis;
      double floor_cost = token_estimate_cost_ex(model, &floor_u, &priced);
      if (priced) /* same model -> ceiling is priced too; only emit $ when known */
      {
         out->est_saved_cost_floor = floor_cost;
         token_usage_t ceil_u = {0};
         ceil_u.input_tokens = basis;
         out->est_saved_cost_ceiling = token_estimate_cost_ex(model, &ceil_u, NULL);
      }
   }

   /* Only the measure path falls through with reason still unset; a successful
    * fold above already stamped REDUCED (and owns out->messages). */
   if (out->reason == REDUCE_REASON_NONE)
   {
      out->reason = REDUCE_REASON_MEASURED;
      out->mutated = 0;
      out->messages = NULL; /* no new array — caller uses its original */
   }
   return 0;
}
