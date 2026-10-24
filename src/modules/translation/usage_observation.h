/* Provider usage presence is independent of its numeric value. Call only after
 * validating counters: absent primary totals do not establish observed zero.
 * Partial observations remain in the provider sidecar, not a fabricated total. */
#ifndef AIMEE_TRANSLATION_USAGE_OBSERVATION_H
#define AIMEE_TRANSLATION_USAGE_OBSERVATION_H
#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"
static inline int wire_usage_fields(const cJSON *usage, const char *input, const char *output)
{
   return (cJSON_GetObjectItemCaseSensitive(usage, input) ? 1 : 0) |
          (cJSON_GetObjectItemCaseSensitive(usage, output) ? 2 : 0);
}
static inline void wire_response_usage(aimee_response_t *response, const cJSON *usage,
                                       const char *input, const char *output)
{
   response->usage_unobserved = wire_usage_fields(usage, input, output) != 3;
   if (response->usage_unobserved)
      response->usage_in = response->usage_out = response->usage_cache_read =
          response->usage_cache_write = response->usage_reasoning = 0;
}
static inline void wire_unknown_delta_usage(aimee_delta_t *delta, int observed)
{
   delta->usage_unobserved = !observed;
   if (!observed)
      delta->usage_in = delta->usage_out = delta->usage_cache_read = delta->usage_cache_write =
          delta->usage_reasoning = 0;
}
#endif
