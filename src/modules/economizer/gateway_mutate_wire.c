/* gateway_mutate_wire.c: see gateway_mutate_wire.h. Buffered orchestration over the
 * pure gateway_mutate.h helpers + the session breaker + telemetry. */
#include "aimee.h"
#include "gateway_mutate_wire.h"

#include "economizer_module_client.h"

#include <string.h>

#include "agent_protocol.h" /* message_history_repair */
#include "config.h"
#include "gateway_mutate.h"
#include "gw_mutate_stats.h"

int gw_economizer_measure(cJSON *messages, const char *system_prompt, const char *model,
                          int retained_msgs, gw_reduce_report_t *out)
{
   if (!out)
      return 1;
   memset(out, 0, sizeof(*out));
   if (!cJSON_IsArray(messages))
      return 1; /* nothing to measure; out is zeroed so the caller's free is safe */

   /* Shadow mode: the module measures and never mutates. Only the ledger fields
    * the callers read are populated; a failed call leaves them zero, which reads
    * as "no opportunity" rather than a fabricated one. */
   econ_module_request_t mreq;
   memset(&mreq, 0, sizeof mreq);
   mreq.measure_only = 1;
   mreq.retained_msgs = retained_msgs;

   cJSON *ignored = NULL;
   econ_module_result_t mres;
   int rc = econ_module_reduce(messages, system_prompt, ECON_MODULE_SEAM_GATEWAY, &mreq, &ignored,
                               &mres);
   cJSON_Delete(ignored); /* measure_only never returns an array; defensive */
   if (rc == 0)
   {
      out->baseline_tokens = mres.baseline_tokens;
      out->reduced_tokens = mres.reduced_tokens;
      out->removed_tokens = mres.removed_tokens;
      out->foldable_tokens = mres.foldable_tokens;
      out->reason = GW_REDUCE_REASON_MEASURED;
   }
   econ_module_result_free(&mres);
   return rc;
}

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
   /* aggressive tier (P3): live-primary mutation needs enabled && aggressive && the lever. */
   return econ_gateway_mutate_on_current();
}

int gw_mutate_upstream_ok(int upstream_is_anthropic)
{
   /* Gateway wire-mutation is OpenAI-only by policy. An Anthropic upstream serves a
    * byte-verbatim, prompt-cached passthrough (build_anthropic_parity_headers); mutating
    * the wire there would bust the cached prefix AND force the request off the parity
    * passthrough. Reducing that upstream cache-coherently is the pre-economize seam's job
    * (delegate seam / tool_condense), not the gateway's. So the live mutator engages only
    * when the serving upstream is NOT Anthropic. `upstream_is_anthropic` is the caller's
    * driver_is_anthropic()/parity signal. */
   return !upstream_is_anthropic && gw_mutate_is_enabled();
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

   if (!econ_gateway_mutate_on_current())
      return; /* dark unless the economizer tier is aggressive (OpenAI-family egress only) */
   econ_preset_t ep;
   econ_preset_current(&ep);
   ctx->mutate_on = 1;
   ctx->ttl_ms = ep.gateway_session_disable_ttl_ms;

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

   /* The reduction itself lives in the Go economizer module now; this seam
    * resolves config and owns the pristine/restore contract. Compress-only at the
    * gateway: there is no per-conversation state here to hold a freeze boundary,
    * so the fold has nothing to freeze. */
   econ_module_request_t mreq;
   memset(&mreq, 0, sizeof mreq);
   mreq.compress = 1;
   mreq.retained_msgs = config_fold_retained_msgs();

   cJSON *reduced = NULL;
   econ_module_result_t mres;
   int rrc =
       econ_module_reduce(msgs, system_prompt, ECON_MODULE_SEAM_GATEWAY, &mreq, &reduced, &mres);

   /* Reuse the pure decision helper by handing it the module's ledger. An
    * unreachable module is a no-op, not an internal error: the request is
    * pristine and forwarding it is correct. */
   gw_reduce_report_t res;
   memset(&res, 0, sizeof(res));
   res.messages = reduced;
   res.mutated = mres.mutated;
   res.reason = mres.mutated ? GW_REDUCE_REASON_REDUCED : GW_REDUCE_REASON_NONE;
   res.baseline_tokens = mres.baseline_tokens;
   res.reduced_tokens = mres.reduced_tokens;

   gw_bypass_reason_t bypass = gw_should_apply(rrc == 0 ? 0 : 1, &res);
   if (bypass != GW_BYPASS_NONE)
   {
      gw_stat_inc_reason("hard_bypass", gw_bypass_reason_str(bypass));
      gw_provenance_clear(&ctx->st);
      cJSON_Delete(reduced);
      econ_module_result_free(&mres);
      /* keep pristine: not mutated, but harmless; freed in ctx_free */
      return;
   }

   if (gw_replace_messages(container, key, reduced) != 0)
   {
      gw_stat_inc_reason("hard_bypass", "replace_failed");
      gw_provenance_clear(&ctx->st);
      cJSON_Delete(reduced);
      econ_module_result_free(&mres);
      return;
   }
   int baseline_tok = mres.baseline_tokens;
   int reduced_tok = mres.reduced_tokens;
   econ_module_result_free(&mres);

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
