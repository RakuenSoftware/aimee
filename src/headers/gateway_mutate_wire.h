/* gateway_mutate_wire.h: the buffered request-path ORCHESTRATION for economizer
 * gateway mutation (proposal §2.5, buffered). Sits above the pure decision helpers
 * (gateway_mutate.h): resolves the per-session key from the thread-local request
 * identity, honors the per-session circuit breaker, snapshots + reduces + replaces
 * the messages array under the default-OFF reduce_gateway_mutate flag, and after the
 * upstream status handles the 4xx-restore-resend / 5xx-disable contract. Shared by
 * the Anthropic (/v1/messages) and OpenAI (/v1/responses) buffered paths. Provider-
 * agnostic: it operates on a cJSON `container` and the message-array `key` within
 * it, so restore/replace touch exactly the array the provider body is built from. */
#ifndef DEC_GATEWAY_MUTATE_WIRE_H
#define DEC_GATEWAY_MUTATE_WIRE_H 1

#include "economizer.h"
#include "msg_session_disable.h"
#include <cJSON.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Per-request mutation state, carried from the pre-send attempt to the post-send
    * status handling. Zero-init via gw_mutate_ctx_init; free via gw_mutate_ctx_free. */
   typedef struct
   {
      int mutate_on; /* the feature flag was enabled for this request */
      int have_key;  /* a per-identity session key was resolvable */
      int mutated;   /* the reduced payload was actually installed + dispatched */
      char skey[MSG_SESSION_KEY_LEN];
      cJSON *pristine;   /* owned deep copy of the original array (NULL once restored/freed) */
      reduce_state_t st; /* provenance marker owner */
      int ttl_ms;        /* disable-window TTL resolved from config */
   } gw_mutate_ctx_t;

   void gw_mutate_ctx_init(gw_mutate_ctx_t *ctx);
   void gw_mutate_ctx_free(gw_mutate_ctx_t *ctx);

   /* Cheap (mtime-cached config_load) check of reduce_gateway_mutate, so a caller can
    * skip the system-prompt flattening + the mutate attempt entirely on the default-
    * OFF hot path. gw_buffered_mutate re-checks internally (defense in depth). */
   int gw_mutate_is_enabled(void);

   /* Pre-send: under reduce_gateway_mutate, resolve the session key from the thread-
    * local request identity; if the session is not disabled, snapshot container[key],
    * run the compress-only economizer, and — when gw_should_apply passes — replace
    * container[key] with the reduced array (marking provenance) so the provider body
    * is built from it. On any bypass/disable/no-key the container is left byte-intact.
    * Records the mutate/hard_bypass/session-blocked counters. No-op (dark) when the
    * flag is off. `system_prompt` may be NULL. The identity inputs (`session_hdr`,
    * `bearer`, `auth_identity`; each may be NULL/"") are passed in by the caller
    * (from the thread-local request identity) so this stays unit-testable. */
   void gw_buffered_mutate(cJSON *container, const char *key, const char *model,
                           const char *system_prompt, const char *session_hdr, const char *bearer,
                           const char *auth_identity, gw_mutate_ctx_t *ctx);

   /* Post-send action for a MUTATED request, from the upstream status. */
   typedef enum
   {
      GW_POST_NONE = 0, /* forward the response as-is (not mutated, or 2xx/3xx) */
      GW_POST_RESEND, /* the caller must rebuild the body from the restored container + resend once
                       */
   } gw_post_action_t;

   /* Post-send: classify the upstream `http_status` for a mutated request. On ANY 4xx
    * (400/413/422/…): restore the pristine array into container[key], run
    * message_history_repair on it, disable the session, clear provenance, and return
    * GW_POST_RESEND (the caller rebuilds the provider body from the now-restored
    * container and resends ONCE). On 5xx: disable the session, clear provenance, and
    * return GW_POST_NONE (no resend — forward the 5xx as the provider returned it).
    * For a non-mutated request or a 2xx/3xx: GW_POST_NONE, no state change. Idempotent
    * to a second call (pristine is consumed on restore). */
   gw_post_action_t gw_buffered_after_status(cJSON *container, const char *key, int http_status,
                                             gw_mutate_ctx_t *ctx);

   /* Streaming disposition (§2.5 streaming): a mutated stream CANNOT restore/resend —
    * the HTTP 200 is committed to the client before the upstream status is known, so
    * only SUBSEQUENT turns are protected. gw_stream_disable circuit-breaks the session
    * for subsequent turns (records gateway_stream_error_disable + the reason), clears
    * provenance, and is idempotent within a turn (it flips ctx->mutated off so a second
    * call no-ops). No-op for a non-mutated / keyless request. `reason` is a stable
    * static label ("stream_invalid_request" | "stream_decoder_error"). */
   void gw_stream_disable(gw_mutate_ctx_t *ctx, const char *reason);

   /* Classify an Anthropic SSE error-frame `data` (the JSON body of an `event: error`
    * frame). Returns 1 for an invalid-request-class error (invalid_request_error /
    * request_too_large — a reduced-payload bug => disable), 0 for rate-limit /
    * overloaded / api_error / auth frames (transient or unrelated => forward WITHOUT
    * disabling, so the breaker is not false-tripped). NULL/garbage-safe. */
   int gw_stream_anthropic_error_is_invalid_request(const char *data);

   /* Whether an upstream HTTP status is the invalid-request class a bad reduced
    * payload can produce (400/413/422). Used by the buffered-replay streaming path to
    * disable ONLY on a payload-class 4xx — never on 401/403/404/429 (auth/rate-limit,
    * which the streaming contract forwards without disabling). */
   int gw_status_is_invalid_request(int http_status);

#ifdef __cplusplus
}
#endif

#endif /* DEC_GATEWAY_MUTATE_WIRE_H */
