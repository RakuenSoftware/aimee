/* Provider usage presence is independent of its numeric value. Call only after
 * validating counters: absent primary totals do not establish observed zero.
 * Partial observations remain in the provider sidecar, not a fabricated total. */
#ifndef AIMEE_TRANSLATION_USAGE_OBSERVATION_H
#define AIMEE_TRANSLATION_USAGE_OBSERVATION_H
#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"
/* Explicit native metadata may be absent/null, but cannot silently become a
 * missing canonical id/model when the provider supplied a different type. */
static inline int wire_response_metadata_valid(const cJSON *response)
{
   static const char *const names[] = {"id", "model"};
   for (size_t i = 0; i < sizeof names / sizeof *names; i++)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(response, names[i]);
      if (value && !cJSON_IsNull(value) && !cJSON_IsString(value))
         return 0;
   }
   return 1;
}
/* Known provider aliases remain typed even when this wire does not select them
 * as its primary totals. Unknown extension fields remain inert sidecar data. */
static inline int wire_usage_validate(const cJSON *usage)
{
   static const char *const totals[] = {"input_tokens",
                                        "prompt_tokens",
                                        "output_tokens",
                                        "completion_tokens",
                                        "cache_read_input_tokens",
                                        "cached_input_tokens",
                                        "cache_creation_input_tokens"};
   static const char *const details[] = {"prompt_tokens_details", "input_tokens_details",
                                         "completion_tokens_details", "output_tokens_details"};
   long ignored = 0;
   for (size_t i = 0; i < sizeof totals / sizeof *totals; i++)
      if (aimee_ir_read_usage(usage, totals[i], &ignored))
         return -1;
   for (size_t i = 0; i < sizeof details / sizeof *details; i++)
      if (aimee_ir_read_usage(cJSON_GetObjectItemCaseSensitive(usage, details[i]),
                              i < 2 ? "cached_tokens" : "reasoning_tokens", &ignored))
         return -1;
   return 0;
}
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
