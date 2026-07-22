#include "kb/kb_workload_jwt.h"

#include <assert.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *b64url(const unsigned char *in, size_t len)
{
   static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   char *out = malloc(((len + 2) / 3) * 4 + 1);
   assert(out);
   size_t o = 0;
   for (size_t i = 0; i < len; i += 3)
   {
      unsigned v = (unsigned)in[i] << 16;
      if (i + 1 < len)
         v |= (unsigned)in[i + 1] << 8;
      if (i + 2 < len)
         v |= in[i + 2];
      out[o++] = table[(v >> 18) & 63];
      out[o++] = table[(v >> 12) & 63];
      if (i + 1 < len)
         out[o++] = table[(v >> 6) & 63];
      if (i + 2 < len)
         out[o++] = table[v & 63];
   }
   out[o] = '\0';
   return out;
}

static EVP_PKEY *key_generate(void)
{
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
   EVP_PKEY *key = NULL;
   assert(ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0 &&
          EVP_PKEY_keygen(ctx, &key) > 0);
   EVP_PKEY_CTX_free(ctx);
   return key;
}

static char *jwks_make(EVP_PKEY *key)
{
   BIGNUM *n = NULL, *e = NULL;
   assert(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &n) == 1);
   assert(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &e) == 1);
   unsigned char nb[512], eb[16];
   int nl = BN_bn2bin(n, nb), el = BN_bn2bin(e, eb);
   BN_free(n);
   BN_free(e);
   char *ns = b64url(nb, (size_t)nl), *es = b64url(eb, (size_t)el);
   char *out = malloc(strlen(ns) + strlen(es) + 128);
   assert(out);
   sprintf(out, "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"k1\",\"n\":\"%s\",\"e\":\"%s\"}]}", ns, es);
   free(ns);
   free(es);
   return out;
}

static char *jwt_make(const char *payload, EVP_PKEY *key)
{
   const char *header = "{\"alg\":\"RS256\",\"kid\":\"k1\"}";
   char *h = b64url((const unsigned char *)header, strlen(header));
   char *p = b64url((const unsigned char *)payload, strlen(payload));
   char *signed_input = malloc(strlen(h) + strlen(p) + 2);
   assert(signed_input);
   sprintf(signed_input, "%s.%s", h, p);
   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   size_t sig_len = 0;
   assert(ctx && EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, key) == 1 &&
          EVP_DigestSign(ctx, NULL, &sig_len, (const unsigned char *)signed_input,
                         strlen(signed_input)) == 1);
   unsigned char *sig = malloc(sig_len);
   assert(sig && EVP_DigestSign(ctx, sig, &sig_len, (const unsigned char *)signed_input,
                                strlen(signed_input)) == 1);
   EVP_MD_CTX_free(ctx);
   char *s = b64url(sig, sig_len);
   char *jwt = malloc(strlen(signed_input) + strlen(s) + 2);
   assert(jwt);
   sprintf(jwt, "%s.%s", signed_input, s);
   free(sig);
   free(signed_input);
   free(h);
   free(p);
   free(s);
   return jwt;
}

static void assert_zero(const kb_workload_identity_t *identity)
{
   const unsigned char *p = (const unsigned char *)identity;
   for (size_t i = 0; i < sizeof(*identity); ++i)
      assert(p[i] == 0);
}

static kb_workload_result_t validate(const char *payload, EVP_PKEY *key, const char *jwks,
                                     kb_workload_identity_t *out)
{
   char *jwt = jwt_make(payload, key);
   kb_workload_result_t rc = kb_workload_jwt_validate(
       jwt, strlen(jwt), jwks, strlen(jwks), "https://spire.test", "aimee-kb", 1000, 300, out);
   free(jwt);
   return rc;
}

static void rejected(const char *payload, EVP_PKEY *key, const char *jwks)
{
   kb_workload_identity_t out;
   memset(&out, 0xa5, sizeof(out));
   assert(validate(payload, key, jwks, &out) == KB_WORKLOAD_INTEGRITY);
   assert_zero(&out);
}

int main(void)
{
   EVP_PKEY *key = key_generate();
   char *jwks = jwks_make(key);
   kb_workload_identity_t out;
   static const char valid[] =
       "{\"iss\":\"https://spire.test\",\"sub\":\"spiffe://test/kb/instance-1\","
       "\"aud\":\"aimee-kb\",\"iat\":990,\"exp\":1100,\"nbf\":990}";
   assert(validate(valid, key, jwks, &out) == KB_WORKLOAD_OK);
   assert(!strcmp(out.issuer, "https://spire.test"));
   assert(!strcmp(out.subject, "spiffe://test/kb/instance-1"));
   assert(out.issued_at == 990 && out.expires_at == 1100);
   char *valid_jwt = jwt_make(valid, key);
   unsigned char expected_hash[32];
   unsigned int expected_hash_len = 0;
   assert(EVP_Digest(valid_jwt, strlen(valid_jwt), expected_hash, &expected_hash_len, EVP_sha256(),
                     NULL) == 1 &&
          expected_hash_len == sizeof(expected_hash));
   assert(kb_workload_jwt_validate(valid_jwt, strlen(valid_jwt), jwks, strlen(jwks),
                                   "https://spire.test", "aimee-kb", 1000, 300,
                                   &out) == KB_WORKLOAD_OK &&
          memcmp(out.token_hash, expected_hash, sizeof(expected_hash)) == 0);
   free(valid_jwt);

   /* A singleton audience array is unambiguous and accepted. */
   assert(validate("{\"iss\":\"https://spire.test\",\"sub\":\"instance-1\","
                   "\"aud\":[\"aimee-kb\"],\"iat\":990,\"exp\":1100}",
                   key, jwks, &out) == KB_WORKLOAD_OK);

   rejected("{\"iss\":\"https://spire.test\",\"iss\":\"https://spire.test\","
            "\"sub\":\"s\",\"aud\":\"aimee-kb\",\"iat\":990,\"exp\":1100}",
            key, jwks);
   rejected("{\"iss\":\"https://spire.test\",\"sub\":\"\",\"aud\":\"aimee-kb\","
            "\"iat\":990,\"exp\":1100}",
            key, jwks);
   rejected("{\"iss\":\"https://spire.test\",\"sub\":\"bad\\u0000suffix\","
            "\"aud\":\"aimee-kb\",\"iat\":990,\"exp\":1100}",
            key, jwks);
   assert(validate("{\"iss\":\"https://spire.test\",\"sub\":\"literal\\\\u0000\","
                   "\"aud\":\"aimee-kb\",\"iat\":990,\"exp\":1100}",
                   key, jwks, &out) == KB_WORKLOAD_OK);
   assert(!strcmp(out.subject, "literal\\u0000"));
   rejected("{\"iss\":\"https://spire.test\",\"sub\":\"bad\\u001fsuffix\","
            "\"aud\":\"aimee-kb\",\"iat\":990,\"exp\":1100}",
            key, jwks);
   rejected("{\"iss\":\"https://spire.test\",\"sub\":\"s\","
            "\"aud\":[\"aimee-kb\",\"other\"],\"iat\":990,\"exp\":1100}",
            key, jwks);
   rejected("{\"iss\":\"https://spire.test\",\"sub\":\"s\",\"aud\":\"aimee-kb\","
            "\"iat\":990.5,\"exp\":1100}",
            key, jwks);
   rejected("{\"iss\":\"https://spire.test\",\"sub\":\"s\",\"aud\":\"aimee-kb\","
            "\"iat\":990,\"exp\":1100,\"nbf\":1003}",
            key, jwks);
   rejected("{\"iss\":\"https://spire.test\",\"sub\":\"s\",\"aud\":\"aimee-kb\","
            "\"iat\":1003,\"exp\":1100}",
            key, jwks);
   assert(validate("{\"iss\":\"https://spire.test\",\"sub\":\"s with space\","
                   "\"aud\":\"aimee-kb\",\"iat\":1002,\"exp\":1100}",
                   key, jwks, &out) == KB_WORKLOAD_OK);

   /* Epoch-adjacent values must not trigger unsigned skew/age underflow. */
   char *epoch_jwt = jwt_make("{\"iss\":\"https://spire.test\",\"sub\":\"s\","
                              "\"aud\":\"aimee-kb\",\"iat\":0,\"exp\":30}",
                              key);
   assert(kb_workload_jwt_validate(epoch_jwt, strlen(epoch_jwt), jwks, strlen(jwks),
                                   "https://spire.test", "aimee-kb", 0, 300,
                                   &out) == KB_WORKLOAD_OK);
   free(epoch_jwt);
   rejected("{\"iss\":\"https://spire.test\",\"sub\":\"s\",\"aud\":\"aimee-kb\","
            "\"iat\":600,\"exp\":1100}",
            key, jwks);
   rejected("{\"iss\":\"https://spire.test\",\"sub\":\"s\",\"aud\":\"aimee-kb\","
            "\"iat\":990,\"exp\":1029}",
            key, jwks);
   rejected("{\"iss\":\"https://spire.test\",\"sub\":\"s\",\"aud\":\"aimee-kb\","
            "\"iat\":990,\"exp\":1e999}",
            key, jwks);
   rejected("{\"iss\":\"https://spire.test\",\"sub\":\"s\",\"aud\":\"aimee-kb\","
            "\"iat\":9007199254740992,\"exp\":9007199254740992}",
            key, jwks);

   char long_sub[900];
   memset(long_sub, 'a', 601);
   long_sub[601] = '\0';
   char payload[1200];
   snprintf(payload, sizeof(payload),
            "{\"iss\":\"https://spire.test\",\"sub\":\"%s\",\"aud\":\"aimee-kb\","
            "\"iat\":990,\"exp\":1100}",
            long_sub);
   rejected(payload, key, jwks);

   memset(&out, 0xa5, sizeof(out));
   assert(kb_workload_jwt_validate("x\0y", 3, jwks, strlen(jwks), "https://spire.test", "aimee-kb",
                                   1000, 300, &out) == KB_WORKLOAD_INVALID);
   assert_zero(&out);

   free(jwks);
   EVP_PKEY_free(key);
   puts("kb_workload_jwt: strict claims passed");
   return 0;
}
