/* agent_protocol.h: LLM request building and response parsing */
#ifndef DEC_AGENT_PROTOCOL_H
#define DEC_AGENT_PROTOCOL_H 1

#include "agent_types.h"
#include <aimee/ir/aimee_ir.h>

struct cJSON;

/* --- Request builders --- */

/* Prepare the complete owned host request, after profile shaping and delegate
 * policy, without provider rendering or projection events. Inputs are borrowed;
 * out must be zero-initialized or an owned request. Success atomically replaces
 * it; failure leaves it unchanged. Native producers consume this typed context
 * rather than recover host authority from SDK-generated native requests. */
int agent_prepare_request_ir(const agent_t *agent, const struct cJSON *messages,
                             const aimee_request_t *catalog, const char *system_prompt,
                             int max_tokens, double temperature, aimee_wire_t wire, int is_delegate,
                             aimee_request_t *out);

/* The runtime stores canonical {role,blocks} messages. Native history is accepted
 * only by this migration adapter, and native requests are built only at egress.
 * Optional dispatch_catalog receives the owned, post-policy function catalog on
 * success, replacing its previous owned value. It never contains conversation
 * content. Pass NULL at output-only boundaries. */
struct cJSON *agent_build_request_ir(const agent_t *agent, const struct cJSON *messages,
                                     const aimee_request_t *catalog, const char *system_prompt,
                                     int max_tokens, double temperature, aimee_wire_t wire,
                                     int is_delegate, aimee_request_t *dispatch_catalog);

/* Resolve the output-token cap for an outbound request: an explicit caller
 * `requested` (>0) wins, else the agent's pinned max_tokens (>0), else the
 * model's registry ceiling (model_max_output). Never returns 0 — request
 * builders emit a concrete, model-appropriate cap rather than a hardcoded one. */
int agent_request_max_tokens(const agent_t *agent, int requested);

/* --- Response parsers --- */

/* Extract the Responses (codex) response object (the one with the `output` array)
 * from an SSE body, so the IR responses_backend_parse can consume it. Returns a new
 * cJSON the caller owns, or NULL. */
struct cJSON *agent_responses_sse_response_object(const char *body);

/* Native response entry adapters. The caller owns the full typed response until
 * tool dispatch and history append finish. No text/tool projection occurs here.
 * rescue_mode < 0 skips rescue, 0 admits dialect calls, 1 also admits bare JSON;
 * n_rescued receives the admitted recovered-call count when provided. */
#define AGENT_IR_CAPTURE_FAILED (-2)
struct aimee_anthropic_replay;
int agent_ir_decode_json_response_bound(const struct cJSON *root, int anthropic, int rescue_mode,
                                        int *n_rescued,
                                        const struct aimee_anthropic_replay *request,
                                        aimee_response_t *out);
int agent_ir_decode_json_response(const struct cJSON *root, int anthropic, int rescue_mode,
                                  int *n_rescued, aimee_response_t *out);
int agent_ir_decode_responses(const char *body, int rescue_mode, int *n_rescued,
                              aimee_response_t *out);

/* Legacy output-only compatibility values are released here while their
 * remaining native emitters are retired. Execution never constructs them. */

/* --- Message utilities --- */

/* Repair a canonical working-history view. Missing results become explicit
 * unknown-outcome diagnostics; orphaned results are removed in causal order.
 * Native/mixed/malformed history refuses atomically; import belongs at translation.
 * This never admits a repaired view as authoritative persisted history.
 * Returns repairs, or -1 on refusal. NULL/empty input performs no repairs. */
int message_history_repair(struct cJSON *messages);

#endif /* DEC_AGENT_PROTOCOL_H */
