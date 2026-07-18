/* gateway_mutate.h: the provider-agnostic decision + snapshot/replace helpers that
 * let the inbound /v1 gateway APPLY context reduction to the live primary-agent
 * request under the correctness-first contract of proposal economizer-gateway-
 * mutation (§2.2/§2.3). Pure over a cJSON messages array; no I/O, no provider
 * knowledge, no telemetry side effects (the caller emits gateway_hard_bypass{reason}
 * from the returned reason). The core invariant: NEVER dispatch a reduced payload
 * that cannot be restored to the pristine original. */
#ifndef DEC_GATEWAY_MUTATE_H
#define DEC_GATEWAY_MUTATE_H 1

#include "economizer.h"
#include <cJSON.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Why the gateway did NOT apply the reduced payload (forwards pristine instead).
    * GW_BYPASS_NONE means "apply". The reduce_* group is the reducer's internal-error
    * class (§2.2); the rest are gateway-side outcomes. gw_bypass_reason_str yields the
    * stable snake_case label used in gateway_hard_bypass{reason}. */
   typedef enum
   {
      GW_BYPASS_NONE = 0, /* should_apply passed — mutate */
      /* reducer internal-error class (rc != 0) */
      GW_BYPASS_REDUCE_ALLOC_FAILED,
      GW_BYPASS_REDUCE_PARSE_FAILED,
      GW_BYPASS_REDUCE_INTERNAL_ASSERTION,
      GW_BYPASS_REDUCE_FORMAT_UNSUPPORTED,
      /* gateway-side outcomes */
      GW_BYPASS_NO_OP,                /* nothing changed / not a net shrink / not REDUCED */
      GW_BYPASS_STRUCTURAL_VIOLATION, /* message_history_repair found an orphaned tool pair */
      GW_BYPASS_SNAPSHOT_OOM,         /* a required deep copy failed */
      GW_BYPASS_REPLACE_FAILED,       /* installing the reduced array into the request failed */
      GW_BYPASS_CONSTRUCT_FAILED, /* a post-should_apply construction step failed pre-dispatch */
   } gw_bypass_reason_t;

   const char *gw_bypass_reason_str(gw_bypass_reason_t reason);

   /* Deep copy of a messages array (every message + string independently allocated),
    * so restoring the pristine original is independent of any retained references.
    * Returns NULL on allocation failure (cJSON_Duplicate frees its own partial work);
    * the caller MUST then hard-bypass (never send an un-restorable reduced payload).
    * `messages` is not modified. */
   cJSON *gw_snapshot_messages(const cJSON *messages);

   /* Estimated token count of a messages array (for the token-delta telemetry). */
   int gw_snapshot_token_count(cJSON *messages);

   /* Decide whether the reduced result may be applied to the live request.
    * `reduce_rc` is context_reduce's return; `res` its populated result. Returns
    * GW_BYPASS_NONE only if: the reducer reported no internal error; the result is a
    * genuine REDUCED net shrink (a no-op reduce is not a mutation); and a
    * message_history_repair run on a COPY of the reduced result reports no structural
    * violation. Otherwise returns the specific hard-bypass reason. Pure: does not
    * mutate `res` or emit telemetry. */
   gw_bypass_reason_t gw_should_apply(int reduce_rc, const reduce_result_t *res);

   /* Install `reduced` as container[key], replacing the existing array. Takes
    * ownership of `reduced` on success (it is added to `container`). Returns 0 on
    * success; on failure returns non-zero (map to GW_BYPASS_REPLACE_FAILED), leaves
    * `container` byte-intact, and does NOT take ownership (caller frees `reduced`). */
   int gw_replace_messages(cJSON *container, const char *key, cJSON *reduced);

   /* Provenance (§2.2): the gateway owns reduce_state.reduced explicitly. Mark ONLY
    * after replace succeeds; clear on EVERY hard-bypass / restore / OOM path so a
    * partially-applied request never leaves a falsely reduced=true marker that the
    * delegate seam would skip. NULL-safe. */
   void gw_provenance_mark_reduced(reduce_state_t *st);
   void gw_provenance_clear(reduce_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* DEC_GATEWAY_MUTATE_H */
