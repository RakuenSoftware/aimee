#include "kb_management_health_exchange.h"

#include "cJSON.h"
#include "modules/db2/c/db2_tenant.h"
#include "kb_mgmt_endpoint.h"

#include <openssl/crypto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEALTH_PURPOSE "management.health.v1"

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

static int lower_hex(const char *s, size_t n)
{
   if (!s || strnlen(s, n + 1) != n)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int decimal(const char *s, uint64_t *out)
{
   if (!s || !*s || !out || (s[0] == '0' && s[1]))
      return -1;
   uint64_t v = 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
   {
      if (*p < '0' || *p > '9' || v > (UINT64_MAX - (*p - '0')) / 10)
         return -1;
      v = v * 10 + (*p - '0');
   }
   *out = v;
   return 0;
}

static int exact_object(const cJSON *j, const char *const *names, size_t count)
{
   unsigned seen = 0;
   size_t fields = 0;
   if (!cJSON_IsObject(j) || count > sizeof(seen) * 8)
      return 0;
   for (const cJSON *p = j->child; p; p = p->next)
   {
      size_t i;
      for (i = 0; i < count && (!p->string || strcmp(p->string, names[i])); i++)
         ;
      if (i == count || (seen & (1U << i)))
         return 0;
      seen |= 1U << i;
      fields++;
   }
   return fields == count;
}

static int b64_value(unsigned char c)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   return c == '-' ? 62 : (c == '_' ? 63 : -1);
}

static int nonce_decode(const char *s, unsigned char out[32])
{
   if (!s || strlen(s) != 43)
      return -1;
   uint32_t acc = 0;
   unsigned bits = 0;
   size_t o = 0;
   for (size_t i = 0; i < 43; i++)
   {
      int v = b64_value((unsigned char)s[i]);
      if (v < 0)
         return -1;
      acc = (acc << 6) | (unsigned)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         if (o >= 32)
            return -1;
         out[o++] = (unsigned char)(acc >> bits);
         acc &= bits ? (1U << bits) - 1U : 0U;
      }
   }
   return o == 32 && bits == 2 && acc == 0 ? 0 : -1;
}

static void nonce_encode(const unsigned char in[32], char out[44])
{
   static const char alphabet[] =
       "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   uint32_t acc = 0;
   unsigned bits = 0;
   size_t o = 0;
   for (size_t i = 0; i < 32; i++)
   {
      acc = (acc << 8) | in[i];
      bits += 8;
      while (bits >= 6)
      {
         bits -= 6;
         out[o++] = alphabet[(acc >> bits) & 63];
         acc &= bits ? (1U << bits) - 1U : 0U;
      }
   }
   if (bits)
      out[o++] = alphabet[(acc << (6 - bits)) & 63];
   out[o] = '\0';
}

static cJSON *parse_exact(const char *raw, size_t len)
{
   if (!raw || len < 2 || len > KB_MANAGEMENT_HEALTH_RESPONSE_MAX || memchr(raw, '\0', len) ||
       raw[0] != '{' || raw[len - 1] != '}')
      return NULL;
   const char *end = NULL;
   cJSON *j = cJSON_ParseWithLengthOpts(raw, len, &end, 0);
   if (!j || end != raw + len)
   {
      cJSON_Delete(j);
      return NULL;
   }
   return j;
}

int kb_management_health_challenge_decode(const char *raw, size_t len, unsigned char nonce[32],
                                          uint64_t *expires)
{
   static const char *const names[] = {"nonce", "expires_at"};
   if (!nonce || !expires)
      return -1;
   memset(nonce, 0, 32);
   *expires = 0;
   cJSON *j = parse_exact(raw, len);
   const cJSON *n = j ? cJSON_GetObjectItemCaseSensitive(j, "nonce") : NULL;
   const cJSON *e = j ? cJSON_GetObjectItemCaseSensitive(j, "expires_at") : NULL;
   const char *ns = cJSON_IsString(n) ? cJSON_GetStringValue(n) : NULL;
   const char *es = cJSON_IsString(e) ? cJSON_GetStringValue(e) : NULL;
   int rc = exact_object(j, names, 2) && ns && es && nonce_decode(ns, nonce) == 0 &&
                    decimal(es, expires) == 0
                ? 0
                : -1;
   cJSON_Delete(j);
   if (rc)
   {
      OPENSSL_cleanse(nonce, 32);
      *expires = 0;
   }
   return rc;
}

int kb_management_read_challenge_decode(const char *raw, size_t len, const char *purpose,
                                        unsigned char nonce[32], uint64_t *expires)
{
   static const char *const names[] = {"nonce", "purpose", "expires_at"};
   if (!purpose || !nonce || !expires)
      return -1;
   memset(nonce, 0, 32);
   *expires = 0;
   cJSON *j = parse_exact(raw, len);
   const cJSON *n = j ? cJSON_GetObjectItemCaseSensitive(j, "nonce") : NULL;
   const cJSON *p = j ? cJSON_GetObjectItemCaseSensitive(j, "purpose") : NULL;
   const cJSON *e = j ? cJSON_GetObjectItemCaseSensitive(j, "expires_at") : NULL;
   const char *ns = cJSON_IsString(n) ? cJSON_GetStringValue(n) : NULL;
   int rc = exact_object(j, names, 3) && ns && cJSON_IsString(p) && cJSON_IsNumber(e) &&
                    !strcmp(cJSON_GetStringValue(p), purpose) && e->valuedouble >= 0 &&
                    e->valuedouble <= (double)UINT64_MAX &&
                    (double)(uint64_t)e->valuedouble == e->valuedouble &&
                    nonce_decode(ns, nonce) == 0
                ? 0
                : -1;
   if (!rc)
      *expires = (uint64_t)e->valuedouble;
   cJSON_Delete(j);
   if (rc)
   {
      OPENSSL_cleanse(nonce, 32);
      *expires = 0;
   }
   return rc;
}

int kb_management_health_response_decode(const char *raw, size_t len, const char *target)
{
   static const char *const names[] = {"status", "server_id"};
   cJSON *j = parse_exact(raw, len);
   const cJSON *s = j ? cJSON_GetObjectItemCaseSensitive(j, "status") : NULL;
   const cJSON *id = j ? cJSON_GetObjectItemCaseSensitive(j, "server_id") : NULL;
   int rc = exact_object(j, names, 2) && cJSON_IsString(s) && cJSON_IsString(id) && target &&
                    strcmp(cJSON_GetStringValue(s), "ok") == 0 &&
                    strcmp(cJSON_GetStringValue(id), target) == 0
                ? 0
                : -1;
   cJSON_Delete(j);
   return rc;
}

static int snapshot_valid(const db2_server_snapshot_t *s, const char *target)
{
   size_t serial_len =
       s ? strnlen(s->management_serial_norm, sizeof(s->management_serial_norm)) : 0;
   return s && target && strcmp(s->server_id, target) == 0 && token(s->server_id, 127) &&
          kb_mgmt_endpoint_validate(s->endpoint) == 0 && strcmp(s->status, "active") == 0 &&
          strcmp(s->enrollment_state, "active") == 0 && !s->revoked_at[0] &&
          s->management_issuer[0] && serial_len >= 1 && serial_len <= 128 &&
          lower_hex(s->management_serial_norm, serial_len) &&
          lower_hex(s->management_fingerprint, 64) && s->revocation_generation >= 1;
}

static int snapshot_equal(const db2_server_snapshot_t *a, const db2_server_snapshot_t *b)
{
   return !strcmp(a->server_id, b->server_id) && !strcmp(a->endpoint, b->endpoint) &&
          !strcmp(a->management_issuer, b->management_issuer) &&
          !strcmp(a->management_serial_norm, b->management_serial_norm) &&
          CRYPTO_memcmp(a->management_fingerprint, b->management_fingerprint, 64) == 0;
}

static void fp_hex(const unsigned char in[32], char out[65])
{
   static const char h[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; i++)
   {
      out[i * 2] = h[in[i] >> 4];
      out[i * 2 + 1] = h[in[i] & 15];
   }
   out[64] = 0;
}

static kb_management_health_result_t status_check(const kb_mgmt_status_t *s,
                                                  const kb_management_cert_active_t *active,
                                                  const db2_server_snapshot_t *snap,
                                                  const unsigned char nonce[32], uint64_t now,
                                                  const kb_management_health_dependencies_t *d)
{
   char fp[65];
   fp_hex(active->fingerprint, fp);
   if (strcmp(s->key_id, d->status_key_id) || CRYPTO_memcmp(s->nonce, nonce, 32) ||
       strcmp(s->caller_issuer, active->issuer) ||
       strcmp(s->caller_serial_norm, active->serial_norm) ||
       CRYPTO_memcmp(s->caller_fingerprint, fp, 64) ||
       strcmp(s->target_server_id, snap->server_id) ||
       CRYPTO_memcmp(s->target_mgmt_fingerprint, snap->management_fingerprint, 64) ||
       strcmp(s->purpose, HEALTH_PURPOSE) ||
       kb_mgmt_status_validate(s, now, (uint64_t)snap->revocation_generation) ||
       kb_mgmt_status_verify_signature(s, d->status_public_key))
      return KB_MANAGEMENT_HEALTH_INTEGRITY;
   return KB_MANAGEMENT_HEALTH_OK;
}

kb_management_health_result_t
kb_management_health_exchange(const kb_management_health_request_t *r,
                              const kb_management_health_dependencies_t *d)
{
   kb_management_health_result_t rc = KB_MANAGEMENT_HEALTH_INVALID;
   db2_server_snapshot_t a = {0}, b = {0};
   kb_management_cert_bundle_t bundle = {0};
   kb_management_cert_active_t active = {0};
   unsigned char nonce[32] = {0};
   char challenge[1024] = {0}, request[1024] = {0}, staple[KB_MGMT_STATUS_JSON_MAX + 1] = {0};
   char health[512] = {0}, headers[KB_MGMT_STATUS_JSON_MAX + 64] = {0};
   void *session = NULL;
   int status = 0, loaded = 0;
   if (!r || !d || !r->actor || !token(r->server_id, 127) || !r->deadline_millis || !d->snapshot ||
       !d->bundle_load || !d->bundle_clear || !d->server_open || !d->server_request ||
       !d->server_close || !d->authority_issue || !d->wall_seconds || !d->monotonic_millis ||
       !token(d->status_key_id, 64) || !d->status_public_key ||
       d->monotonic_millis(d->clock_ctx) >= r->deadline_millis)
      goto done;
   if ((rc = d->snapshot(d->snapshot_ctx, r->actor, r->team_id, r->server_id, &a)) !=
       KB_MANAGEMENT_HEALTH_OK)
      goto done;
   if (strcmp(a.status, "active") || strcmp(a.enrollment_state, "active") || a.revoked_at[0])
   {
      rc = KB_MANAGEMENT_HEALTH_DENIED;
      goto done;
   }
   if (!snapshot_valid(&a, r->server_id))
   {
      rc = KB_MANAGEMENT_HEALTH_INTEGRITY;
      goto done;
   }
   if ((rc = d->bundle_load(d->bundle_ctx, &bundle, &active)) != KB_MANAGEMENT_HEALTH_OK)
      goto done;
   loaded = 1;
   uint64_t now = d->wall_seconds(d->clock_ctx);
   if (now > INT64_MAX || active.not_before_epoch < 1 || active.not_before_epoch > (int64_t)now ||
       active.not_after_epoch <= (int64_t)now || active.revocation_generation < 1 ||
       !active.issuer[0] || !active.serial_norm[0])
   {
      rc = KB_MANAGEMENT_HEALTH_DENIED;
      goto done;
   }
   if ((rc = d->server_open(d->server_ctx, &a, &bundle, r->deadline_millis, &session)) !=
       KB_MANAGEMENT_HEALTH_OK)
      goto done;
   if ((rc = d->server_request(d->server_ctx, session, "POST", "/v1/management/challenge", "", NULL,
                               r->deadline_millis, challenge, sizeof(challenge), &status)) !=
           KB_MANAGEMENT_HEALTH_OK ||
       status != 200)
   {
      if (rc == KB_MANAGEMENT_HEALTH_OK)
         rc = status == 403 || status == 401 ? KB_MANAGEMENT_HEALTH_DENIED
                                             : KB_MANAGEMENT_HEALTH_UNAVAILABLE;
      goto done;
   }
   uint64_t expires = 0;
   now = d->wall_seconds(d->clock_ctx);
   if (kb_management_health_challenge_decode(challenge, strlen(challenge), nonce, &expires) ||
       expires <= now || expires - now > 15)
   {
      rc = KB_MANAGEMENT_HEALTH_INTEGRITY;
      goto done;
   }
   char encoded[44];
   nonce_encode(nonce, encoded);
   int n = snprintf(request, sizeof(request),
                    "{\"nonce\":\"%s\",\"target\":\"%s\",\"target_mgmt_fp\":\"%s\","
                    "\"purpose\":\"%s\"}",
                    encoded, a.server_id, a.management_fingerprint, HEALTH_PURPOSE);
   if (n < 0 || (size_t)n >= sizeof(request))
   {
      rc = KB_MANAGEMENT_HEALTH_INVALID;
      goto done;
   }
   uint64_t mono = d->monotonic_millis(d->clock_ctx);
   if (mono >= r->deadline_millis)
   {
      rc = KB_MANAGEMENT_HEALTH_UNAVAILABLE;
      goto done;
   }
   uint64_t expiry_budget =
       expires - now > (UINT64_MAX - mono) / 1000 ? UINT64_MAX : mono + (expires - now) * 1000;
   uint64_t protocol_deadline =
       expiry_budget < r->deadline_millis ? expiry_budget : r->deadline_millis;
   if ((rc = d->authority_issue(d->authority_ctx, &bundle, request, (size_t)n, protocol_deadline,
                                staple, sizeof(staple), &status)) != KB_MANAGEMENT_HEALTH_OK)
      goto done;
   d->bundle_clear(d->bundle_ctx, &bundle);
   loaded = 0;
   if (status != 200)
   {
      rc = status == 400   ? KB_MANAGEMENT_HEALTH_INVALID
           : status == 403 ? KB_MANAGEMENT_HEALTH_DENIED
           : status == 409 ? KB_MANAGEMENT_HEALTH_CONFLICT
                           : KB_MANAGEMENT_HEALTH_UNAVAILABLE;
      goto done;
   }
   kb_mgmt_status_t parsed;
   now = d->wall_seconds(d->clock_ctx);
   if (kb_mgmt_status_from_json(staple, &parsed) ||
       (rc = status_check(&parsed, &active, &a, nonce, now, d)) != KB_MANAGEMENT_HEALTH_OK)
   {
      rc = KB_MANAGEMENT_HEALTH_INTEGRITY;
      OPENSSL_cleanse(&parsed, sizeof(parsed));
      goto done;
   }
   OPENSSL_cleanse(&parsed, sizeof(parsed));
   now = d->wall_seconds(d->clock_ctx);
   if (now > expires || d->monotonic_millis(d->clock_ctx) >= protocol_deadline)
   {
      rc = KB_MANAGEMENT_HEALTH_CONFLICT;
      goto done;
   }
   if ((rc = d->snapshot(d->snapshot_ctx, r->actor, r->team_id, r->server_id, &b)) !=
       KB_MANAGEMENT_HEALTH_OK)
      goto done;
   if (!snapshot_valid(&b, r->server_id) || !snapshot_equal(&a, &b))
   {
      rc = KB_MANAGEMENT_HEALTH_CONFLICT;
      goto done;
   }
   if (b.revocation_generation < a.revocation_generation)
   {
      rc = KB_MANAGEMENT_HEALTH_INTEGRITY;
      goto done;
   }
   n = snprintf(headers, sizeof(headers), "X-Aimee-Management-Status: %s\r\n", staple);
   if (n < 0 || (size_t)n >= sizeof(headers) ||
       (rc = d->server_request(d->server_ctx, session, "GET", "/v1/management/health", "", headers,
                               protocol_deadline, health, sizeof(health), &status)) !=
           KB_MANAGEMENT_HEALTH_OK)
      goto done;
   rc = status == 200 &&
                kb_management_health_response_decode(health, strlen(health), r->server_id) == 0
            ? KB_MANAGEMENT_HEALTH_OK
            : (status == 409                    ? KB_MANAGEMENT_HEALTH_CONFLICT
               : status == 503                  ? KB_MANAGEMENT_HEALTH_UNAVAILABLE
               : status == 401 || status == 403 ? KB_MANAGEMENT_HEALTH_DENIED
                                                : KB_MANAGEMENT_HEALTH_INTEGRITY);
done:
   if (session)
      d->server_close(d->server_ctx, session);
   if (loaded)
      d->bundle_clear(d->bundle_ctx, &bundle);
   OPENSSL_cleanse(&bundle, sizeof(bundle));
   OPENSSL_cleanse(&active, sizeof(active));
   OPENSSL_cleanse(nonce, sizeof(nonce));
   OPENSSL_cleanse(challenge, sizeof(challenge));
   OPENSSL_cleanse(request, sizeof(request));
   OPENSSL_cleanse(staple, sizeof(staple));
   OPENSSL_cleanse(health, sizeof(health));
   OPENSSL_cleanse(headers, sizeof(headers));
   return rc;
}

kb_management_health_result_t kb_management_health_snapshot_primary(void *unused,
                                                                    const kb_principal_t *actor,
                                                                    int64_t team, const char *id,
                                                                    db2_server_snapshot_t *out)
{
   (void)unused;
   if (!actor || !id || !out)
      return KB_MANAGEMENT_HEALTH_INVALID;
   int rc = db2_tenant_scope_begin(actor, team);
   if (rc == DB2_ERR_TENANT_DENIED || rc == DB2_ERR_TENANT_UNAUTHENTICATED)
      return KB_MANAGEMENT_HEALTH_DENIED;
   if (rc)
      return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
   rc = db2_server_registry_snapshot(team, id, out);
   if (rc == 0 && db2_tenant_scope_commit() == 0)
      return KB_MANAGEMENT_HEALTH_OK;
   db2_tenant_scope_rollback();
   return rc == 1 ? KB_MANAGEMENT_HEALTH_NOT_FOUND : KB_MANAGEMENT_HEALTH_UNAVAILABLE;
}

kb_management_health_result_t
kb_management_health_bundle_active(void *ctx, kb_management_cert_bundle_t *bundle,
                                   kb_management_cert_active_t *active)
{
   kb_management_cert_result_t rc = kb_management_cert_load_active(ctx, bundle, active);
   switch (rc)
   {
   case KB_MANAGEMENT_CERT_OK:
      return KB_MANAGEMENT_HEALTH_OK;
   case KB_MANAGEMENT_CERT_DENIED:
   case KB_MANAGEMENT_CERT_DISABLED:
      return KB_MANAGEMENT_HEALTH_DENIED;
   case KB_MANAGEMENT_CERT_CONFLICT:
      return KB_MANAGEMENT_HEALTH_CONFLICT;
   case KB_MANAGEMENT_CERT_INTEGRITY:
      return KB_MANAGEMENT_HEALTH_INTEGRITY;
   case KB_MANAGEMENT_CERT_INVALID:
      return KB_MANAGEMENT_HEALTH_INVALID;
   default:
      return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
   }
}

void kb_management_health_bundle_cleanse(void *unused, kb_management_cert_bundle_t *bundle)
{
   (void)unused;
   kb_management_cert_bundle_clear(bundle);
}

kb_management_health_result_t
kb_management_health_server_open_production(void *ctx, const db2_server_snapshot_t *snapshot,
                                            const kb_management_cert_bundle_t *bundle,
                                            uint64_t deadline, void **out)
{
   const kb_management_health_server_config_t *config = ctx;
   if (!config || !config->server_ca_pem || !snapshot || !bundle || !out || !bundle->leaf_pem_len ||
       !bundle->key_pem_len)
      return KB_MANAGEMENT_HEALTH_INVALID;
   kb_mgmt_client_session_t *session = calloc(1, sizeof(*session));
   if (!session)
      return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
   if (kb_mgmt_client_session_open_deadline(
           session, snapshot->endpoint, config->server_ca_pem, bundle->leaf_pem, bundle->key_pem,
           snapshot->management_issuer, snapshot->management_serial_norm,
           snapshot->management_fingerprint, deadline, 0))
   {
      free(session);
      return KB_MANAGEMENT_HEALTH_UNAVAILABLE;
   }
   *out = session;
   return KB_MANAGEMENT_HEALTH_OK;
}

kb_management_health_result_t kb_management_health_server_request_production(
    void *unused, void *opaque, const char *method, const char *path, const char *body,
    const char *headers, uint64_t deadline, char *response, size_t cap, int *status)
{
   (void)unused;
   return kb_mgmt_client_session_request_deadline(opaque, method, path, body, headers, deadline,
                                                  response, cap, status) == 0
              ? KB_MANAGEMENT_HEALTH_OK
              : KB_MANAGEMENT_HEALTH_UNAVAILABLE;
}

void kb_management_health_server_close_production(void *unused, void *opaque)
{
   (void)unused;
   kb_mgmt_client_session_t *session = opaque;
   kb_mgmt_client_session_close(session);
   free(session);
}
