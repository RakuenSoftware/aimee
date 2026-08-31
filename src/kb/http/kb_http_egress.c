/* P2b-a buffered certificate-authorized Bedrock egress route. */
#include "kb_http_egress.h"

#include <aimee/translation/aimee_frontend.h>
#include "cJSON.h"
#include "modules/db2/c/enrollments.h"
#include "modules/db2/c/org_egress.h"
#include "modules/db2/c/db2_tenant.h"
#include "../kb_bedrock_egress.h"
#include "../kb_vault_key_use.h"
#include "../kb_vault_policy.h"

#include <math.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define P2B_LEASE_SECONDS 60

typedef struct
{
   kb_bedrock_authorized_target_t *target;
   const aimee_request_t *ir;
   const char *canonical_body;
   size_t canonical_body_len;
   kb_bedrock_wire_request_t *wire;
   char amz_date[17];
   char date[9];
} sign_context_t;

static int envelope_field_allowed(const char *name)
{
   static const char *allowed[] = {"request_id", "team_id", "project_id",
                                   "model_id",   "stream",  "payload"};
   for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
      if (strcmp(name, allowed[i]) == 0)
         return 1;
   return 0;
}

static int payload_field_allowed(const char *name)
{
   static const char *allowed[] = {"model", "messages", "max_tokens", "temperature",
                                   "top_p", "stop",     "stream"};
   for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
      if (strcmp(name, allowed[i]) == 0)
         return 1;
   return 0;
}

static int object_unique_allowed(const cJSON *root, int envelope)
{
   for (const cJSON *a = root ? root->child : NULL; a; a = a->next)
   {
      if (!a->string ||
          !(envelope ? envelope_field_allowed(a->string) : payload_field_allowed(a->string)))
         return 0;
      for (const cJSON *b = a->next; b; b = b->next)
         if (b->string && strcmp(a->string, b->string) == 0)
            return 0;
   }
   return 1;
}

static int uuid_v4(const char *s)
{
   if (!s || strlen(s) != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-' ||
       s[14] != '4' || !strchr("89ab", s[19]))
      return 0;
   for (int i = 0; i < 36; i++)
   {
      if (i == 8 || i == 13 || i == 18 || i == 23)
         continue;
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   }
   return 1;
}

static int json_int64(const cJSON *v, int64_t *out)
{
   if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 1 ||
       v->valuedouble > 9007199254740991.0 || floor(v->valuedouble) != v->valuedouble)
      return 0;
   *out = (int64_t)v->valuedouble;
   return 1;
}

static int text_only_ir(const aimee_request_t *ir)
{
   if (!ir || !ir->model || !ir->model[0] || !ir->has_max_tokens || ir->max_tokens < 1 ||
       ir->stream || ir->n_messages < 1 || ir->n_tools || ir->tool_choice || ir->metadata ||
       ir->service_tier || ir->thinking || ir->has_top_k)
      return 0;
   for (int i = 0; i < ir->n_system; i++)
      if (ir->system[i].type != AIMEE_BLK_TEXT || !ir->system[i].text)
         return 0;
   for (int i = 0; i < ir->n_messages; i++)
   {
      if (!ir->messages[i].role ||
          (strcmp(ir->messages[i].role, "user") && strcmp(ir->messages[i].role, "assistant")) ||
          ir->messages[i].n_blocks < 1)
         return 0;
      for (int j = 0; j < ir->messages[i].n_blocks; j++)
         if (ir->messages[i].blocks[j].type != AIMEE_BLK_TEXT || !ir->messages[i].blocks[j].text)
            return 0;
   }
   return 1;
}

static int hex_random(char out[33])
{
   static const char h[] = "0123456789abcdef";
   unsigned char raw[16];
   if (RAND_bytes(raw, sizeof(raw)) != 1)
      return -1;
   for (size_t i = 0; i < sizeof(raw); i++)
   {
      out[2 * i] = h[raw[i] >> 4];
      out[2 * i + 1] = h[raw[i] & 15];
   }
   out[32] = '\0';
   OPENSSL_cleanse(raw, sizeof(raw));
   return 0;
}

static int digest_part(EVP_MD_CTX *ctx, const void *bytes, size_t len)
{
   unsigned char n[8];
   uint64_t v = (uint64_t)len;
   for (int i = 7; i >= 0; i--)
   {
      n[i] = (unsigned char)(v & 0xff);
      v >>= 8;
   }
   return EVP_DigestUpdate(ctx, n, sizeof(n)) == 1 &&
                  EVP_DigestUpdate(ctx, bytes ? bytes : "", len) == 1
              ? 0
              : -1;
}

static int request_digest(const char *authority, const char *request_id, int64_t team,
                          int has_project, int64_t project, const char *model, const char *body,
                          size_t body_len, char out[65])
{
   char team_s[32], project_s[40];
   snprintf(team_s, sizeof(team_s), "%lld", (long long)team);
   snprintf(project_s, sizeof(project_s), has_project ? "1:%lld" : "0", (long long)project);
   const char *domain = "aimee.p2b.egress.v1";
   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   unsigned char md[EVP_MAX_MD_SIZE];
   unsigned int n = 0;
   int rc = !ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
                    digest_part(ctx, domain, strlen(domain)) ||
                    digest_part(ctx, authority, strlen(authority)) ||
                    digest_part(ctx, request_id, strlen(request_id)) ||
                    digest_part(ctx, team_s, strlen(team_s)) ||
                    digest_part(ctx, project_s, strlen(project_s)) ||
                    digest_part(ctx, model, strlen(model)) || digest_part(ctx, body, body_len) ||
                    EVP_DigestFinal_ex(ctx, md, &n) != 1 || n != 32
                ? -1
                : 0;
   if (ctx)
      EVP_MD_CTX_free(ctx);
   static const char hex[] = "0123456789abcdef";
   if (!rc)
   {
      for (unsigned i = 0; i < n; i++)
      {
         out[2 * i] = hex[md[i] >> 4];
         out[2 * i + 1] = hex[md[i] & 15];
      }
      out[64] = '\0';
   }
   OPENSSL_cleanse(md, sizeof(md));
   return rc;
}

static void skip_ws(const unsigned char **p, const unsigned char *end)
{
   while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n'))
      (*p)++;
}

static int json_borrowed_string(const unsigned char **p, const unsigned char *end,
                                const unsigned char **value, size_t *len)
{
   skip_ws(p, end);
   if (*p >= end || *(*p)++ != '"')
      return -1;
   const unsigned char *s = *p;
   while (*p < end && **p != '"')
   {
      if (**p == '\\' || **p < 0x20 || **p == 0x7f)
         return -1;
      (*p)++;
   }
   if (*p >= end)
      return -1;
   *value = s;
   *len = (size_t)(*p - s);
   (*p)++;
   return *len ? 0 : -1;
}

static int credential_parse(const unsigned char *plain, size_t plain_len,
                            kb_bedrock_credential_view_t *view)
{
   const unsigned char *p = plain, *end = plain + plain_len;
   int seen_access = 0, seen_secret = 0;
   memset(view, 0, sizeof(*view));
   skip_ws(&p, end);
   if (p >= end || *p++ != '{')
      return -1;
   for (;;)
   {
      const unsigned char *key, *value;
      size_t key_len, value_len;
      skip_ws(&p, end);
      if (p < end && *p == '}')
      {
         p++;
         break;
      }
      if (json_borrowed_string(&p, end, &key, &key_len) ||
          (skip_ws(&p, end), p >= end || *p++ != ':') ||
          json_borrowed_string(&p, end, &value, &value_len))
         return -1;
      if (key_len == 13 && !memcmp(key, "access_key_id", 13) && !seen_access)
      {
         view->access_key_id = value;
         view->access_key_id_len = value_len;
         seen_access = 1;
      }
      else if (key_len == 17 && !memcmp(key, "secret_access_key", 17) && !seen_secret)
      {
         view->secret_access_key = value;
         view->secret_access_key_len = value_len;
         seen_secret = 1;
      }
      else
         return -1;
      skip_ws(&p, end);
      if (p < end && *p == ',')
      {
         p++;
         continue;
      }
      if (p < end && *p == '}')
      {
         p++;
         break;
      }
      return -1;
   }
   skip_ws(&p, end);
   return p == end && seen_access && seen_secret ? 0 : -1;
}

static int sign_callback(const unsigned char *plaintext, size_t plaintext_len, void *opaque)
{
   sign_context_t *ctx = opaque;
   kb_bedrock_credential_view_t view;
   if (credential_parse(plaintext, plaintext_len, &view))
      return -1;
   view.amz_date = ctx->amz_date;
   view.date = ctx->date;
   kb_bedrock_result_t r = kb_bedrock_authorized_wire_build(ctx->target, ctx->ir, &view, ctx->wire);
   OPENSSL_cleanse(&view, sizeof(view));
   if (r != KB_BEDROCK_OK || ctx->wire->body_len != ctx->canonical_body_len ||
       CRYPTO_memcmp(ctx->wire->body, ctx->canonical_body, ctx->canonical_body_len) != 0)
   {
      kb_bedrock_wire_request_clear(ctx->wire);
      return -1;
   }
   return 0;
}

static int settle(const kb_principal_t *transport, int64_t team, int64_t id, const char *owner,
                  int64_t generation, const char *state, int http, const aimee_response_t *response,
                  const char *outcome, const char *basis)
{
   if (db2_tenant_scope_begin(transport, team) != 0)
      return -1;
   int ok = 0;
   int rc = db2_org_egress_settle(
       id, owner, generation, state, http, response ? response->usage_in : 0,
       response ? response->usage_out : 0, response ? response->usage_cache_read : 0,
       response ? response->usage_cache_write : 0, outcome, basis, &ok);
   if (rc || !ok)
   {
      db2_tenant_scope_rollback();
      return -1;
   }
   return db2_tenant_scope_commit() == 0 ? 0 : -1;
}

static int renew_dispatch_lease(const kb_principal_t *transport, int64_t team, int64_t id,
                                const char *owner, int64_t generation)
{
   if (db2_tenant_scope_begin(transport, team) != 0)
      return -1;
   int ok = 0;
   int rc = db2_org_egress_heartbeat(id, owner, generation, P2B_LEASE_SECONDS, &ok);
   if (rc != 0 || !ok)
   {
      db2_tenant_scope_rollback();
      return -1;
   }
   return db2_tenant_scope_commit() == 0 ? 0 : -1;
}

static int render_success(const aimee_response_t *response, const char *model,
                          const char *request_id, int64_t team_id, char *out, int cap)
{
   cJSON *root = openai_frontend_render(response);
   if (!root)
      return -1;
   cJSON_DeleteItemFromObjectCaseSensitive(root, "model");
   cJSON_AddStringToObject(root, "model", model);
   cJSON_AddStringToObject(root, "request_id", request_id);
   cJSON_AddNumberToObject(root, "team_id", (double)team_id);
   char *json = cJSON_PrintUnformatted(root);
   int ok = json && strlen(json) < (size_t)cap;
   if (ok)
      memcpy(out, json, strlen(json) + 1);
   free(json);
   cJSON_Delete(root);
   return ok ? 0 : -1;
}

int kb_http_egress_route(const char *method, const char *path, const char *body, int body_len,
                         const kb_principal_t *transport, const char *fingerprint, char *out,
                         int out_cap)
{
   if (strcmp(path, "/v1/llm/egress"))
      return -1;
   if (strcmp(method, "POST"))
   {
      snprintf(out, (size_t)out_cap, "{\"error\":\"method not allowed\"}");
      return 405;
   }
   if (!transport || transport->kind != KB_PRIN_CERT || !transport->authenticated || !fingerprint ||
       !kb_egress_release_allowed())
   {
      snprintf(out, (size_t)out_cap, "{\"error\":\"egress unavailable\"}");
      return 503;
   }
   const char *endp = NULL;
   cJSON *root =
       body && body_len > 0 ? cJSON_ParseWithLengthOpts(body, (size_t)body_len, &endp, 0) : NULL;
   if (!root || !cJSON_IsObject(root) || !object_unique_allowed(root, 1) || !endp ||
       endp != body + body_len)
   {
      cJSON_Delete(root);
      snprintf(out, (size_t)out_cap, "{\"error\":\"invalid request\"}");
      return 400;
   }
   cJSON *jreq = cJSON_GetObjectItemCaseSensitive(root, "request_id"),
         *jteam = cJSON_GetObjectItemCaseSensitive(root, "team_id");
   cJSON *jproject = cJSON_GetObjectItemCaseSensitive(root, "project_id");
   cJSON *jmodel = cJSON_GetObjectItemCaseSensitive(root, "model_id");
   cJSON *jstream = cJSON_GetObjectItemCaseSensitive(root, "stream");
   cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
   int64_t team = 0, project = 0;
   int has_project = 0;
   if (!cJSON_IsString(jreq) || !uuid_v4(jreq->valuestring) || !json_int64(jteam, &team) ||
       (jproject && !(has_project = json_int64(jproject, &project))) || !cJSON_IsString(jmodel) ||
       !jmodel->valuestring[0] || !cJSON_IsFalse(jstream) || !cJSON_IsObject(payload) ||
       !object_unique_allowed(payload, 0))
   {
      cJSON_Delete(root);
      snprintf(out, (size_t)out_cap, "{\"error\":\"invalid authority fields\"}");
      return 400;
   }
   char request_id[37];
   memcpy(request_id, jreq->valuestring, sizeof(request_id));
   char model_id[201];
   if (strlen(jmodel->valuestring) >= sizeof(model_id))
   {
      cJSON_Delete(root);
      snprintf(out, (size_t)out_cap, "{\"error\":\"invalid model\"}");
      return 400;
   }
   memcpy(model_id, jmodel->valuestring, strlen(jmodel->valuestring) + 1);
   aimee_request_t ir;
   char parse_err[128] = "";
   if (openai_frontend_parse(payload, &ir, parse_err, sizeof(parse_err)) != 0 ||
       !text_only_ir(&ir) || strcmp(ir.model, model_id) != 0 ||
       !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(payload, "stream")))
   {
      cJSON_Delete(root);
      aimee_request_free(&ir);
      snprintf(out, (size_t)out_cap, "{\"error\":\"unsupported payload\"}");
      return 400;
   }
   cJSON_Delete(root);
   char authority[33], origin[576];
   int enrollment_rc = db2_enrollment_authority_resolve(fingerprint, transport->issuer,
                                                        transport->subject, authority);
   if (enrollment_rc == 1)
   {
      aimee_request_free(&ir);
      snprintf(out, (size_t)out_cap, "{\"error\":\"not authorized\"}");
      return 401;
   }
   if (enrollment_rc != 0)
   {
      aimee_request_free(&ir);
      snprintf(out, (size_t)out_cap, "{\"error\":\"admission unavailable\"}");
      return 503;
   }
   if (kb_identity_key(transport, origin, sizeof(origin)) != 0)
   {
      aimee_request_free(&ir);
      snprintf(out, (size_t)out_cap, "{\"error\":\"not authorized\"}");
      return 403;
   }
   char *canonical = NULL;
   size_t canonical_len = 0;
   char digest[65];
   if (kb_bedrock_canonical_body(&ir, &canonical, &canonical_len) != KB_BEDROCK_OK ||
       request_digest(authority, request_id, team, has_project, project, model_id, canonical,
                      canonical_len, digest))
   {
      aimee_request_free(&ir);
      free(canonical);
      snprintf(out, (size_t)out_cap, "{\"error\":\"invalid payload\"}");
      return 400;
   }
   kb_bedrock_authorized_target_t *target = NULL;
   db2_org_egress_admission_t admission;
   int ar = -1;
   int scope_rc = db2_tenant_scope_begin(transport, team);
   if (scope_rc != 0)
   {
      kb_bedrock_authorized_target_clear(&target);
      free(canonical);
      aimee_request_free(&ir);
      snprintf(out, (size_t)out_cap, "{\"error\":\"%s\"}",
               scope_rc == DB2_ERR_TENANT_DENIED ? "not authorized" : "admission unavailable");
      return scope_rc == DB2_ERR_TENANT_DENIED ? 403 : 503;
   }
   kb_bedrock_result_t target_rc = kb_bedrock_authorized_target_resolve(team, model_id, &target);
   if (target_rc != KB_BEDROCK_OK)
   {
      db2_tenant_scope_rollback();
      kb_bedrock_authorized_target_clear(&target);
      free(canonical);
      aimee_request_free(&ir);
      snprintf(out, (size_t)out_cap, "{\"error\":\"%s\"}",
               target_rc == KB_BEDROCK_INVALID_TARGET ? "not authorized" : "admission unavailable");
      return target_rc == KB_BEDROCK_INVALID_TARGET ? 403 : 503;
   }
   if ((ar = db2_org_egress_admit(authority, fingerprint, transport->issuer, transport->subject,
                                  origin, request_id, team, has_project, project, model_id, digest,
                                  P2B_LEASE_SECONDS, &admission)) != 0)
   {
      db2_tenant_scope_rollback();
      kb_bedrock_authorized_target_clear(&target);
      free(canonical);
      aimee_request_free(&ir);
      snprintf(out, (size_t)out_cap, "{\"error\":\"%s\"}",
               ar == DB2_EGRESS_ERR_DENIED ? "not authorized" : "admission unavailable");
      return ar == DB2_EGRESS_ERR_CONFLICT ? 409 : ar == DB2_EGRESS_ERR_DENIED ? 403 : 503;
   }
   if (admission.outcome == DB2_EGRESS_RATE_REFUSED ||
       admission.outcome == DB2_EGRESS_BUDGET_REFUSED)
   {
      db2_tenant_scope_rollback();
      kb_bedrock_authorized_target_clear(&target);
      free(canonical);
      aimee_request_free(&ir);
      snprintf(out, (size_t)out_cap, "{\"error\":\"admission refused\"}");
      return 429;
   }
   if (admission.outcome == DB2_EGRESS_REPLAY)
   {
      db2_tenant_scope_rollback();
      kb_bedrock_authorized_target_clear(&target);
      free(canonical);
      aimee_request_free(&ir);
      snprintf(out, (size_t)out_cap, "{\"error\":\"request already recorded\",\"state\":\"%s\"}",
               admission.state);
      return 409;
   }
   if (ir.max_tokens > admission.max_output_tokens)
   {
      db2_tenant_scope_rollback();
      kb_bedrock_authorized_target_clear(&target);
      free(canonical);
      aimee_request_free(&ir);
      snprintf(out, (size_t)out_cap, "{\"error\":\"admission failed\"}");
      return 400;
   }
   if (db2_tenant_scope_commit() != 0)
   {
      kb_bedrock_authorized_target_clear(&target);
      free(canonical);
      aimee_request_free(&ir);
      snprintf(out, (size_t)out_cap, "{\"error\":\"admission unavailable\"}");
      return 503;
   }
   char owner[33];
   int64_t dispatch_id = 0, generation = 0;
   int claim_failed = hex_random(owner);
   if (!claim_failed && db2_tenant_scope_begin(transport, team) != 0)
      claim_failed = 1;
   if (!claim_failed && db2_org_egress_begin(authority, request_id, owner, "aimee-kb",
                                             P2B_LEASE_SECONDS, &dispatch_id, &generation) != 0)
   {
      db2_tenant_scope_rollback();
      claim_failed = 1;
   }
   else if (!claim_failed && db2_tenant_scope_commit() != 0)
      claim_failed = 1;
   if (claim_failed)
   {
      kb_bedrock_authorized_target_clear(&target);
      free(canonical);
      aimee_request_free(&ir);
      snprintf(out, (size_t)out_cap, "{\"error\":\"dispatch unavailable\"}");
      return 503;
   }
   kb_bedrock_wire_request_t wire;
   kb_bedrock_wire_request_init(&wire);
   sign_context_t sc = {.target = target,
                        .ir = &ir,
                        .canonical_body = canonical,
                        .canonical_body_len = canonical_len,
                        .wire = &wire};
   time_t now = time(NULL);
   struct tm tmv;
   gmtime_r(&now, &tmv);
   strftime(sc.amz_date, sizeof(sc.amz_date), "%Y%m%dT%H%M%SZ", &tmv);
   strftime(sc.date, sizeof(sc.date), "%Y%m%d", &tmv);
   kb_vault_key_use_status_t vu = KB_VAULT_KEY_USE_RETRY;
   if (renew_dispatch_lease(transport, team, dispatch_id, owner, generation) == 0)
      vu = kb_vault_key_use(transport, team, transport, request_id, admission.key_id,
                            admission.vault_principal, admission.vault_agent, admission.vault_cred,
                            digest, "bedrock", model_id, "converse", sign_callback, &sc);
   aimee_response_t response;
   kb_bedrock_response_init(&response);
   int http = 0, vendor_possible = 0;
   int status = 503;
   if (vu != KB_VAULT_KEY_USE_OK)
   {
      /* P7 may already have committed the WORM intent before returning a
       * non-OK status.  Its public enum does not prove otherwise, so retain the
       * full reservation and forbid any retry under this request ID. */
      (void)settle(transport, team, dispatch_id, owner, generation, "uncertain", 0, NULL,
                   "vault_outcome_uncertain", "reservation");
      snprintf(out, (size_t)out_cap, "{\"error\":\"dispatch unavailable\"}");
      status = 504;
   }
   else if (renew_dispatch_lease(transport, team, dispatch_id, owner, generation) != 0)
   {
      snprintf(out, (size_t)out_cap, "{\"error\":\"dispatch ownership lost\"}");
   }
   else
   {
      int guarded = 0;
      int guard_scope_rc = db2_tenant_scope_begin(transport, team);
      if (guard_scope_rc != 0)
      {
         snprintf(out, (size_t)out_cap, "{\"error\":\"dispatch ownership lost\"}");
         status = 504;
      }
      else if (db2_org_egress_owner_guard(dispatch_id, owner, generation, &guarded) != 0 ||
               !guarded)
      {
         db2_tenant_scope_rollback();
         snprintf(out, (size_t)out_cap, "{\"error\":\"dispatch ownership lost\"}");
         status = 504;
      }
      else
      {
         kb_bedrock_result_t br =
             kb_bedrock_authorized_wire_dispatch(target, &wire, &response, &http, &vendor_possible);
         int complete_ok = br == KB_BEDROCK_OK && http == 200;
         int complete_denial =
             br == KB_BEDROCK_PROVIDER_ERROR && http > 0 && (http < 200 || http > 299);
         int definite_prewrite = http == 0 && !vendor_possible;
         const char *terminal = complete_ok         ? "succeeded"
                                : complete_denial   ? "denied"
                                : definite_prewrite ? "failed"
                                                    : "uncertain";
         const char *klass = complete_ok         ? "complete"
                             : complete_denial   ? "provider_denied"
                             : definite_prewrite ? "prewrite_failed"
                                                 : "transport_ambiguous";
         const char *basis = complete_ok ? "actual" : definite_prewrite ? "zero" : "reservation";
         int durable_http = complete_ok ? 200 : complete_denial ? http : 0;
         int settled = 0;
         int settle_rc = db2_org_egress_settle(
             dispatch_id, owner, generation, terminal, durable_http,
             complete_ok ? response.usage_in : 0, complete_ok ? response.usage_out : 0,
             complete_ok ? response.usage_cache_read : 0,
             complete_ok ? response.usage_cache_write : 0, klass, basis, &settled);
         if (settle_rc != 0 || !settled)
         {
            db2_tenant_scope_rollback();
            snprintf(out, (size_t)out_cap, "{\"error\":\"ambiguous settlement\"}");
            status = 504;
         }
         else if (db2_tenant_scope_commit() != 0)
         {
            snprintf(out, (size_t)out_cap, "{\"error\":\"ambiguous settlement\"}");
            status = 504;
         }
         else if (complete_ok &&
                  render_success(&response, model_id, request_id, team, out, out_cap) == 0)
            status = 200;
         else
         {
            snprintf(out, (size_t)out_cap, "{\"error\":\"provider dispatch failed\"}");
            status = (complete_denial || definite_prewrite) ? 502 : 504;
         }
      }
   }
   aimee_response_free(&response);
   kb_bedrock_wire_request_clear(&wire);
   kb_bedrock_authorized_target_clear(&target);
   OPENSSL_cleanse(canonical, canonical_len);
   free(canonical);
   aimee_request_free(&ir);
   OPENSSL_cleanse(owner, sizeof(owner));
   return status;
}
