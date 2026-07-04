/* gateway_mutate_wire.c: see gateway_mutate_wire.h. Buffered orchestration over the
 * pure gateway_mutate.h helpers + the session breaker + telemetry. */
#include "aimee.h"
#include "gateway_mutate_wire.h"

#include <string.h>

#include "agent_protocol.h" /* message_history_repair */
#include "config.h"
#include "gateway_mutate.h"
#include "gw_mutate_stats.h"

void gw_mutate_ctx_init(gw_mutate_ctx_t *ctx)
{
   if (ctx)
      memset(ctx, 0, sizeof(*ctx));
}

void gw_mutate_ctx_free(gw_mutate_ctx_t *ctx)
{
   if (!ctx)
      return;
   if (ctx->pristine)
   {
      cJSON_Delete(ctx->pristine);
      ctx->pristine = NULL;
   }
}

int gw_mutate_is_enabled(void)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return 0;
   return cfg.reduce_gateway_mutate ? 1 : 0;
}

void gw_buffered_mutate(cJSON *container, const char *key, const char *model,
                        const char *system_prompt, const char *session_hdr, const char *bearer,
                        const char *auth_identity, gw_mutate_ctx_t *ctx)
{
   if (!ctx)
      return;
   gw_mutate_ctx_init(ctx);
   if (!container || !key)
      return;

   config_t cfg;
   if (config_load(&cfg) != 0)
      return;
   if (!cfg.reduce_gateway_mutate)
      return; /* dark by default */
   ctx->mutate_on = 1;
   ctx->ttl_ms = cfg.reduce_gateway_session_disable_ttl_ms;

   /* Resolve a per-identity session key; an identity-less request is a pristine
    * passthrough with NO disable state written (§2.4). */
   msg_session_key_status_t ks =
       msg_session_key_resolve(session_hdr, bearer, auth_identity, ctx->skey);
   if (ks == MSG_SESSION_KEY_NONE)
      return;
   ctx->have_key = 1;

   /* Honor the circuit breaker: a disabled session is a pristine passthrough. */
   if (msg_session_is_disabled(ctx->skey))
   {
      gw_stat_inc(GW_STAT_SESSION_DISABLED_BLOCKS);
      return;
   }

   cJSON *msgs = cJSON_GetObjectItemCaseSensitive(container, key);
   if (!cJSON_IsArray(msgs))
      return;

   gw_stat_inc(GW_STAT_MUTATE_ATTEMPTED);

   /* Snapshot FIRST: never send a reduced payload we cannot restore. */
   ctx->pristine = gw_snapshot_messages(msgs);
   if (!ctx->pristine)
   {
      gw_stat_inc_reason("hard_bypass", "snapshot_oom");
      return;
   }

   reduce_config_t rc;
   memset(&rc, 0, sizeof(rc));
   rc.gateway_seam = 1;
   rc.compress = 1; /* D3: compress-only at the gateway in v1 (fold deferred) */
   rc.measure_only = 0;
   rc.fold.retained_msgs = cfg.fold_retained_msgs;

   reduce_result_t res;
   memset(&res, 0, sizeof(res));
   int rrc = context_reduce(msgs, system_prompt, model, NULL, REDUCE_SEAM_GATEWAY, &rc, NULL, &res);

   gw_bypass_reason_t bypass = gw_should_apply(rrc, &res);
   if (bypass != GW_BYPASS_NONE)
   {
      gw_stat_inc_reason("hard_bypass", gw_bypass_reason_str(bypass));
      gw_provenance_clear(&ctx->st);
      context_reduce_result_free(&res);
      /* keep pristine: not mutated, but harmless; freed in ctx_free */
      return;
   }

   /* Apply: install the reduced array (ownership transfers on success). */
   if (gw_replace_messages(container, key, res.messages) != 0)
   {
      gw_stat_inc_reason("hard_bypass", "replace_failed");
      gw_provenance_clear(&ctx->st);
      context_reduce_result_free(&res); /* res.messages still owned by res -> freed here */
      return;
   }
   res.messages = NULL;                    /* ownership moved into container */
   int baseline_tok = res.baseline_tokens; /* capture the token counts before freeing res */
   int reduced_tok = res.reduced_tokens;
   context_reduce_result_free(&res);

   gw_provenance_mark_reduced(&ctx->st); /* mark ONLY after replace succeeds */
   ctx->mutated = 1;
   gw_stat_inc(GW_STAT_MUTATE_APPLIED);
   gw_stat_record_token_delta(baseline_tok, reduced_tok); /* sampled §4 */
}

gw_post_action_t gw_buffered_after_status(cJSON *container, const char *key, int http_status,
                                          gw_mutate_ctx_t *ctx)
{
   if (!ctx || !ctx->mutated || !container || !key)
      return GW_POST_NONE;

   int cls = http_status / 100;
   if (cls == 4)
   {
      /* Restore the pristine original, repair defensively, disable the session for
       * subsequent turns, clear provenance, and signal a single resend. */
      if (ctx->pristine)
      {
         if (gw_replace_messages(container, key, ctx->pristine) == 0)
         {
            ctx->pristine = NULL; /* ownership moved back into container */
            cJSON *restored = cJSON_GetObjectItemCaseSensitive(container, key);
            if (restored)
               message_history_repair(restored);
         }
      }
      msg_session_disable(ctx->skey, ctx->ttl_ms, "4xx");
      gw_provenance_clear(&ctx->st);
      gw_stat_inc(GW_STAT_4XX_RESTORE_RESEND);
      ctx->mutated = 0; /* the request is now pristine; no double handling */
      return GW_POST_RESEND;
   }
   if (cls == 5)
   {
      /* Provider state is uncertain after a 5xx: disable, do NOT resend. */
      msg_session_disable(ctx->skey, ctx->ttl_ms, "5xx");
      gw_provenance_clear(&ctx->st);
      gw_stat_inc(GW_STAT_5XX_DISABLE);
      ctx->mutated = 0;
      return GW_POST_NONE;
   }
   return GW_POST_NONE;
}

void gw_stream_disable(gw_mutate_ctx_t *ctx, const char *reason)
{
   if (!ctx || !ctx->mutated || !ctx->have_key)
      return;
   msg_session_disable(ctx->skey, ctx->ttl_ms, reason ? reason : "stream");
   gw_provenance_clear(&ctx->st);
   gw_stat_inc(GW_STAT_STREAM_ERROR_DISABLE);
   ctx->mutated = 0; /* one disable per turn; a later frame no-ops */
}

int gw_stream_anthropic_error_is_invalid_request(const char *data)
{
   if (!data || !data[0])
      return 0;
   cJSON *root = cJSON_Parse(data);
   if (!root)
      return 0;
   int invalid = 0;
   cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
   cJSON *type = err ? cJSON_GetObjectItemCaseSensitive(err, "type") : NULL;
   if (cJSON_IsString(type) && type->valuestring)
   {
      const char *t = type->valuestring;
      /* Anthropic invalid-request class (error taxonomy as of 2024-2026):
       * invalid_request_error + request_too_large (the 413-equivalent a bad reduced
       * serialization can produce). EXACT match — not substring — so a future type
       * that merely contains these words does not false-trip. rate_limit_error /
       * overloaded_error / api_error / authentication_error are NOT reduction bugs. */
      if (strcmp(t, "invalid_request_error") == 0 || strcmp(t, "request_too_large") == 0)
         invalid = 1;
   }
   cJSON_Delete(root);
   return invalid;
}

int gw_status_is_invalid_request(int http_status)
{
   /* The 4xx codes a bad reduced serialization can produce: 400 invalid_request,
    * 413 request_too_large, 422 unprocessable. 401/403/404/429 are auth / rate-limit
    * / not-found — NOT reduction bugs, so a streaming path must not disable on them. */
   return http_status == 400 || http_status == 413 || http_status == 422;
}
