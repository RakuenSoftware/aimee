#ifndef AIMEE_AGENT_RETRY_USAGE_H
#define AIMEE_AGENT_RETRY_USAGE_H
#include "agent_types.h"

/* Only initialized retry owners may inspect a prior result. Per-attempt raw
 * observations stay intact; prior work is not priced as the final model. */
static inline int agent_retry_usage_tainted(const agent_result_t *prior)
{
   const aimee_response_t *r = &prior->canonical_response;
   return agent_result_usage_is_unobserved(prior) || prior->prompt_tokens > 0 ||
          prior->completion_tokens > 0 || prior->cache_read_tokens > 0 ||
          prior->cache_write_tokens > 0 || r->usage_in > 0 || r->usage_out > 0 ||
          r->usage_cache_read > 0 || r->usage_cache_write > 0 || r->usage_reasoning > 0;
}
static inline void agent_retry_usage_apply(agent_result_t *result, int tainted)
{
   if (!tainted)
      return;
   result->usage_unobserved = result->canonical_response.usage_unobserved = 1;
   result->canonical_response.usage_in = result->canonical_response.usage_out =
       result->canonical_response.usage_cache_read = result->canonical_response.usage_cache_write =
           result->canonical_response.usage_reasoning = 0;
}
/* Whole-attempt diagnostics are additive even when the attempted body is not
 * admitted. Only the owning caller decides whether that body may replace the
 * current one; canonical usage cannot price prior work as the final model. */
static inline void agent_retry_merge_attempt(agent_result_t *result, agent_result_t *attempt,
                                             int replace_body)
{
   int tainted = agent_retry_usage_tainted(replace_body ? result : attempt);
   int cache_hit = result->cache_hit && attempt->cache_hit;
   if (replace_body)
      agent_result_move_response(result, attempt);
   result->prompt_tokens += attempt->prompt_tokens;
   result->completion_tokens += attempt->completion_tokens;
   result->cache_read_tokens += attempt->cache_read_tokens;
   result->cache_write_tokens += attempt->cache_write_tokens;
   result->latency_ms += attempt->latency_ms;
   result->turns += attempt->turns;
   result->tool_calls += attempt->tool_calls;
   result->successful_tool_calls += attempt->successful_tool_calls;
   result->rescue_recoveries += attempt->rescue_recoveries;
   result->cache_hit = cache_hit;
   agent_retry_usage_apply(result, tainted || agent_result_usage_is_unobserved(result));
}
#endif
