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
#include "log.h"
#include "token_tracker.h" /* token_estimate_cost_ex, for the freeze guardrail */

/* Declared rather than including db1.h, the same way agent_runtime.c does: this
 * seam needs exactly two calls, and pulling the whole db1 surface into an
 * economizer translation unit would widen the module's dependencies for nothing. */
int db1_economizer_state_save(const char *session_id, const char *json);
int db1_economizer_state_load(const char *session_id, char *out, size_t out_sz);

/* The gateway's reducer state is keyed by session key AND a conversation
 * fingerprint, in its own namespace.
 *
 * The namespace prefix keeps it clear of the delegate seam, which keys state by
 * conversation id: without it a gateway key that happened to equal a conversation
 * id would load and overwrite that conversation's freeze boundary.
 *
 * The fingerprint is the part that matters for correctness. msg_session_key_resolve
 * is deliberately per-IDENTITY -- "to group one identity's sessions under a stable
 * key" -- so one user with two concurrent conversations resolves to ONE key. Keyed
 * on that alone, the two would share a freeze boundary and thrash: each turn would
 * find the other's prefix digest, fail the match, and re-epoch, so the freeze would
 * never hold and folding would cost cache reads instead of saving them. Hashing the
 * first message separates conversations, and it is stable across their turns because
 * a conversation's opening message does not change. If a client rewrites its history
 * the fingerprint moves, which correctly reads as a different conversation. */
static uint64_t gw_fnv1a(const char *s)
{
   uint64_t h = 1469598103934665603ull;
   for (; s && *s; s++)
   {
      h ^= (unsigned char)*s;
      h *= 1099511628211ull;
   }
   return h;
}

static void gw_state_key(const char *skey, cJSON *msgs, char *out, size_t out_sz)
{
   uint64_t fp = 0;
   cJSON *first = msgs ? cJSON_GetArrayItem(msgs, 0) : NULL;
   if (first)
   {
      char *txt = cJSON_PrintUnformatted(first);
      if (txt)
      {
         fp = gw_fnv1a(txt);
         free(txt);
      }
   }
   snprintf(out, out_sz, "gw:%s:%016llx", skey ? skey : "", (unsigned long long)fp);
}

/* The module overwrites its loaded state's turn with the request's, so the seam
 * has to supply an increasing one. The gateway has no turn counter of its own,
 * so read it back out of the state we persisted last time. Turn drives only the
 * recall page-table TTL, so a restart resetting it to 0 costs recall freshness
 * for one session, never correctness. */
static int gw_state_next_turn(const char *state_json)
{
   if (!state_json || !state_json[0])
      return 0;
   cJSON *root = cJSON_Parse(state_json);
   if (!root)
      return 0;
   cJSON *turn = cJSON_GetObjectItemCaseSensitive(root, "turn");
   int next = cJSON_IsNumber(turn) ? turn->valueint + 1 : 0;
   cJSON_Delete(root);
   return next;
}

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
   /* ANTHROPIC MUTATES TOO, now that the gateway holds a freeze boundary.
    *
    * The old policy was OpenAI-only, because mutating an Anthropic wire "would bust
    * the cached prefix". That was true of a gateway with no state: every turn
    * re-folded from cold, so every turn moved the prefix and every turn missed the
    * cache. It is not true of one that persists a freeze per session. The fold moves
    * the prefix ONCE, when it first engages; from the next turn the frozen prefix is
    * byte-identical and the cache reads resume. One miss, bounded, in exchange for
    * every later turn carrying a folded history.
    *
    * Deferring to "the pre-economize seam" was not a real alternative either: that
    * seam is the delegate loop, which a proxied client never enters. An Anthropic
    * client through the gateway had NO reduction path at all.
    *
    * On the provider's cache breakpoints: the economizer still neither adds, removes,
    * nor moves one, and the live-surface guard greps this file to keep it that way --
    * strictly enough that naming the field here in prose trips it, which is the right
    * trade. Folding can retire a message that happened to carry a breakpoint, leaving
    * FEWER of them, and fewer is legal (Anthropic caps them at four rather than
    * requiring them). What it must never do is leave one attached to content that
    * moved, which is exactly what the freeze prevents. */
   (void)upstream_is_anthropic;
   return gw_mutate_is_enabled();
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
    * resolves config and owns the pristine/restore contract.
    *
    * THE GATEWAY FOLDS. It used to be compress-only, for the stated reason that
    * "there is no per-conversation state here to hold a freeze boundary" -- true
    * of the seam, but not of the deployment: the per-identity session key resolved
    * above is exactly such a handle, and the reducer state keyed by it survives
    * between requests in the same place the delegate seam keeps its own. Without
    * this the gateway could never fold for ANY provider, so the biggest context
    * consumer aimee has was reachable only through its own agent loop. */
   char state_blob[ECON_MODULE_STATE_MAX];
   state_blob[0] = '\0';
   char skey_ns[MSG_SESSION_KEY_LEN + 24];
   gw_state_key(ctx->skey, msgs, skey_ns, sizeof skey_ns);
   (void)db1_economizer_state_load(skey_ns, state_blob, sizeof state_blob);

   econ_module_request_t mreq;
   memset(&mreq, 0, sizeof mreq);
   mreq.compress = 1;
   mreq.history_fold = 1;
   mreq.retained_msgs = config_fold_retained_msgs();
   mreq.min_fold_msgs = config_fold_min_fold_msgs();
   mreq.excerpt_bytes = config_fold_excerpt_bytes();
   mreq.compact_head_bytes = config_compact_head_bytes();
   mreq.compact_tail_bytes = config_compact_tail_bytes();
   mreq.register_enabled = config_fold_register_enabled();
   mreq.closet_enabled = config_coord_closet_enabled();
   mreq.closet_budget_bytes = config_coord_closet_budget_bytes();
   mreq.closet_max_ratio_pct = config_coord_closet_max_ratio_pct();
   mreq.recall_enabled = config_fold_recall_enabled();
   mreq.recall_ttl_turns = config_fold_recall_ttl_turns();
   mreq.recall_inject = config_fold_recall_inject();
   mreq.state = state_blob[0] ? state_blob : NULL;
   mreq.turn = gw_state_next_turn(state_blob);

   char closet_denylist[CONFIG_COPY_MAX];
   config_coord_closet_denylist_copy(closet_denylist, sizeof(closet_denylist));
   mreq.closet_denylist = closet_denylist[0] ? closet_denylist : NULL;

   /* The freeze is what makes folding safe for a prefix-cached upstream, so it is
    * not optional here the way it is a tunable on the delegate seam. The module
    * does the arithmetic; this side supplies the per-tier rates. */
   mreq.freeze_guard_enabled = 1;
   mreq.freeze_guard_horizon = ep.freeze_guard_horizon;
   {
      token_usage_t w = {0}, in = {0}, rd = {0};
      w.cache_write_tokens = 1000;
      in.input_tokens = 1000;
      rd.cache_read_tokens = 1000;
      int priced = 0;
      double wc = token_estimate_cost_ex(model, &w, &priced);
      if (priced)
      {
         mreq.priced = 1;
         mreq.write_cost = wc;
         mreq.input_cost = token_estimate_cost_ex(model, &in, NULL);
         mreq.read_cost = token_estimate_cost_ex(model, &rd, NULL);
      }
   }

   cJSON *reduced = NULL;
   econ_module_result_t mres;
   int rrc =
       econ_module_reduce(msgs, system_prompt, ECON_MODULE_SEAM_GATEWAY, &mreq, &reduced, &mres);

   /* Persist BEFORE the apply decision, and regardless of it. The freeze boundary
    * has to advance on every turn the module saw, not only the ones that shipped a
    * reduced payload: drop it on a bypass and the next turn re-folds from cold,
    * which moves the prefix and costs the cache read the freeze exists to keep.
    * Mirrors the delegate seam, which persists on the same unconditional terms. */
   if (mres.state && mres.state[0])
      (void)db1_economizer_state_save(skey_ns, mres.state);

   /* One line per gateway reduction, because until now this seam was invisible:
    * its telemetry goes to the obs bus, nothing reached server.log, and proving
    * whether it had run at all meant inferring from token deltas. reused=1 is the
    * signal that actually matters -- it means the freeze held and the folded prefix
    * was byte-identical to last turn, which is the difference between folding that
    * pays for itself and folding that burns a cache read every turn. */
   if (rrc != 0)
      aimee_log(LOG_INFO, "economizer.gateway",
                "seam=gateway UNREACHED call_result=%d (1=capability_absent: nothing is "
                "serving the reduce stage) -- request dispatched pristine",
                mres.call_result);
   else
      aimee_log(LOG_INFO, "economizer.gateway",
                "seam=gateway mutated=%d reason=%s baseline=%d reduced=%d removed=%d "
                "folded=%d retained=%d reused=%d",
                mres.mutated, mres.reason[0] ? mres.reason : "none", mres.baseline_tokens,
                mres.reduced_tokens, mres.removed_tokens, mres.folded_msgs, mres.retained_msgs,
                mres.reused_boundary);

   /* The verdict is the module's: it holds the reduced array in-process, so it
    * is the only place the structural check can run without shipping that array
    * back across the bus to ask about it. The labels are the same ones
    * gw_bypass_reason_str emits, so the string goes straight to telemetry.
    *
    * A module that was not reached returns no verdict, and an absent verdict is
    * never read as consent: that is the reduce_internal_assertion path, exactly
    * what gw_should_apply returned for a non-zero rc before. The request is
    * pristine either way, so forwarding it is correct. */
   const char *bypass = gw_module_bypass(rrc, mres.bypass);
   if (strcmp(bypass, gw_bypass_reason_str(GW_BYPASS_NONE)) != 0)
   {
      gw_stat_inc_reason("hard_bypass", bypass);
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
