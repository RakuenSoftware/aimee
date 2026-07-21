#include "kb_mgmt_status_authority.h"
#include "cJSON.h"

#include <openssl/crypto.h>
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
