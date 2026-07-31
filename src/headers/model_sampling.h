/* model_sampling.h: opt-in per-model sampling defaults for delegates */
#ifndef DEC_MODEL_SAMPLING_H
#define DEC_MODEL_SAMPLING_H 1

#include "aimee.h"
#include "agent_types.h"

struct cJSON;

typedef struct
{
   const char *model_key;
   double temperature;    /* -1 = omit */
   double top_p;          /* -1 = omit */
   int top_k;             /* -1 = omit */
   double min_p;          /* -1 = omit */
   double repeat_penalty; /* -1 = omit */
   const char *source_url;
} model_sampling_row_t;

int model_sampling_get(const char *model_key, model_sampling_row_t *out);

/* The ONE temperature a model will accept, or -1 when it accepts a range.
 *
 * Distinct from model_provider_t.fixed_temperature, which is a FALLBACK used
 * only when nobody supplied a temperature. This is a constraint of the model
 * itself: sending anything else is a hard 4xx, so it must override the caller,
 * the sampling row, and the provider fallback alike. Keyed on the agent (not a
 * bare model string) because the constraint is vendor-scoped — it takes the
 * catalog vendor to tell moonshotai's "k3" from anyone else's. */
double model_sampling_required_temperature(const agent_t *agent);

void model_sampling_apply_openai(const agent_t *agent, struct cJSON *req,
                                 double caller_temperature);
void model_sampling_apply_anthropic(const agent_t *agent, struct cJSON *req,
                                    double caller_temperature);

#endif /* DEC_MODEL_SAMPLING_H */
