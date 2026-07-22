/* gw_mutate_stats.h: process-global telemetry sink for the economizer gateway-
 * mutation path (proposal economizer-gateway-mutation §4). Flat per-request counters
 * are lock-free atomics (the hot path); reason-keyed counters (hard_bypass{reason},
 * session_disabled_set{reason}) go through a small mutex-guarded label registry
 * because they fire only on exceptional events and their reason vocabulary spans
 * several modules — keeping the sink decoupled from each module's reason enum. All
 * counters are zero at startup and never reset in production (reset is test-only). */
#ifndef DEC_GW_MUTATE_STATS_H
#define DEC_GW_MUTATE_STATS_H 1

#include "cJSON.h" /* gw_stat_to_json param type */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Flat per-request counters (§4). Emitters land across slices 2-7. */
   typedef enum
   {
      GW_STAT_MUTATE_ATTEMPTED = 0,    /* a resolvable, non-disabled session was eligible */
      GW_STAT_MUTATE_APPLIED,          /* should_apply passed and the reduced payload was sent */
      GW_STAT_4XX_RESTORE_RESEND,      /* buffered: a mutated req 4xx'd -> restore + resend once */
      GW_STAT_5XX_DISABLE,             /* buffered: a mutated req 5xx'd -> disable, no resend */
      GW_STAT_STREAM_ERROR_DISABLE,    /* streaming: invalid-request / decoder / non-SSE disable */
      GW_STAT_SESSION_DISABLED_BLOCKS, /* a request was blocked because its session was disabled */
      GW_STAT__COUNT
   } gw_stat_t;

   /* Increment a flat counter by one (relaxed atomic; a single logical event per call). */
   void gw_stat_inc(gw_stat_t which);
   uint64_t gw_stat_get(gw_stat_t which);

   /* Increment a reason-keyed counter. `group` is a stable static label
    * ("hard_bypass" | "session_disabled_set"); `reason` is a stable static label
    * (e.g. "snapshot_oom", "4xx", "stream_decoder_error"). Unknown (group,reason)
    * pairs are registered on first use up to GW_STAT_REASON_MAX; overflow is counted
    * under (group,"_overflow") so telemetry never silently drops. */
   void gw_stat_inc_reason(const char *group, const char *reason);
   uint64_t gw_stat_get_reason(const char *group, const char *reason);

   /* Sampled pre/post token-delta (§4 gateway_token_delta_pre_post_sampled). Called on
    * every APPLIED mutation with the reducer's baseline + reduced token counts; a
    * deterministic 1-in-GW_STAT_TOKEN_SAMPLE_N sample is accumulated (sum of baseline,
    * sum of reduced, sample count) so the §6 net-shrink gate can check that the sampled
    * mean reduced < sampled mean baseline. Cheap: one relaxed-atomic counter gates the
    * sample; the sums are published before the count so a concurrent dump never sees a
    * count ahead of its sums. */
   void gw_stat_record_token_delta(int baseline_tokens, int reduced_tokens);
   uint64_t gw_stat_token_sample_count(void);
   uint64_t gw_stat_token_baseline_sum(void);
   uint64_t gw_stat_token_reduced_sum(void);

   /* Human-readable dump of every non-zero counter, one `name value` per line
    * (Prometheus-ish), for a /metrics-style endpoint or diagnostics. */
   void gw_stat_dump(FILE *out);

   /* JSON view of the counters (flat counters + token_delta + reason breakdown) for the
    * user-facing GET /v1/economizer/stats endpoint. `out` is a cJSON object to populate. */
   void gw_stat_to_json(cJSON *out);

   /* Test-only: zero every counter and clear the reason registry. */
   void gw_stat_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_GW_MUTATE_STATS_H */
