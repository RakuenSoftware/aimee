/* ir_ingress_stubs.c -- WEAK no-op stubs for the IR/router hooks wired into
 * anthropic_http.c / openai_chat.c. The minimal-link ingress tests (#include the
 * ingress .c to exercise tool-policing / memory / SSE) don't test the IR path, so
 * they don't need the real (heavy) router_advise + IR chain linked. These stubs are
 * WEAK: if a test does link the real objects, the strong definitions win. */
#include <stddef.h>
#include <string.h>

#include "gateway_mutate_wire.h" /* gw_mutate_ctx_t + gw_post_action_t (header-only deps) */

/* Gateway-mutation hooks wired into anthropic_http.c / openai_chat.c (§ economizer
 * gateway mutation). The minimal-link ingress tests exercise the non-mutation shape
 * behavior, so these weak stubs make the mutation path inert (is_enabled -> 0, so the
 * real block is skipped; init/mutate zero the ctx so the streaming relay's
 * gwmc->mutated read is well-defined; after_status -> no resend). Real objects win. */
__attribute__((weak)) void gw_mutate_ctx_init(gw_mutate_ctx_t *ctx)
{
   if (ctx)
      memset(ctx, 0, sizeof(*ctx));
}
__attribute__((weak)) void gw_mutate_ctx_free(gw_mutate_ctx_t *ctx)
{
   (void)ctx;
}
__attribute__((weak)) int gw_mutate_is_enabled(void)
{
   return 0;
}
__attribute__((weak)) void gw_buffered_mutate(cJSON *container, const char *key, const char *model,
                                              const char *system_prompt, const char *session_hdr,
                                              const char *bearer, const char *auth_identity,
                                              gw_mutate_ctx_t *ctx)
{
   (void)container;
   (void)key;
   (void)model;
   (void)system_prompt;
   (void)session_hdr;
   (void)bearer;
   (void)auth_identity;
   if (ctx)
      memset(ctx, 0, sizeof(*ctx));
}
__attribute__((weak)) gw_post_action_t gw_buffered_after_status(cJSON *container, const char *key,
                                                                int http_status,
                                                                gw_mutate_ctx_t *ctx)
{
   (void)container;
   (void)key;
   (void)http_status;
   (void)ctx;
   return GW_POST_NONE;
}
__attribute__((weak)) void gw_stream_disable(gw_mutate_ctx_t *ctx, const char *reason)
{
   (void)ctx;
   (void)reason;
}
__attribute__((weak)) int gw_stream_anthropic_error_is_invalid_request(const char *data)
{
   (void)data;
   return 0;
}
__attribute__((weak)) int gw_status_is_invalid_request(int http_status)
{
   (void)http_status;
   return 0;
}
__attribute__((weak)) const char *server_http_identity_session_hdr(void)
{
   return "";
}
__attribute__((weak)) const char *server_http_identity_bearer(void)
{
   return "";
}
__attribute__((weak)) const char *server_http_identity_principal(void)
{
   return "";
}

__attribute__((weak)) int gw_stage_router(void *r, void *ud)
{
   (void)r;
   (void)ud;
   return 0;
}

__attribute__((weak)) void aimee_ir_shadow_observe_request(const void *req, int frontend)
{
   (void)req;
   (void)frontend;
}

/* Slice 2-wire: the response-side shadow. anthropic_http.o now calls this on the
 * live buffered paths; the minimal shape/p2c test links do not pull the real
 * aimee_ir_shadow.o (+ backend parsers), so a weak inert stub resolves the link.
 * Real aimee_ir_shadow.o wins when linked. */
__attribute__((weak)) void aimee_ir_shadow_compare_response(const void *legacy,
                                                            const void *resp_json, int wire)
{
   (void)legacy;
   (void)resp_json;
   (void)wire;
}

__attribute__((weak)) int aimee_ir_path_enabled(void)
{
   return 0;
}

/* Slice 5-wire: the incremental IR-delta relay. The flag stub returns 0 (legacy
 * relay), so the delta helpers below are never CALLED by the ingress tests -- they
 * exist only to resolve the link. Real definitions (aimee_ir_serve/aimee_ir_stream)
 * win when linked. */
__attribute__((weak)) int aimee_ir_stream_relay_enabled(void)
{
   return 0;
}

__attribute__((weak)) void openai_stream_state_init(void *st)
{
   (void)st;
}

__attribute__((weak)) int openai_chunk_to_deltas(const void *chunk, void *st, void *out, int max)
{
   (void)chunk;
   (void)st;
   (void)out;
   (void)max;
   return 0;
}

__attribute__((weak)) int anthropic_delta_emit(const void *d, void *st, const char *msg_id,
                                               const char *model, void *emit, void *ctx)
{
   (void)d;
   (void)st;
   (void)msg_id;
   (void)model;
   (void)emit;
   (void)ctx;
   return 0;
}

__attribute__((weak)) char *aimee_ir_build_provider_body(const void *req, const char *driver_name,
                                                         const char *agent_model,
                                                         int max_tokens_override, int want_stream)
{
   (void)req;
   (void)driver_name;
   (void)agent_model;
   (void)max_tokens_override;
   (void)want_stream;
   return NULL;
}

/* IR observability counters. The ingress increments these on the legacy-fallback and
 * shadow-compare paths; the minimal-link tests don't link the metrics table, so the
 * counter is inert here. Real aimee_ir_metrics.o wins when linked. */
__attribute__((weak)) void aimee_ir_metric_inc(int metric, int frontend)
{
   (void)metric;
   (void)frontend;
}

__attribute__((weak)) int aimee_ir_responses_to_chat(const char *body, char *model, size_t model_n,
                                                     char **instructions_out, void **messages_out,
                                                     void **tools_out, int *stream_out)
{
   (void)body;
   (void)model;
   (void)model_n;
   (void)instructions_out;
   (void)messages_out;
   (void)tools_out;
   (void)stream_out;
   return -1;
}

__attribute__((weak)) void *aimee_ir_build_from_chat(const char *agent_model, const void *messages,
                                                     const void *tools, const char *system,
                                                     const char *driver_name)
{
   (void)agent_model;
   (void)messages;
   (void)tools;
   (void)system;
   (void)driver_name;
   return NULL;
}

/* Slice 3: the OPENAI-WIRE IR response path. The anthropic_http shape/p2c tests
 * #include anthropic_http.c and link minimally; these inert weak stubs resolve the
 * link. aimee_ir_resp_path_enabled()->0 keeps the legacy parse on the test path, so
 * the other three are never actually called. Real objects win when linked. */
__attribute__((weak)) int aimee_ir_resp_path_enabled(void)
{
   return 0;
}
__attribute__((weak)) void aimee_ir_response_to_parsed(const void *r, void *out)
{
   (void)r;
   (void)out;
}
__attribute__((weak)) int openai_backend_parse(const void *resp, void *out, char *err, unsigned long errn)
{
   (void)resp;
   (void)out;
   (void)err;
   (void)errn;
   return -1;
}
__attribute__((weak)) void aimee_response_free(void *r)
{
   (void)r;
}
