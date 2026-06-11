/* token_tracker.h: shared token usage normalisation and cost estimation */
#ifndef DEC_TOKEN_TRACKER_H
#define DEC_TOKEN_TRACKER_H 1

/* Per-request token usage, normalised across providers */
typedef struct
{
   int input_tokens;
   int output_tokens;
   int cache_write_tokens; /* Anthropic: cache_creation_input_tokens */
   int cache_read_tokens;  /* Anthropic: cache_read_input_tokens */
} token_usage_t;

/* Estimate cost in USD for a single request. The model is matched
 * (most-specific-first) against an internal pricing table; on a miss it falls
 * back to the model registry via token_tracker_registry_price() when that bridge
 * is linked. Returns 0.0 when no source prices the model. This is the single
 * cost authority shared by the agent audit and delegate economics paths. */
double token_estimate_cost(const char *model, const token_usage_t *usage);

/* Registry-price fallback hook. token_estimate_cost calls this (when installed)
 * on a static-table miss, so it covers registry-only providers (gemini, groq,
 * mistral) and models.dev / operator overrides. The hook is installed by a
 * constructor in token_tracker_registry.c, which is linked into the server and
 * the pricing tests; binaries that do not link it keep table-only pricing. This
 * indirection (a function pointer rather than a hard call) keeps token_tracker.o
 * free of a link dependency on the model registry for the many unrelated
 * binaries that link it. Sets base input/output $/MTok and returns 1 when priced.
 */
typedef int (*token_registry_price_fn)(const char *model, double *in_per_mtok,
                                       double *out_per_mtok);
void token_tracker_set_registry_price_fn(token_registry_price_fn fn);

#endif /* DEC_TOKEN_TRACKER_H */
