#include "kb_mgmt_status_authority.h"
#include "cJSON.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int bounded(const char *s, size_t min, size_t max)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, max + 1);
   return n >= min && n <= max;
}

static int ascii_token(const char *s, size_t min, size_t max)
{
   if (!bounded(s, min, max))
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
      if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '.' || *p == '_' || *p == '-'))
         return 0;
   return 1;
}

static int lower_hex(const char *s, size_t min, size_t max)
{
   if (!bounded(s, min, max))
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
         return 0;
   return 1;
}

static int printable(const char *s, size_t min, size_t max)
{
   if (!bounded(s, min, max))
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
      if (*p < 0x20 || *p == 0x7f)
         return 0;
   return 1;
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

/* A 32-byte value has exactly 43 unpadded base64url characters.  Requiring
 * zero unused low bits makes the accepted spelling canonical. */
static int nonce_decode(const char *encoded, unsigned char out[KB_MGMT_STATUS_NONCE_LEN])
{
   if (!encoded || strlen(encoded) != 43)
      return -1;
   size_t o = 0;
   uint32_t acc = 0;
   unsigned bits = 0;
   for (size_t i = 0; i < 43; ++i)
   {
      int v = b64_value((unsigned char)encoded[i]);
      if (v < 0)
         return -1;
      acc = (acc << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         if (o >= KB_MGMT_STATUS_NONCE_LEN)
            return -1;
         out[o++] = (unsigned char)(acc >> bits);
         acc &= bits ? ((1u << bits) - 1u) : 0u;
      }
   }
   return o == KB_MGMT_STATUS_NONCE_LEN && bits == 2 && acc == 0 ? 0 : -1;
}

static int copy_field(char *out, size_t cap, const char *value)
{
   if (!out || !cap || !value)
      return -1;
   size_t n = strlen(value);
   if (n >= cap)
      return -1;
   memcpy(out, value, n + 1);
   return 0;
}

kb_mgmt_status_authority_result_t kb_mgmt_status_request_from_json(const char *raw, size_t raw_len,
                                                                   kb_mgmt_status_request_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!raw || !out || raw_len < 2 || raw_len > KB_MGMT_STATUS_REQUEST_JSON_MAX || raw[0] != '{' ||
       raw[raw_len - 1] != '}' || memchr(raw, '\0', raw_len))
      return KB_MGMT_STATUS_AUTHORITY_INVALID;

   /* cJSON exposes decoded strings as NUL-terminated values without their
    * decoded length.  Reject the sole JSON spelling of an embedded NUL before
    * parsing so a validated prefix cannot hide a suffix. */
   for (size_t i = 0; i + 5 < raw_len; ++i)
      if (raw[i] == '\\' && raw[i + 1] == 'u' && raw[i + 2] == '0' && raw[i + 3] == '0' &&
          raw[i + 4] == '0' && raw[i + 5] == '0')
         return KB_MGMT_STATUS_AUTHORITY_INVALID;

   char copy[KB_MGMT_STATUS_REQUEST_JSON_MAX + 1];
   memcpy(copy, raw, raw_len);
   copy[raw_len] = '\0';
   const char *end = NULL;
   cJSON *object = cJSON_ParseWithLengthOpts(copy, raw_len + 1, &end, 1);
   static const char *names[] = {"nonce", "target", "target_mgmt_fp", "purpose"};
   const char *values[4] = {0};
   unsigned seen = 0;
   size_t count = 0;
   if (!cJSON_IsObject(object) || end != copy + raw_len)
      goto invalid;
   for (const cJSON *item = object->child; item; item = item->next)
   {
      int field = -1;
      for (int i = 0; i < 4; ++i)
         if (item->string && strcmp(item->string, names[i]) == 0)
            field = i;
      if (field < 0 || (seen & (1u << (unsigned)field)) || !cJSON_IsString(item))
         goto invalid;
      values[field] = cJSON_GetStringValue(item);
      seen |= 1u << (unsigned)field;
      ++count;
   }
   if (count != 4 || seen != 0x0fu || nonce_decode(values[0], out->nonce) != 0 ||
       !ascii_token(values[1], 1, 127) || !lower_hex(values[2], 64, 64) ||
       strcmp(values[3], "management.health.v1") != 0 ||
       copy_field(out->target_server_id, sizeof(out->target_server_id), values[1]) != 0 ||
       copy_field(out->target_mgmt_fingerprint, sizeof(out->target_mgmt_fingerprint), values[2]) !=
           0 ||
       copy_field(out->purpose, sizeof(out->purpose), values[3]) != 0)
      goto invalid;
   cJSON_Delete(object);
   return KB_MGMT_STATUS_AUTHORITY_OK;

invalid:
   cJSON_Delete(object);
   memset(out, 0, sizeof(*out));
   return KB_MGMT_STATUS_AUTHORITY_INVALID;
}

static kb_mgmt_status_authority_result_t callback_result(int rc, int lookup)
{
   if (rc == KB_MGMT_STATUS_CALLBACK_CONFLICT)
      return KB_MGMT_STATUS_AUTHORITY_CONFLICT;
   if (lookup && rc == KB_MGMT_STATUS_CALLBACK_DENIED)
      return KB_MGMT_STATUS_AUTHORITY_DENIED;
   if (rc == KB_MGMT_STATUS_CALLBACK_INTEGRITY)
      return KB_MGMT_STATUS_AUTHORITY_INTEGRITY;
   return KB_MGMT_STATUS_AUTHORITY_UNAVAILABLE;
}

kb_mgmt_status_authority_result_t
kb_mgmt_status_authority_issue(const kb_mgmt_status_request_t *r, const char *issuer,
                               const char *serial, const char *fingerprint, const char *key_id,
                               uint64_t now, kb_mgmt_status_lookup_fn lookup, void *lookup_ctx,
                               kb_mgmt_status_sign_fn sign, void *sign_ctx, kb_mgmt_status_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!r || !issuer || !serial || !fingerprint || !key_id || !lookup || !sign || !out ||
       !ascii_token(r->target_server_id, 1, 127) ||
       !lower_hex(r->target_mgmt_fingerprint, 64, 64) ||
       strcmp(r->purpose, "management.health.v1") != 0 || !printable(issuer, 1, 600) ||
       !lower_hex(serial, 1, 128) || !lower_hex(fingerprint, 64, 64) ||
       !ascii_token(key_id, 1, 64) || now == 0 || now > UINT64_MAX - 10)
      return KB_MGMT_STATUS_AUTHORITY_INVALID;
   int64_t generation = 0;
   char authoritative_fp[65] = "";
   int lookup_rc = lookup(issuer, serial, fingerprint, r->target_server_id, r->purpose, &generation,
                          authoritative_fp, sizeof(authoritative_fp), lookup_ctx);
   if (lookup_rc != KB_MGMT_STATUS_CALLBACK_OK)
      return callback_result(lookup_rc, 1);
   if (generation < 1 || !lower_hex(authoritative_fp, 64, 64))
      return KB_MGMT_STATUS_AUTHORITY_INTEGRITY;
   if (CRYPTO_memcmp(authoritative_fp, r->target_mgmt_fingerprint, 64) != 0)
      return KB_MGMT_STATUS_AUTHORITY_DENIED;
   out->version = 1;
   memcpy(out->nonce, r->nonce, sizeof(out->nonce));
   snprintf(out->key_id, sizeof(out->key_id), "%s", key_id);
   snprintf(out->caller_issuer, sizeof(out->caller_issuer), "%s", issuer);
   snprintf(out->caller_serial_norm, sizeof(out->caller_serial_norm), "%s", serial);
   snprintf(out->caller_fingerprint, sizeof(out->caller_fingerprint), "%s", fingerprint);
   snprintf(out->target_server_id, sizeof(out->target_server_id), "%s", r->target_server_id);
   snprintf(out->target_mgmt_fingerprint, sizeof(out->target_mgmt_fingerprint), "%s",
            authoritative_fp);
   snprintf(out->purpose, sizeof(out->purpose), "%s", r->purpose);
   out->issued_at = now;
   out->expires_at = now + 10;
   out->revocation_generation = (uint64_t)generation;
   int sign_rc = sign(out, sign_ctx);
   if (sign_rc != KB_MGMT_STATUS_CALLBACK_OK)
   {
      OPENSSL_cleanse(out, sizeof(*out));
      return callback_result(sign_rc, 0);
   }
   return KB_MGMT_STATUS_AUTHORITY_OK;
}

static int uint64_string(const char *s, uint64_t *out)
{
   if (!s || !*s || (s[0] == '0' && s[1]) || !out)
      return -1;
   uint64_t value = 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
   {
      if (*p < '0' || *p > '9' || value > (UINT64_MAX - (uint64_t)(*p - '0')) / 10)
         return -1;
      value = value * 10 + (uint64_t)(*p - '0');
   }
   *out = value;
   return 0;
}

static int b64url_decode(const char *in, unsigned char *out, size_t cap, size_t *out_len)
{
   if (!in || !out || !out_len || strlen(in) % 4 == 1)
      return -1;
   uint32_t acc = 0;
   unsigned bits = 0;
   size_t used = 0;
   for (const unsigned char *p = (const unsigned char *)in; *p; ++p)
   {
      int v = b64_value(*p);
      if (v < 0)
         return -1;
      acc = (acc << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         if (used == cap)
            return -1;
         out[used++] = (unsigned char)(acc >> bits);
         acc &= bits ? (1u << bits) - 1u : 0;
      }
   }
   if ((bits && acc) || !used)
      return -1;
   *out_len = used;
   return 0;
}

static void digest_hex(const unsigned char digest[32], char out[65])
{
   static const char hex[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; ++i)
   {
      out[i * 2] = hex[digest[i] >> 4];
      out[i * 2 + 1] = hex[digest[i] & 15];
   }
   out[64] = 0;
}

kb_mgmt_status_authority_result_t
kb_mgmt_checkpoint_request_from_json(const char *raw, size_t raw_len,
                                     kb_mgmt_checkpoint_request_t *out)
{
   static const char *names[] = {"version",       "purpose",
                                 "nonce",         "caller_issuer_b64",
                                 "caller_serial", "caller_fingerprint",
                                 "target",        "staple_generation",
                                 "staple_sha256", "correlation_id",
                                 "jti",           "request_sha256"};
   if (out)
      memset(out, 0, sizeof(*out));
   if (!raw || !out || raw_len < 2 || raw_len > KB_MGMT_CHECKPOINT_JSON_MAX ||
       memchr(raw, 0, raw_len))
      return KB_MGMT_STATUS_AUTHORITY_INVALID;
   char copy[KB_MGMT_CHECKPOINT_JSON_MAX + 1];
   memcpy(copy, raw, raw_len);
   copy[raw_len] = 0;
   const char *end = NULL;
   cJSON *j = cJSON_ParseWithLengthOpts(copy, raw_len + 1, &end, 1);
   const cJSON *p = cJSON_IsObject(j) ? j->child : NULL;
   const char *v[12] = {0};
   for (size_t i = 0; i < 12; ++i)
   {
      if (!p || !p->string || strcmp(p->string, names[i]) || !cJSON_IsString(p))
         goto bad_checkpoint;
      v[i] = cJSON_GetStringValue(p);
      p = p->next;
   }
   if (p || end != copy + raw_len || strcmp(v[0], "1") || strcmp(v[1], "management.action.v1") ||
       nonce_decode(v[2], out->nonce) || !lower_hex(v[4], 1, 128) || !lower_hex(v[5], 64, 64) ||
       !ascii_token(v[6], 1, 127) || uint64_string(v[7], &out->staple_generation) ||
       !lower_hex(v[8], 64, 64) || !lower_hex(v[9], 64, 64) || !lower_hex(v[10], 64, 64) ||
       !lower_hex(v[11], 64, 64))
      goto bad_checkpoint;
   unsigned char issuer[601];
   size_t issuer_len = 0;
   if (b64url_decode(v[3], issuer, sizeof(issuer) - 1, &issuer_len) ||
       memchr(issuer, 0, issuer_len))
      goto bad_checkpoint;
   issuer[issuer_len] = 0;
   if (!printable((const char *)issuer, 1, 600) ||
       copy_field(out->caller_issuer, sizeof(out->caller_issuer), (const char *)issuer) ||
       copy_field(out->caller_serial_norm, sizeof(out->caller_serial_norm), v[4]) ||
       copy_field(out->caller_fingerprint, sizeof(out->caller_fingerprint), v[5]) ||
       copy_field(out->target_server_id, sizeof(out->target_server_id), v[6]) ||
       copy_field(out->staple_sha256, sizeof(out->staple_sha256), v[8]) ||
       copy_field(out->correlation_id, sizeof(out->correlation_id), v[9]) ||
       copy_field(out->jti, sizeof(out->jti), v[10]) ||
       copy_field(out->action_request_sha256, sizeof(out->action_request_sha256), v[11]))
      goto bad_checkpoint;
   /* Exact raw spelling is part of the signed decision digest. */
   char canonical[KB_MGMT_CHECKPOINT_JSON_MAX + 1];
   int n = snprintf(canonical, sizeof(canonical),
                    "{\"version\":\"1\",\"purpose\":\"management.action.v1\",\"nonce\":\"%s\","
                    "\"caller_issuer_b64\":\"%s\",\"caller_serial\":\"%s\","
                    "\"caller_fingerprint\":\"%s\",\"target\":\"%s\","
                    "\"staple_generation\":\"%s\",\"staple_sha256\":\"%s\","
                    "\"correlation_id\":\"%s\",\"jti\":\"%s\",\"request_sha256\":\"%s\"}",
                    v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
   if (n < 0 || (size_t)n != raw_len || CRYPTO_memcmp(canonical, raw, raw_len))
      goto bad_checkpoint;
   unsigned char digest[32];
   unsigned int digest_len = 0;
   if (EVP_Digest(raw, raw_len, digest, &digest_len, EVP_sha256(), NULL) != 1 || digest_len != 32)
      goto bad_checkpoint;
   digest_hex(digest, out->canonical_sha256);
   out->version = 1;
   cJSON_Delete(j);
   return KB_MGMT_STATUS_AUTHORITY_OK;
bad_checkpoint:
   cJSON_Delete(j);
   memset(out, 0, sizeof(*out));
   return KB_MGMT_STATUS_AUTHORITY_INVALID;
}

kb_mgmt_status_authority_result_t kb_mgmt_checkpoint_authority_issue(
    const kb_mgmt_checkpoint_request_t *r, const char *peer_issuer, const char *peer_serial,
    const char *peer_fingerprint, const char *key_id, uint64_t now,
    kb_mgmt_checkpoint_lookup_fn lookup, void *lookup_ctx, kb_mgmt_checkpoint_sign_fn sign,
    void *sign_ctx, kb_mgmt_checkpoint_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!r || r->version != 1 || !peer_issuer || !peer_serial || !peer_fingerprint || !key_id ||
       !lookup || !sign || !out || !printable(peer_issuer, 1, 600) ||
       !lower_hex(peer_serial, 1, 128) || !lower_hex(peer_fingerprint, 64, 64) ||
       !ascii_token(key_id, 1, 64) || !printable(r->caller_issuer, 1, 600) ||
       !lower_hex(r->caller_serial_norm, 1, 128) || !lower_hex(r->caller_fingerprint, 64, 64) ||
       !ascii_token(r->target_server_id, 1, 127) || r->staple_generation < 1 ||
       !lower_hex(r->staple_sha256, 64, 64) || !lower_hex(r->correlation_id, 64, 64) ||
       !lower_hex(r->jti, 64, 64) || !lower_hex(r->action_request_sha256, 64, 64) ||
       !lower_hex(r->canonical_sha256, 64, 64) || !now || now > UINT64_MAX - 5)
      return KB_MGMT_STATUS_AUTHORITY_INVALID;
   int revoked = 0;
   int64_t generation = 0;
   int rc =
       lookup(peer_issuer, peer_serial, peer_fingerprint, r, &revoked, &generation, lookup_ctx);
   if (rc != KB_MGMT_STATUS_CALLBACK_OK)
      return callback_result(rc, 1);
   if ((revoked != 0 && revoked != 1) || generation < 1)
      return KB_MGMT_STATUS_AUTHORITY_INTEGRITY;
   out->version = 1;
   snprintf(out->request_sha256, sizeof(out->request_sha256), "%s", r->canonical_sha256);
   out->revoked = revoked;
   out->generation = (uint64_t)generation;
   out->issued_at = now;
   out->expires_at = now + 5;
   snprintf(out->key_id, sizeof(out->key_id), "%s", key_id);
   kb_mgmt_checkpoint_request_t admitted_request = *r;
   snprintf(admitted_request.authenticated_peer_fingerprint,
            sizeof(admitted_request.authenticated_peer_fingerprint), "%s", peer_fingerprint);
   rc = sign(out, &admitted_request, sign_ctx);
   OPENSSL_cleanse(&admitted_request, sizeof(admitted_request));
   if (rc != KB_MGMT_STATUS_CALLBACK_OK)
   {
      memset(out, 0, sizeof(*out));
      return callback_result(rc, 0);
   }
   return KB_MGMT_STATUS_AUTHORITY_OK;
}
