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

/* As token_estimate_cost, but also reports whether the model is PRICED (known) —
 * set *priced to 1 when the static table lists it (a 0 there is an intentional
 * free price) or the registry returns a nonzero price, else 0. This disambiguates
 * a genuinely free/zero-priced model (cost 0, priced=1) from an unknown one
 * (cost 0, priced=0), so callers (delegate economics) do not flat-rate a free
 * model as unknown spend. *priced may be NULL. */
double token_estimate_cost_ex(const char *model, const token_usage_t *usage, int *priced);

/* Resolve the billable model for the audit/cost key by precedence (ingress
 * cost-accounting §2a): provider-reported model (most specific, echoed in the
 * response) > served model (what aimee actually routed to) > requested model
 * (what the client asked for). Agent names are NEVER pricing keys. Returns one of
 * the inputs verbatim, or "" when none is set (the by-model breakdown maps "" to
 * "(unattributed)" rather than mis-pricing it). Inputs may be NULL. */
const char *token_billable_model(const char *provider_model, const char *served_model,
                                 const char *requested_model);

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

/* Cost-shaped reward for the delegate-routing bandit. Combines task success with
 * a spend penalty so the bandit prefers cheaper arms when quality is comparable:
 *   reward = clamp01(success - (lambda_pct/100) * min(cost_usd / cost_ref, 1))
 * where cost_ref = ref_usd_milli / 1000 (milli-USD). A failed call (success=0)
 * always scores 0; a free success scores 1; an expensive success is discounted
 * by at most lambda_pct. Pure/deterministic so it is unit-tested directly. */
double cost_shaped_reward(int success, double cost_usd, int lambda_pct, int ref_usd_milli);

#endif /* DEC_TOKEN_TRACKER_H */
