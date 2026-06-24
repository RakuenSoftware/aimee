/* gateway_pipeline.h: the gateway's per-call MEMORY stage (P3 of the universal
 * gateway). One home for context pre-injection across every ingress, so the
 * "<aimee-context> envelope" build + apply lives in a single named stage rather
 * than ad-hoc at each /v1 handler. Wraps the gated envelope builder
 * (ingress_preinject_build) and the apply shapes the ingresses need:
 *   - Anthropic /v1/messages: fold into the request's `system`.
 *   - OpenAI /v1/responses:    merge into the `instructions` string.
 *   - OpenAI completion paths:  envelope passed to agent_execute as system_prompt.
 * Behaviour is byte-identical to the prior inline sites and cache-safe (the
 * dedup key still hashes the same envelope bytes). Gating (config
 * ingress_preinject_enabled + the x-aimee-preinject:0 per-request disable) is
 * unchanged — inherited from ingress_preinject_build. */
#ifndef DEC_GATEWAY_PIPELINE_H
#define DEC_GATEWAY_PIPELINE_H 1

struct cJSON;

/* Build the <aimee-context> memory envelope for `query`. The single named entry
 * for the memory stage's build step (thin over ingress_preinject_build). Caller
 * owns the returned heap string; NULL when pre-injection is disabled or recall is
 * empty. */
char *gateway_pipeline_memory_envelope(const char *query);

/* Apply the memory envelope to an Anthropic-shape request's `system`, deriving
 * the query from `req`'s messages. Appends a trailing system text block (array
 * form) / joins (string form) / sets it (absent), preserving a cache_control'd
 * system prefix. Mutates `req` in place; must run before translate/build. No-op
 * when disabled or recall is empty. */
void gateway_pipeline_memory_apply_messages(struct cJSON *req);

/* Apply the memory envelope to an OpenAI /v1/responses `instructions` string,
 * deriving the query from `messages`. On injection, frees the old *instructions
 * and replaces it with the merged string; leaves it untouched when disabled or
 * empty. *instructions may be NULL/empty going in. */
void gateway_pipeline_memory_apply_instructions(char **instructions, const struct cJSON *messages);

#endif /* DEC_GATEWAY_PIPELINE_H */
