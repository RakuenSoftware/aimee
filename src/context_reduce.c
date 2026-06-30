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

   out->reason = REDUCE_REASON_MEASURED;
   out->mutated = 0;
   out->messages = NULL; /* no new array — caller uses its original */
   return 0;
}
