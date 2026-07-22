#include "kb_mgmt_token_public.h"

#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

static void put_u32be(uint8_t out[4], uint32_t value)
{
   out[0] = (uint8_t)(value >> 24);
   out[1] = (uint8_t)(value >> 16);
   out[2] = (uint8_t)(value >> 8);
   out[3] = (uint8_t)value;
}

static void hex_encode(const uint8_t *value, size_t len, char *out)
{
   static const char hex[] = "0123456789abcdef";
   for (size_t i = 0; i < len; ++i)
   {
      out[i * 2] = hex[value[i] >> 4];
      out[i * 2 + 1] = hex[value[i] & 15];
   }
   out[len * 2] = 0;
}

static int b64url_encode(const uint8_t *value, size_t len, char *out, size_t cap, size_t *out_len)
{
   if (out && cap)
      OPENSSL_cleanse(out, cap);
   if (out_len)
      *out_len = 0;
   if (!value || !out || !out_len || len > (INT_MAX - 2) / 4 * 3)
      return -1;
   size_t padded = 4 * ((len + 2) / 3);
   size_t actual = padded;
   while (actual &&
          ((len % 3 == 1 && padded - actual < 2) || (len % 3 == 2 && padded - actual < 1)))
      --actual;
   if (actual + 1 > cap)
      return -1;
   unsigned char tmp[KB_MGMT_TOKEN_JWK_MAX];
   if (padded >= sizeof(tmp) || EVP_EncodeBlock(tmp, value, (int)len) != (int)padded)
      goto fail;
   for (size_t i = 0; i < actual; ++i)
      out[i] = tmp[i] == '+' ? '-' : (tmp[i] == '/' ? '_' : (char)tmp[i]);
   out[actual] = 0;
   *out_len = actual;
   OPENSSL_cleanse(tmp, sizeof(tmp));
   return 0;
fail:
   OPENSSL_cleanse(tmp, sizeof(tmp));
   OPENSSL_cleanse(out, cap);
   return -1;
}

static int b64url_decode(const char *value, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
   if (out && cap)
      OPENSSL_cleanse(out, cap);
   if (out_len)
      *out_len = 0;
   if (!value || !len || !out || !out_len || len % 4 == 1 || len > KB_MGMT_TOKEN_JWK_MAX - 4)
      return -1;
   size_t padded = (len + 3) & ~(size_t)3;
   unsigned char tmp[KB_MGMT_TOKEN_JWK_MAX] = {0};
   uint8_t decoded_bytes[KB_MGMT_TOKEN_JWK_MAX] = {0};
   for (size_t i = 0; i < len; ++i)
   {
      unsigned char c = (unsigned char)value[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_'))
         goto fail;
      tmp[i] = c == '-' ? '+' : (c == '_' ? '/' : c);
   }
   for (size_t i = len; i < padded; ++i)
      tmp[i] = '=';
   size_t decoded = padded / 4 * 3 - (padded - len);
   int raw_len = EVP_DecodeBlock(decoded_bytes, tmp, (int)padded);
   if (decoded > cap || raw_len < 0 || (size_t)raw_len != padded / 4 * 3)
      goto fail;
   memcpy(out, decoded_bytes, decoded);
   char canonical[KB_MGMT_TOKEN_JWK_MAX];
   size_t canonical_len = 0;
   int ok = !b64url_encode(out, decoded, canonical, sizeof(canonical), &canonical_len) &&
            canonical_len == len && !CRYPTO_memcmp(canonical, value, len);
   OPENSSL_cleanse(tmp, sizeof(tmp));
   OPENSSL_cleanse(decoded_bytes, sizeof(decoded_bytes));
   OPENSSL_cleanse(canonical, sizeof(canonical));
   if (!ok)
   {
      OPENSSL_cleanse(out, cap);
      return -1;
   }
   *out_len = decoded;
   return 0;
fail:
   OPENSSL_cleanse(tmp, sizeof(tmp));
   OPENSSL_cleanse(decoded_bytes, sizeof(decoded_bytes));
   OPENSSL_cleanse(out, cap);
   return -1;
}

int kb_mgmt_token_kid(const uint8_t *modulus, size_t modulus_len, char *out, size_t cap)
{
   static const char domain[] = "aimee.p5.token.public.v1\n";
   static const uint8_t exponent[] = {0x01, 0x00, 0x01};
   uint8_t digest[32], ml[4], el[4];
   if (out && cap)
      OPENSSL_cleanse(out, cap);
   if (!modulus || modulus_len != KB_MGMT_TOKEN_MODULUS_LEN || !modulus[0] || !out || cap < 45)
      return -1;
   put_u32be(ml, (uint32_t)modulus_len);
   put_u32be(el, sizeof(exponent));
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   unsigned int n = 0;
   int ok = md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
            EVP_DigestUpdate(md, domain, sizeof(domain) - 1) == 1 &&
            EVP_DigestUpdate(md, ml, sizeof(ml)) == 1 &&
            EVP_DigestUpdate(md, modulus, modulus_len) == 1 &&
            EVP_DigestUpdate(md, el, sizeof(el)) == 1 &&
            EVP_DigestUpdate(md, exponent, sizeof(exponent)) == 1 &&
            EVP_DigestFinal_ex(md, digest, &n) == 1 && n == sizeof(digest);
   EVP_MD_CTX_free(md);
   if (!ok)
   {
      OPENSSL_cleanse(digest, sizeof(digest));
      return -1;
   }
   memcpy(out, "p5-token-v1-", 12);
   hex_encode(digest, 16, out + 12);
   OPENSSL_cleanse(digest, sizeof(digest));
   return 0;
}

int kb_mgmt_token_jwk(const uint8_t *modulus, size_t modulus_len, char *out, size_t cap,
                      size_t *out_len)
{
   char kid[KB_MGMT_TOKEN_KID_MAX + 1], n[KB_MGMT_TOKEN_JWK_MAX];
   size_t n_len = 0;
   if (out && cap)
      OPENSSL_cleanse(out, cap);
   if (out_len)
      *out_len = 0;
   if (!out || !out_len || kb_mgmt_token_kid(modulus, modulus_len, kid, sizeof(kid)) ||
       b64url_encode(modulus, modulus_len, n, sizeof(n), &n_len))
      return -1;
   int written = snprintf(out, cap,
                          "{\"kty\":\"RSA\",\"kid\":\"%s\",\"use\":\"sig\","
                          "\"alg\":\"RS256\",\"n\":\"%s\",\"e\":\"AQAB\"}",
                          kid, n);
   OPENSSL_cleanse(n, sizeof(n));
   OPENSSL_cleanse(kid, sizeof(kid));
   if (written < 0 || (size_t)written >= cap)
   {
      OPENSSL_cleanse(out, cap);
      return -1;
   }
   *out_len = (size_t)written;
   return 0;
}

int kb_mgmt_token_jwk_validate(const char *jwk, size_t jwk_len, uint8_t *modulus,
                               size_t modulus_cap, size_t *modulus_len)
{
   static const char prefix[] = "{\"kty\":\"RSA\",\"kid\":\"";
   static const char middle[] = "\",\"use\":\"sig\",\"alg\":\"RS256\",\"n\":\"";
   static const char suffix[] = "\",\"e\":\"AQAB\"}";
   if (modulus && modulus_cap)
      OPENSSL_cleanse(modulus, modulus_cap);
   if (modulus_len)
      *modulus_len = 0;
   if (!jwk || !modulus || modulus_cap < KB_MGMT_TOKEN_MODULUS_LEN || !modulus_len ||
       jwk_len < sizeof(prefix) + sizeof(middle) + sizeof(suffix) ||
       jwk_len >= KB_MGMT_TOKEN_JWK_MAX)
      return -1;
   char bounded[KB_MGMT_TOKEN_JWK_MAX];
   memcpy(bounded, jwk, jwk_len);
   bounded[jwk_len] = 0;
   const char *p = bounded;
   if (memcmp(p, prefix, sizeof(prefix) - 1))
      return -1;
   p += sizeof(prefix) - 1;
   const char *m = strstr(p, middle);
   if (!m || (size_t)(m - p) > KB_MGMT_TOKEN_KID_MAX)
      return -1;
   size_t kid_len = (size_t)(m - p);
   p = m + sizeof(middle) - 1;
   const char *s = strstr(p, suffix);
   if (!s || s + sizeof(suffix) - 1 != bounded + jwk_len)
      return -1;
   if (b64url_decode(p, (size_t)(s - p), modulus, modulus_cap, modulus_len) ||
       *modulus_len != KB_MGMT_TOKEN_MODULUS_LEN || !modulus[0])
      goto fail;
   char kid[KB_MGMT_TOKEN_KID_MAX + 1];
   if (kb_mgmt_token_kid(modulus, *modulus_len, kid, sizeof(kid)) || strlen(kid) != kid_len ||
       CRYPTO_memcmp(kid, m - kid_len, kid_len))
   {
      OPENSSL_cleanse(kid, sizeof(kid));
      goto fail;
   }
   OPENSSL_cleanse(kid, sizeof(kid));
   return 0;
fail:
   OPENSSL_cleanse(modulus, modulus_cap);
   *modulus_len = 0;
   return -1;
}
