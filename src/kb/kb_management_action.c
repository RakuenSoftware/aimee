#include "kb_management_action.h"

#include "cJSON.h"
#include "json_wire.h"
#include "kb_mgmt_client.h"
#include <aimee/core/connection/auth.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

#define ACTION_PURPOSE "management.action.v1"

static int token(const char *s, size_t max)
{
   size_t n = s ? strnlen(s, max + 1) : 0;
   if (!n || n > max)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
            (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '_' || s[i] == '-'))
         return 0;
   return 1;
}

static void hex32(const uint8_t in[32], char out[65])
{
   static const char h[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; i++)
   {
      out[i * 2] = h[in[i] >> 4];
      out[i * 2 + 1] = h[in[i] & 15];
   }
   out[64] = 0;
}

static cJSON *parse_exact(const char *raw, size_t len, size_t max)
{
   if (!raw || len < 2 || len > max || memchr(raw, 0, len) || json_wire_has_nul_escape(raw, len) ||
       raw[0] != '{' || raw[len - 1] != '}')
      return NULL;
   const char *end = NULL;
   cJSON *j = cJSON_ParseWithLengthOpts(raw, len, &end, 0);
   if (!j || end != raw + len || !cJSON_IsObject(j))
   {
      cJSON_Delete(j);
      return NULL;
   }
   return j;
}

static int exact_two(const cJSON *j, const char *a, const char *b)
{
   unsigned seen = 0;
   size_t count = 0;
   for (const cJSON *p = j ? j->child : NULL; p; p = p->next)
   {
      unsigned bit = p->string && !strcmp(p->string, a)   ? 1U
                     : p->string && !strcmp(p->string, b) ? 2U
                                                          : 0U;
      if (!bit || (seen & bit))
         return 0;
      seen |= bit;
      count++;
   }
   return seen == 3U && count == 2;
}

int kb_management_action_body_parse(const char *raw, size_t len, kb_management_action_body_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out)
      return -1;
   cJSON *j = parse_exact(raw, len, KB_MANAGEMENT_ACTION_BODY_MAX);
   const cJSON *a = j ? cJSON_GetObjectItemCaseSensitive(j, "action") : NULL;
   const cJSON *n = j ? cJSON_GetObjectItemCaseSensitive(j, "agent") : NULL;
   const char *action = cJSON_IsString(a) ? cJSON_GetStringValue(a) : NULL;
   const char *agent = cJSON_IsString(n) ? cJSON_GetStringValue(n) : NULL;
   /* INGRESS contract: both spellings of the roster action are accepted. The ops
    * are `model.*` since the roster was named for what an entry is, but a
    * control plane still emitting the pre-rename `agent.*` is not redeployed in
    * lockstep with this service. The value is echoed verbatim into the canonical
    * form that gets digested, so neither spelling is rewritten into the other. */
   if (!exact_two(j, "action", "agent") || !action || !agent ||
       (strcmp(action, "model.enable") && strcmp(action, "model.disable") &&
        strcmp(action, "agent.enable") && strcmp(action, "agent.disable")) ||
       !token(agent, 63))
   {
      cJSON_Delete(j);
      return -1;
   }
   int written = snprintf(out->canonical, sizeof(out->canonical),
                          "{\"action\":\"%s\",\"agent\":\"%s\"}", action, agent);
   if (written < 0 || (size_t)written >= sizeof(out->canonical))
      goto fail;
   out->canonical_len = (size_t)written;
   memcpy(out->action, action, strlen(action) + 1);
   memcpy(out->agent, agent, strlen(agent) + 1);
   unsigned digest_len = 0;
   if (EVP_Digest(out->canonical, out->canonical_len, out->digest, &digest_len, EVP_sha256(),
                  NULL) != 1 ||
       digest_len != 32)
      goto fail;
   hex32(out->digest, out->digest_hex);
   cJSON_Delete(j);
   return 0;
fail:
   cJSON_Delete(j);
   OPENSSL_cleanse(out, sizeof(*out));
   return -1;
}

int kb_management_action_response_parse(const char *raw, size_t len, int status,
                                        db2_management_action_outcome_operation_t *out)
{
   if (!out)
      return -1;
   out->result = 0;
   out->result_class = 0;
   if (!raw || !len || len > KB_MANAGEMENT_ACTION_RESPONSE_MAX || memchr(raw, 0, len))
      return -1;
#define EXACT_RESPONSE(s) (len == sizeof(s) - 1 && memcmp(raw, (s), sizeof(s) - 1) == 0)
   if (status == 200 && EXACT_RESPONSE("{\"result\":\"succeeded\",\"effect\":\"applied\"}"))
      out->result = DB2_MANAGEMENT_ACTION_SUCCEEDED,
      out->result_class = DB2_MANAGEMENT_ACTION_CLASS_REMOTE_SUCCESS;
   else if (status == 403 && EXACT_RESPONSE("{\"result\":\"denied\",\"effect\":\"none\"}"))
      out->result = DB2_MANAGEMENT_ACTION_DENIED_RESULT,
      out->result_class = DB2_MANAGEMENT_ACTION_CLASS_REMOTE_DENIED;
   else if (status == 500 && EXACT_RESPONSE("{\"result\":\"failed\",\"effect\":\"none\"}"))
      out->result = DB2_MANAGEMENT_ACTION_FAILED,
      out->result_class = DB2_MANAGEMENT_ACTION_CLASS_REMOTE_FAILURE;
   else if (status == 502 &&
            EXACT_RESPONSE("{\"result\":\"indeterminate\",\"effect\":\"unknown\"}"))
      out->result = DB2_MANAGEMENT_ACTION_INDETERMINATE,
      out->result_class = DB2_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE;
   else
      return -1;
#undef EXACT_RESPONSE
   return 0;
}

static int snapshot_valid(const db2_server_snapshot_t *s, const db2_management_action_intent_t *i)
{
   return s && i && !strcmp(s->server_id, i->target_server_id) &&
          kb_mgmt_endpoint_validate(s->endpoint) == 0 && !strcmp(s->status, "active") &&
          !strcmp(s->enrollment_state, "active") && !s->revoked_at[0] &&
          !strcmp(s->management_issuer, i->target_mgmt_issuer) &&
          !strcmp(s->management_serial_norm, i->target_mgmt_serial_norm) &&
          CRYPTO_memcmp(s->management_fingerprint, i->target_mgmt_fingerprint, 64) == 0 &&
          s->revocation_generation == i->revocation_generation;
}

static int intent_matches_operation(const db2_management_action_intent_t *i,
                                    const db2_management_action_operation_t *o, uint64_t now)
{
   return i && o && now <= INT64_MAX && !strcmp(i->correlation_id, o->correlation_id) &&
          !strcmp(i->jti, o->jti) && i->team_id == o->team_id && i->capability == o->capability &&
          !strcmp(i->target_server_id, o->target_server_id) &&
          !strcmp(i->request_sha256, o->request_sha256) &&
          !strcmp(i->token_issuer, o->token_issuer) && !strcmp(i->audience, o->target_server_id) &&
          !strcmp(i->kid, o->kid) && !strcmp(i->installation_id, o->installation_id) &&
          i->issued_at > 0 && i->issued_at <= (int64_t)now && i->expires_at > (int64_t)now &&
          i->expires_at - i->issued_at == o->ttl_seconds;
}

static int snapshots_equal(const db2_server_snapshot_t *a, const db2_server_snapshot_t *b)
{
   return !strcmp(a->server_id, b->server_id) && !strcmp(a->endpoint, b->endpoint) &&
          !strcmp(a->status, b->status) && !strcmp(a->enrollment_state, b->enrollment_state) &&
          !strcmp(a->revoked_at, b->revoked_at) &&
          !strcmp(a->management_issuer, b->management_issuer) &&
          !strcmp(a->management_serial_norm, b->management_serial_norm) &&
          CRYPTO_memcmp(a->management_fingerprint, b->management_fingerprint, 64) == 0 &&
          a->revocation_generation == b->revocation_generation;
}

static int active_matches(const kb_management_cert_active_t *a,
                          const db2_management_action_intent_t *i)
{
   char fp[65];
   hex32(a->fingerprint, fp);
   return !strcmp(a->installation_id, i->installation_id) &&
          a->generation == i->installation_generation &&
          a->enrollment_id == i->installation_enrollment_id &&
          !strcmp(a->issuer, i->local_cert_issuer) &&
          !strcmp(a->serial_norm, i->local_cert_serial_norm) &&
          CRYPTO_memcmp(fp, i->local_cert_fingerprint, 64) == 0;
}

static void nonce_encode(const unsigned char in[32], char out[44])
{
   static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   uint32_t acc = 0;
   unsigned bits = 0;
   size_t n = 0;
   for (size_t i = 0; i < 32; i++)
   {
      acc = (acc << 8) | in[i];
      bits += 8;
      while (bits >= 6)
      {
         bits -= 6;
         out[n++] = abc[(acc >> bits) & 63];
         acc &= bits ? (1U << bits) - 1U : 0U;
      }
   }
   if (bits)
      out[n++] = abc[(acc << (6 - bits)) & 63];
   out[n] = 0;
}

static int status_matches(const char *json, const unsigned char nonce[32],
                          const kb_management_cert_active_t *active,
                          const db2_management_action_intent_t *intent,
                          const kb_management_action_dependencies_t *d, uint64_t now)
{
   kb_mgmt_status_t s;
   memset(&s, 0, sizeof(s));
   char fp[65];
   hex32(active->fingerprint, fp);
   int bad = kb_mgmt_status_from_json(json, &s) || strcmp(s.key_id, d->status_key_id) ||
             CRYPTO_memcmp(s.nonce, nonce, 32) || strcmp(s.caller_issuer, active->issuer) ||
             strcmp(s.caller_serial_norm, active->serial_norm) ||
             CRYPTO_memcmp(s.caller_fingerprint, fp, 64) ||
             strcmp(s.target_server_id, intent->target_server_id) ||
             CRYPTO_memcmp(s.target_mgmt_fingerprint, intent->target_mgmt_fingerprint, 64) ||
             strcmp(s.purpose, ACTION_PURPOSE) ||
             s.revocation_generation != (uint64_t)intent->revocation_generation ||
             kb_mgmt_status_validate(&s, now, (uint64_t)intent->revocation_generation) ||
             kb_mgmt_status_verify_signature(&s, d->status_public_key);
   OPENSSL_cleanse(&s, sizeof(s));
   return !bad;
}

static db2_management_action_result_t append_outcome(const kb_management_action_request_t *r,
                                                     const kb_management_action_dependencies_t *d,
                                                     db2_management_action_outcome_operation_t *op)
{
   db2_management_action_outcome_t stored;
   db2_management_action_result_t rc;
   do
   {
      memset(&stored, 0, sizeof(stored));
      rc = d->outcome_append(r->actor, op, &stored);
   } while (rc == DB2_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS &&
            d->monotonic_millis(d->clock_ctx) < r->deadline_millis);
   return rc;
}

static kb_management_action_result_t
journal_local_failure(const kb_management_action_request_t *r,
                      const kb_management_action_dependencies_t *d,
                      const db2_management_action_intent_t *intent)
{
   db2_management_action_outcome_operation_t op = {0};
   memcpy(op.correlation_id, intent->correlation_id, 65);
   op.team_id = intent->team_id;
   op.result = DB2_MANAGEMENT_ACTION_FAILED;
   op.result_class = DB2_MANAGEMENT_ACTION_CLASS_LOCAL_FAILURE;
   return append_outcome(r, d, &op) == DB2_MANAGEMENT_ACTION_OK
              ? KB_MANAGEMENT_ACTION_UNAVAILABLE
              : KB_MANAGEMENT_ACTION_INDETERMINATE;
}

kb_management_action_result_t
kb_management_action_execute(const kb_management_action_request_t *r,
                             const kb_management_action_dependencies_t *d)
{
   kb_management_action_body_t body = {0};
   db2_management_action_operation_t operation = {0};
   db2_management_action_intent_t intent = {0};
   db2_server_snapshot_t a = {0}, b = {0};
   kb_management_cert_bundle_t bundle = {0};
   kb_management_cert_active_t active = {0};
   kb_mgmt_token_authority_output_t token_out = {0};
   unsigned char nonce[32] = {0};
   char challenge[1024] = {0}, status_request[1024] = {0};
   char staple[KB_MGMT_STATUS_JSON_MAX + 1] = {0};
   char headers[KB_MGMT_TOKEN_WIRE_MAX + KB_MGMT_STATUS_JSON_MAX + 96] = {0};
   char response[KB_MANAGEMENT_ACTION_RESPONSE_MAX + 1] = {0};
   void *session = NULL;
   int loaded = 0, status = 0;
   kb_management_action_result_t result = KB_MANAGEMENT_ACTION_INVALID;
   if (!r || !d || !r->actor || !r->actor->authenticated || r->team_id < 1 ||
       !token(r->server_id, 127) || !r->deadline_millis || !d->operation_init || !d->intent_start ||
       !d->outcome_append || !d->snapshot || !d->bundle_load || !d->bundle_clear ||
       !d->server_open || !d->server_request || !d->server_close || !d->authority_issue ||
       !d->token_issue || !d->wall_seconds || !d->monotonic_millis || !d->status_public_key ||
       !token(d->status_key_id, 64) || !d->token_issuer || !d->kid || !d->installation_id ||
       d->ttl_seconds < 1 || d->ttl_seconds > 90 ||
       d->monotonic_millis(d->clock_ctx) >= r->deadline_millis ||
       kb_management_action_body_parse(r->body, r->body_len, &body))
      goto done;
   if (d->operation_init(r->team_id, r->server_id, DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES,
                         body.digest, d->token_issuer, d->kid, d->ttl_seconds, d->installation_id,
                         &operation) != DB2_MANAGEMENT_ACTION_OK)
   {
      result = KB_MANAGEMENT_ACTION_UNAVAILABLE;
      goto done;
   }
   db2_management_action_result_t jr;
   do
   {
      memset(&intent, 0, sizeof(intent));
      jr = d->intent_start(r->actor, &operation, &intent);
   } while (jr == DB2_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS &&
            d->monotonic_millis(d->clock_ctx) < r->deadline_millis);
   if (jr != DB2_MANAGEMENT_ACTION_OK)
   {
      result = jr == DB2_MANAGEMENT_ACTION_DENIED     ? KB_MANAGEMENT_ACTION_DENIED
               : jr == DB2_MANAGEMENT_ACTION_CONFLICT ? KB_MANAGEMENT_ACTION_CONFLICT
               : jr == DB2_MANAGEMENT_ACTION_INVALID  ? KB_MANAGEMENT_ACTION_INVALID
                                                      : KB_MANAGEMENT_ACTION_UNAVAILABLE;
      goto done;
   }
   if (intent.replayed)
   {
      result = KB_MANAGEMENT_ACTION_CONFLICT;
      goto done;
   }
   if (!intent_matches_operation(&intent, &operation, d->wall_seconds(d->clock_ctx)) ||
       strcmp(intent.request_sha256, body.digest_hex) ||
       strcmp(intent.target_server_id, r->server_id))
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   kb_management_health_result_t hr =
       d->snapshot(d->snapshot_ctx, r->actor, r->team_id, r->server_id, &a);
   if (hr != KB_MANAGEMENT_HEALTH_OK || !snapshot_valid(&a, &intent))
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   /* The loader may have populated private-key material even when it reports
    * an error or the returned metadata fails validation.  Once invoked, its
    * matching cleanup callback owns every exit path. */
   loaded = 1;
   hr = d->bundle_load(d->bundle_ctx, &bundle, &active);
   uint64_t active_now = d->wall_seconds(d->clock_ctx);
   if (hr != KB_MANAGEMENT_HEALTH_OK || !active_matches(&active, &intent) ||
       active_now > INT64_MAX || active.not_before_epoch < 1 ||
       active.not_before_epoch > (int64_t)active_now ||
       active.not_after_epoch <= (int64_t)active_now || active.revocation_generation < 1)
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   hr = d->server_open(d->server_ctx, &a, &bundle, r->deadline_millis, &session);
   if (hr != KB_MANAGEMENT_HEALTH_OK)
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   kb_management_action_transport_t tr =
       d->server_request(d->server_ctx, session, "POST", "/v1/management/action/challenge", "",
                         NULL, r->deadline_millis, challenge, sizeof(challenge), &status);
   uint64_t expires = 0, now = d->wall_seconds(d->clock_ctx);
   if (tr != KB_MANAGEMENT_ACTION_SENT_RESPONSE || status != 200 ||
       kb_management_health_challenge_decode(challenge, strlen(challenge), nonce, &expires) ||
       expires <= now || expires - now > 15)
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   uint64_t mono_now = d->monotonic_millis(d->clock_ctx);
   uint64_t expiry_budget = expires - now > (UINT64_MAX - mono_now) / 1000U
                                ? UINT64_MAX
                                : mono_now + (expires - now) * 1000U;
   uint64_t protocol_deadline =
       expiry_budget < r->deadline_millis ? expiry_budget : r->deadline_millis;
   if (mono_now >= protocol_deadline)
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   char encoded[44];
   nonce_encode(nonce, encoded);
   int n =
       snprintf(status_request, sizeof(status_request),
                "{\"nonce\":\"%s\",\"target\":\"%s\",\"target_mgmt_fp\":\"%s\","
                "\"purpose\":\"%s\"}",
                encoded, intent.target_server_id, intent.target_mgmt_fingerprint, ACTION_PURPOSE);
   if (n < 0 || (size_t)n >= sizeof(status_request) ||
       d->authority_issue(d->authority_ctx, &bundle, status_request, (size_t)n, protocol_deadline,
                          staple, sizeof(staple), &status) != KB_MANAGEMENT_HEALTH_OK ||
       status != 200 ||
       !status_matches(staple, nonce, &active, &intent, d, d->wall_seconds(d->clock_ctx)))
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   hr = d->snapshot(d->snapshot_ctx, r->actor, r->team_id, r->server_id, &b);
   if (hr != KB_MANAGEMENT_HEALTH_OK || !snapshot_valid(&b, &intent) || !snapshots_equal(&a, &b))
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   if (d->wall_seconds(d->clock_ctx) >= expires ||
       d->monotonic_millis(d->clock_ctx) >= protocol_deadline)
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   if (d->token_issue(d->token_ctx, intent.correlation_id, intent.jti, &token_out) !=
           KB_MGMT_TOKEN_AUTHORITY_IPC_OK ||
       !token_out.jwt_len || token_out.jwt_len > KB_MGMT_TOKEN_WIRE_MAX ||
       strnlen(token_out.jwt, KB_MGMT_TOKEN_WIRE_MAX + 1) != token_out.jwt_len ||
       strpbrk(token_out.jwt, "\r\n"))
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   if (d->wall_seconds(d->clock_ctx) >= expires ||
       d->monotonic_millis(d->clock_ctx) >= protocol_deadline)
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   char authorization[KB_MGMT_TOKEN_WIRE_MAX + 8] = "";
   if (aimee_core_bearer_value(authorization, sizeof(authorization), token_out.jwt) != 0)
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   n = snprintf(headers, sizeof(headers), "Authorization: %s\r\nX-Aimee-Management-Status: %s\r\n",
                authorization, staple);
   if (n < 0 || (size_t)n >= sizeof(headers))
   {
      result = journal_local_failure(r, d, &intent);
      goto done;
   }
   tr = d->server_request(d->server_ctx, session, "POST", "/v1/management/action", body.canonical,
                          headers, protocol_deadline, response, sizeof(response), &status);
   db2_management_action_outcome_operation_t outcome = {0};
   memcpy(outcome.correlation_id, intent.correlation_id, 65);
   outcome.team_id = intent.team_id;
   if (tr == KB_MANAGEMENT_ACTION_NOT_SENT)
   {
      outcome.result = DB2_MANAGEMENT_ACTION_FAILED;
      outcome.result_class = DB2_MANAGEMENT_ACTION_CLASS_LOCAL_FAILURE;
   }
   else if (tr == KB_MANAGEMENT_ACTION_SENT_AMBIGUOUS)
   {
      outcome.result = DB2_MANAGEMENT_ACTION_INDETERMINATE;
      outcome.result_class = DB2_MANAGEMENT_ACTION_CLASS_TRANSPORT_AMBIGUOUS;
   }
   else
   {
      size_t response_len = strnlen(response, sizeof(response));
      outcome.has_status_code = status >= 100 && status <= 599;
      outcome.status_code = outcome.has_status_code ? status : 0;
      outcome.has_response_sha256 = response_len > 0;
      if (outcome.has_response_sha256)
      {
         uint8_t digest[32];
         unsigned digest_len = 0;
         if (EVP_Digest(response, response_len, digest, &digest_len, EVP_sha256(), NULL) != 1 ||
             digest_len != 32)
         {
            result = KB_MANAGEMENT_ACTION_INDETERMINATE;
            goto done;
         }
         hex32(digest, outcome.response_sha256);
         OPENSSL_cleanse(digest, sizeof(digest));
      }
      if (kb_management_action_response_parse(response, response_len, status, &outcome))
      {
         outcome.result = DB2_MANAGEMENT_ACTION_INDETERMINATE;
         outcome.result_class = DB2_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE;
      }
   }
   jr = append_outcome(r, d, &outcome);
   if (jr != DB2_MANAGEMENT_ACTION_OK)
      result = KB_MANAGEMENT_ACTION_INDETERMINATE;
   else if (outcome.result == DB2_MANAGEMENT_ACTION_SUCCEEDED)
      result = KB_MANAGEMENT_ACTION_OK;
   else if (outcome.result == DB2_MANAGEMENT_ACTION_DENIED_RESULT)
      result = KB_MANAGEMENT_ACTION_DENIED;
   else if (outcome.result == DB2_MANAGEMENT_ACTION_FAILED)
      result = KB_MANAGEMENT_ACTION_UNAVAILABLE;
   else
      result = KB_MANAGEMENT_ACTION_INDETERMINATE;
done:
   if (session)
      d->server_close(d->server_ctx, session);
   if (loaded)
      d->bundle_clear(d->bundle_ctx, &bundle);
   OPENSSL_cleanse(&bundle, sizeof(bundle));
   OPENSSL_cleanse(&active, sizeof(active));
   OPENSSL_cleanse(&token_out, sizeof(token_out));
   OPENSSL_cleanse(nonce, sizeof(nonce));
   OPENSSL_cleanse(staple, sizeof(staple));
   OPENSSL_cleanse(headers, sizeof(headers));
   return result;
}

kb_management_action_transport_t kb_management_action_server_request_production(
    void *unused, void *opaque, const char *method, const char *path, const char *body,
    const char *headers, uint64_t deadline, char *response, size_t cap, int *status)
{
   (void)unused;
   if (method && path && !strcmp(method, "POST") && !strcmp(path, "/v1/management/action"))
   {
      kb_mgmt_client_send_result_t rc = kb_mgmt_client_session_action_deadline(
          opaque, body, headers, deadline, response, cap, status);
      return rc == KB_MGMT_CLIENT_SENT_RESPONSE    ? KB_MANAGEMENT_ACTION_SENT_RESPONSE
             : rc == KB_MGMT_CLIENT_SENT_AMBIGUOUS ? KB_MANAGEMENT_ACTION_SENT_AMBIGUOUS
                                                   : KB_MANAGEMENT_ACTION_NOT_SENT;
   }
   return kb_mgmt_client_session_request_deadline(opaque, method, path, body, headers, deadline,
                                                  response, cap, status) == 0
              ? KB_MANAGEMENT_ACTION_SENT_RESPONSE
              : KB_MANAGEMENT_ACTION_NOT_SENT;
}

kb_mgmt_token_authority_ipc_result_t
kb_management_action_token_issue_production(void *ctx, const char *correlation, const char *jti,
                                            kb_mgmt_token_authority_output_t *out)
{
   return kb_mgmt_token_authority_client_issue(ctx, correlation, jti, out);
}
