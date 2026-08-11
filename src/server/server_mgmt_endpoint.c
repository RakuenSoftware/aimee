#include "server_mgmt_endpoint.h"
#include "cJSON.h"
#include "json_wire.h"
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

static int lower_hex_sha256(const char *bytes, size_t n, char out[65])
{
   unsigned char digest[SHA256_DIGEST_LENGTH];
   if (!bytes || !out || !SHA256((const unsigned char *)bytes, n, digest))
      return -1;
   for (size_t i = 0; i < sizeof(digest); i++)
      snprintf(out + i * 2, 3, "%02x", digest[i]);
   return 0;
}

int server_mgmt_action_parse(const char *body, size_t body_len, server_mgmt_action_t *out)
{
   if (!body || !out || body_len < 2 || body_len > SERVER_MGMT_ACTION_BODY_MAX ||
       memchr(body, '\0', body_len) || json_wire_has_nul_escape(body, body_len))
      return -1;
   memset(out, 0, sizeof(*out));
   const char *end = NULL;
   cJSON *root = cJSON_ParseWithLengthOpts(body, body_len, &end, 0);
   if (!root || !cJSON_IsObject(root) || end != body + body_len)
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON *action = NULL, *agent = NULL;
   int fields = 0;
   for (cJSON *it = root->child; it; it = it->next)
   {
      fields++;
      if (!it->string || !strcmp(it->string, "action"))
      {
         if (action)
            fields = 99;
         action = it;
      }
      else if (!strcmp(it->string, "agent"))
      {
         if (agent)
            fields = 99;
         agent = it;
      }
      else
         fields = 99;
   }
   int ok = fields == 2 && cJSON_IsString(action) && cJSON_IsString(agent) && action->valuestring &&
            agent->valuestring &&
            /* INGRESS contract: an external control plane composes these action
             * strings, so both spellings are accepted rather than renamed. The
             * roster ops are `model.*` now, but a caller still emitting the
             * pre-rename `agent.*` must keep working -- it is not redeployed in
             * lockstep with this server. The value is echoed verbatim into the
             * canonical form that gets digested, so neither is rewritten. */
            (!strcmp(action->valuestring, "model.enable") ||
             !strcmp(action->valuestring, "model.disable") ||
             !strcmp(action->valuestring, "agent.enable") ||
             !strcmp(action->valuestring, "agent.disable"));
   size_t agent_len = ok ? strnlen(agent->valuestring, sizeof(out->agent)) : 0;
   if (!agent_len || agent_len >= sizeof(out->agent))
      ok = 0;
   for (size_t i = 0; ok && i < agent_len; i++)
   {
      unsigned char c = (unsigned char)agent->valuestring[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-'))
         ok = 0;
   }
   if (ok)
   {
      snprintf(out->action, sizeof(out->action), "%s", action->valuestring);
      snprintf(out->agent, sizeof(out->agent), "%s", agent->valuestring);
      int n = snprintf(out->canonical, sizeof(out->canonical),
                       "{\"action\":\"%s\",\"agent\":\"%s\"}", out->action, out->agent);
      if (n < 0 || (size_t)n >= sizeof(out->canonical))
         ok = 0;
      else
      {
         out->canonical_len = (size_t)n;
         ok = lower_hex_sha256(out->canonical, out->canonical_len, out->digest) == 0;
      }
   }
   cJSON_Delete(root);
   if (!ok)
      memset(out, 0, sizeof(*out));
   return ok ? 0 : -1;
}

static int finish(server_mgmt_endpoint_result_t *out, int status, const char *result,
                  const char *effect)
{
   out->status = status;
   out->result = result;
   out->effect = effect;
   return status;
}

int server_mgmt_endpoint_dispatch(const server_mgmt_endpoint_request_t *rq,
                                  const server_mgmt_endpoint_deps_t *d,
                                  server_mgmt_endpoint_result_t *out)
{
   server_mgmt_action_t action;
   server_mgmt_token_claims_t claims;
   uint64_t generation = 0;
   char staple_digest[65] = {0};
   if (!out)
      return 500;
   if (!rq || !d || !d->verify_token || !d->verify_and_consume_staple || !d->verify_checkpoint ||
       !d->consume_jti || !d->remote_writes || !d->audit || !d->apply || !rq->peer ||
       !rq->server_id || !rq->expected_issuer || !rq->local_fingerprint ||
       !rq->peer->management_profile || strcmp(rq->peer->cn, "p5-kb-management") || !rq->jwt ||
       !rq->jwt_len || !rq->staple || !rq->staple_len ||
       server_mgmt_action_parse(rq->body, rq->body_len, &action) != 0)
      return finish(out, 403, "denied", "none");
   memset(&claims, 0, sizeof(claims));
   if (d->verify_token(d->ctx, rq, action.digest, &claims) != 0 ||
       strcmp(claims.capability, "remote_writes") != 0)
      return finish(out, 403, "denied", "none");
   if (d->verify_and_consume_staple(d->ctx, rq, &generation, staple_digest) != 0)
      return finish(out, 403, "denied", "none");
   server_mgmt_checkpoint_result_t checkpoint =
       d->verify_checkpoint(d->ctx, rq, &claims, generation, staple_digest);
   if (checkpoint == SERVER_MGMT_CHECKPOINT_DENIED)
      return finish(out, 403, "denied", "none");
   if (checkpoint != SERVER_MGMT_CHECKPOINT_OK)
      return finish(out, 500, "failed", "none");
   /* Deliberate barrier: successful checkpoint verification is followed by the
    * durable consume directly. No logging, network, or policy lookup belongs here. */
   server_mgmt_endpoint_jti_result_t jti = d->consume_jti(d->ctx, rq, &claims);
   if (jti == SERVER_MGMT_JTI_REPLAY)
      return finish(out, 403, "denied", "none");
   if (jti != SERVER_MGMT_JTI_OK)
      return finish(out, 500, "failed", "none");
   if (d->remote_writes(d->ctx) != 2)
      return finish(out, 403, "denied", "none");
   if (d->audit(d->ctx, &claims, &action, 0, 0) != 0)
      return finish(out, 500, "failed", "none");
   int effect = d->apply(d->ctx, &action);
   if (effect < 0 || effect > 2)
      effect = 2;
   if (d->audit(d->ctx, &claims, &action, 1, effect == 0 ? 0 : (effect == 1 ? -1 : -2)) != 0)
      return finish(out, effect == 1 ? 500 : 502, effect == 1 ? "failed" : "indeterminate",
                    effect == 1 ? "none" : "unknown");
   return effect == 0   ? finish(out, 200, "succeeded", "applied")
          : effect == 1 ? finish(out, 500, "failed", "none")
                        : finish(out, 502, "indeterminate", "unknown");
}

int server_mgmt_endpoint_render(const server_mgmt_endpoint_result_t *r, char *out, size_t cap)
{
   if (!r || !r->result || !r->effect || !out || !cap)
      return -1;
   int n = snprintf(out, cap, "{\"result\":\"%s\",\"effect\":\"%s\"}", r->result, r->effect);
   return n >= 0 && (size_t)n < cap ? n : -1;
}
