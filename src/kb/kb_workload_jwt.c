#include "kb_workload_jwt.h"

#include "aws_sts.h"
#include "cJSON.h"

#include <limits.h>
#include <math.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WORKLOAD_TOKEN_MAX 16384u
#define WORKLOAD_JWKS_MAX  65536u
#define WORKLOAD_TIME_MAX  9007199254740991ULL

static int printable_ascii(const char *s, size_t min, size_t max)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, max + 1);
   if (n < min || n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e)
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

static int b64_decode_canonical(const char *in, size_t n, unsigned char *out, size_t cap,
                                size_t *out_len)
{
   if (!in || !out || !out_len || !n || n % 4 == 1)
      return -1;
   size_t o = 0;
   uint32_t acc = 0;
   unsigned bits = 0;
   for (size_t i = 0; i < n; ++i)
   {
      int v = b64_value((unsigned char)in[i]);
      if (v < 0)
         return -1;
      acc = (acc << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         if (o >= cap)
            return -1;
         out[o++] = (unsigned char)(acc >> bits);
         acc &= bits ? ((1u << bits) - 1u) : 0u;
      }
   }
   if (acc != 0)
      return -1;
   *out_len = o;
   return 0;
}

static void json_cleanse(cJSON *item)
{
   for (cJSON *p = item; p; p = p->next)
   {
      if (p->child)
         json_cleanse(p->child);
      if (p->valuestring)
         OPENSSL_cleanse(p->valuestring, strlen(p->valuestring));
      if (p->string)
         OPENSSL_cleanse(p->string, strlen(p->string));
   }
}

static int has_decoded_nul_escape(const unsigned char *raw, size_t n)
{
   int in_string = 0;
   for (size_t i = 0; i < n; ++i)
   {
      if (!in_string)
      {
         if (raw[i] == '"')
            in_string = 1;
         continue;
      }
      if (raw[i] == '"')
      {
         in_string = 0;
         continue;
      }
      if (raw[i] != '\\' || i + 1 >= n)
         continue;
      if (raw[i + 1] == 'u' && i + 5 < n && raw[i + 2] == '0' && raw[i + 3] == '0' &&
          raw[i + 4] == '0' && raw[i + 5] == '0')
         return 1;
      ++i; /* The escaped byte cannot itself begin another escape. */
   }
   return 0;
}

static int exact_u64(const cJSON *item, uint64_t *out)
{
   if (!cJSON_IsNumber(item) || !out || !isfinite(item->valuedouble) || item->valuedouble < 0.0 ||
       item->valuedouble > (double)WORKLOAD_TIME_MAX ||
       floor(item->valuedouble) != item->valuedouble)
      return -1;
   *out = (uint64_t)item->valuedouble;
   return 0;
}

static int audience_exact(const cJSON *item, const char *expected)
{
   if (cJSON_IsString(item))
      return strcmp(cJSON_GetStringValue(item), expected) == 0;
   if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 1)
      return 0;
   const cJSON *only = cJSON_GetArrayItem(item, 0);
   return cJSON_IsString(only) && strcmp(cJSON_GetStringValue(only), expected) == 0;
}

static int strict_claims(const unsigned char *payload, size_t payload_len,
                         const char *expected_issuer, const char *expected_audience, uint64_t now,
                         uint32_t max_age, kb_workload_identity_t *candidate)
{
   if (!payload || !payload_len || payload[0] != '{' || payload[payload_len - 1] != '}' ||
       memchr(payload, '\0', payload_len) || has_decoded_nul_escape(payload, payload_len))
      return -1;

   const char *end = NULL;
   cJSON *root = cJSON_ParseWithLengthOpts((const char *)payload, payload_len + 1, &end, 1);
   int ok = 0;
   if (!cJSON_IsObject(root) || end != (const char *)payload + payload_len)
      goto done;

   enum
   {
      CLAIM_ISS,
      CLAIM_SUB,
      CLAIM_AUD,
      CLAIM_IAT,
      CLAIM_EXP,
      CLAIM_NBF,
      CLAIM_COUNT
   };
   static const char *names[CLAIM_COUNT] = {"iss", "sub", "aud", "iat", "exp", "nbf"};
   const cJSON *values[CLAIM_COUNT] = {0};
   unsigned seen = 0;
   for (const cJSON *p = root->child; p; p = p->next)
      for (unsigned i = 0; i < CLAIM_COUNT; ++i)
         if (p->string && strcmp(p->string, names[i]) == 0)
         {
            if (seen & (1u << i))
               goto done;
            seen |= 1u << i;
            values[i] = p;
         }
   const unsigned required = (1u << CLAIM_ISS) | (1u << CLAIM_SUB) | (1u << CLAIM_AUD) |
                             (1u << CLAIM_IAT) | (1u << CLAIM_EXP);
   if ((seen & required) != required || !cJSON_IsString(values[CLAIM_ISS]) ||
       !cJSON_IsString(values[CLAIM_SUB]) ||
       !printable_ascii(cJSON_GetStringValue(values[CLAIM_ISS]), 1, 600) ||
       !printable_ascii(cJSON_GetStringValue(values[CLAIM_SUB]), 1, 600) ||
       strcmp(cJSON_GetStringValue(values[CLAIM_ISS]), expected_issuer) != 0 ||
       !audience_exact(values[CLAIM_AUD], expected_audience))
      goto done;

   uint64_t issued = 0, expires = 0, not_before = 0;
   if (exact_u64(values[CLAIM_IAT], &issued) != 0 || exact_u64(values[CLAIM_EXP], &expires) != 0 ||
       ((seen & (1u << CLAIM_NBF)) && exact_u64(values[CLAIM_NBF], &not_before) != 0))
      goto done;
   if (issued > now + 2 || (seen & (1u << CLAIM_NBF) && not_before > now + 2) || expires < issued ||
       expires - issued > max_age || (issued <= now && now - issued > max_age) || expires < now ||
       expires - now < 30)
      goto done;

   memcpy(candidate->issuer, cJSON_GetStringValue(values[CLAIM_ISS]),
          strlen(cJSON_GetStringValue(values[CLAIM_ISS])) + 1);
   memcpy(candidate->subject, cJSON_GetStringValue(values[CLAIM_SUB]),
          strlen(cJSON_GetStringValue(values[CLAIM_SUB])) + 1);
   candidate->issued_at = issued;
   candidate->expires_at = expires;
   ok = 1;
done:
   json_cleanse(root);
   cJSON_Delete(root);
   return ok ? 0 : -1;
}

kb_workload_result_t kb_workload_jwt_validate_ex(const void *token_raw, size_t token_len,
                                                 const void *jwks_raw, size_t jwks_len,
                                                 const char *expected_issuer,
                                                 const char *expected_audience, uint64_t now,
                                                 uint32_t max_token_age_seconds,
                                                 kb_workload_identity_t *out, int *reload_jwks)
{
   if (reload_jwks)
      *reload_jwks = 0;
   if (out)
      memset(out, 0, sizeof(*out));
   if (!token_raw || !token_len || token_len > WORKLOAD_TOKEN_MAX || !jwks_raw || !jwks_len ||
       jwks_len > WORKLOAD_JWKS_MAX || !out || max_token_age_seconds == 0 ||
       max_token_age_seconds > 300 || now > WORKLOAD_TIME_MAX - 302 || now > LONG_MAX ||
       !printable_ascii(expected_issuer, 1, 600) || !printable_ascii(expected_audience, 1, 600) ||
       memchr(token_raw, '\0', token_len) || memchr(jwks_raw, '\0', jwks_len))
      return KB_WORKLOAD_INVALID;

   char *token = malloc(token_len + 1);
   char *jwks = malloc(jwks_len + 1);
   unsigned char *payload = NULL;
   kb_workload_identity_t candidate;
   aws_webid_claims_t ignored;
   memset(&candidate, 0, sizeof(candidate));
   memset(&ignored, 0, sizeof(ignored));
   kb_workload_result_t result = KB_WORKLOAD_INTEGRITY;
   if (!token || !jwks)
   {
      result = KB_WORKLOAD_UNAVAILABLE;
      goto done;
   }
   memcpy(token, token_raw, token_len);
   token[token_len] = '\0';
   memcpy(jwks, jwks_raw, jwks_len);
   jwks[jwks_len] = '\0';

   /* The common verifier selects the JWKS key and authenticates the compact JWS
    * before the workload-specific parser reparses any claims. Only an unknown kid
    * is a rotation signal; every other verification failure is final. */
   aws_webid_status_t webid = aws_webidentity_validate(token, jwks, expected_issuer,
                                                       expected_audience, (long)now, &ignored);
   if (webid != AWS_WEBID_OK)
   {
      if (reload_jwks && webid == AWS_WEBID_ERR_NO_KEY)
         *reload_jwks = 1;
      goto done;
   }

   payload = malloc(token_len + 1);
   if (!payload)
   {
      result = KB_WORKLOAD_UNAVAILABLE;
      goto done;
   }

   const char *first = memchr(token, '.', token_len);
   const char *second =
       first ? memchr(first + 1, '.', token_len - (size_t)(first + 1 - token)) : NULL;
   if (!first || !second || memchr(second + 1, '.', token_len - (size_t)(second + 1 - token)))
      goto done;
   size_t payload_len = 0;
   if (b64_decode_canonical(first + 1, (size_t)(second - first - 1), payload, token_len,
                            &payload_len) != 0)
      goto done;
   payload[payload_len] = '\0';
   if (strict_claims(payload, payload_len, expected_issuer, expected_audience, now,
                     max_token_age_seconds, &candidate) != 0)
      goto done;
   unsigned int digest_len = 0;
   if (EVP_Digest(token_raw, token_len, candidate.token_hash, &digest_len, EVP_sha256(), NULL) !=
           1 ||
       digest_len != sizeof(candidate.token_hash))
   {
      result = KB_WORKLOAD_UNAVAILABLE;
      goto done;
   }
   *out = candidate;
   result = KB_WORKLOAD_OK;

done:
   if (result != KB_WORKLOAD_OK)
      memset(out, 0, sizeof(*out));
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   OPENSSL_cleanse(&ignored, sizeof(ignored));
   if (payload)
   {
      OPENSSL_cleanse(payload, token_len + 1);
      free(payload);
   }
   if (token)
   {
      OPENSSL_cleanse(token, token_len + 1);
      free(token);
   }
   if (jwks)
   {
      OPENSSL_cleanse(jwks, jwks_len + 1);
      free(jwks);
   }
   return result;
}

kb_workload_result_t kb_workload_jwt_validate(const void *token_raw, size_t token_len,
                                              const void *jwks_raw, size_t jwks_len,
                                              const char *expected_issuer,
                                              const char *expected_audience, uint64_t now,
                                              uint32_t max_token_age_seconds,
                                              kb_workload_identity_t *out)
{
   return kb_workload_jwt_validate_ex(token_raw, token_len, jwks_raw, jwks_len, expected_issuer,
                                      expected_audience, now, max_token_age_seconds, out, NULL);
}
