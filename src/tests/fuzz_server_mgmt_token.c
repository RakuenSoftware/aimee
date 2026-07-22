/* Strict management-token verifier fuzzing. The harness always exercises the
 * exact valid seed, a mutation of that seed, arbitrary compact-token bytes,
 * and arbitrary authenticated-JWKS bytes. */
#include "server/server_mgmt_token.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_MAX 65536u
#define NOW      INT64_C(1900000000)

static char *seed_jwt;
static char *seed_jwks;

static char *b64(const void *raw, size_t n)
{
   static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   const unsigned char *in = raw;
   char *out = malloc((n * 4 + 2) / 3 + 1);
   if (!out)
      return NULL;
   size_t o = 0;
   for (size_t i = 0; i < n; i += 3)
   {
      size_t left = n - i;
      unsigned v = (unsigned)in[i] << 16;
      if (left > 1)
         v |= (unsigned)in[i + 1] << 8;
      if (left > 2)
         v |= in[i + 2];
      out[o++] = a[(v >> 18) & 63];
      out[o++] = a[(v >> 12) & 63];
      if (left > 1)
         out[o++] = a[(v >> 6) & 63];
      if (left > 2)
         out[o++] = a[v & 63];
   }
   out[o] = '\0';
   return out;
}

static int initialize_seed(void)
{
   if (seed_jwt)
      return 1;
   EVP_PKEY *key = EVP_RSA_gen(2048);
   BIGNUM *n = NULL, *e = NULL;
   if (!key || EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &n) != 1 ||
       EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &e) != 1)
      goto fail;
   unsigned char nb[1024], eb[16];
   int nn = BN_bn2bin(n, nb), en = BN_bn2bin(e, eb);
   char *n64 = b64(nb, (size_t)nn), *e64 = b64(eb, (size_t)en);
   if (!n64 || !e64)
   {
      free(n64);
      free(e64);
      goto fail;
   }
   size_t jwks_cap = strlen(n64) + strlen(e64) + 160;
   seed_jwks = malloc(jwks_cap);
   if (!seed_jwks)
   {
      free(n64);
      free(e64);
      goto fail;
   }
   snprintf(seed_jwks, jwks_cap,
            "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"management-1\",\"use\":\"sig\","
            "\"alg\":\"RS256\",\"n\":\"%s\",\"e\":\"%s\"}]}",
            n64, e64);
   free(n64);
   free(e64);
   const char *header = "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"management-1\"}";
   const char *payload =
       "{\"v\":1,\"iss\":\"issuer\",\"aud\":\"server-01\",\"sub\":\"owner\","
       "\"team_id\":1,\"cap\":\"remote_writes\",\"jti\":\"0123456789abcdef\","
       "\"correlation_id\":\"request-1\","
       "\"request_sha256\":\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\","
       "\"peer_issuer\":\"CN={management}\",\"peer_serial\":\"01af\","
       "\"peer_fingerprint\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
       "\"iat\":1900000000,\"exp\":1900000090}";
   char *h = b64(header, strlen(header)), *p = b64(payload, strlen(payload));
   if (!h || !p)
   {
      free(h);
      free(p);
      goto fail;
   }
   size_t input_cap = strlen(h) + strlen(p) + 2;
   char *input = malloc(input_cap);
   if (!input)
   {
      free(h);
      free(p);
      goto fail;
   }
   snprintf(input, input_cap, "%s.%s", h, p);
   free(h);
   free(p);
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   EVP_PKEY_CTX *pk = NULL;
   size_t sn = 0;
   unsigned char *sig = NULL;
   char *s = NULL;
   int signed_ok = md && EVP_DigestSignInit(md, &pk, EVP_sha256(), NULL, key) == 1 && pk &&
                   EVP_PKEY_CTX_set_rsa_padding(pk, RSA_PKCS1_PADDING) == 1 &&
                   EVP_DigestSign(md, NULL, &sn, (unsigned char *)input, strlen(input)) == 1;
   if (signed_ok)
   {
      sig = malloc(sn);
      signed_ok = sig && EVP_DigestSign(md, sig, &sn, (unsigned char *)input, strlen(input)) == 1;
   }
   if (signed_ok)
      s = b64(sig, sn);
   if (s)
   {
      size_t cap = strlen(input) + strlen(s) + 2;
      seed_jwt = malloc(cap);
      if (seed_jwt)
         snprintf(seed_jwt, cap, "%s.%s", input, s);
   }
   free(sig);
   free(s);
   free(input);
   EVP_MD_CTX_free(md);
fail:
   BN_free(n);
   BN_free(e);
   EVP_PKEY_free(key);
   if (!seed_jwt)
   {
      free(seed_jwks);
      seed_jwks = NULL;
   }
   return seed_jwt != NULL;
}

static void verify_one(const char *jwt, size_t jwt_n, const char *jwks)
{
   static const char hash[] = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
   static const char fingerprint[] =
       "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
   server_mgmt_token_claims_t out;
   (void)server_mgmt_token_verify(jwt, jwt_n, jwks, "issuer", "server-01", "CN={management}",
                                  "01af", fingerprint, hash, NOW, &out);
}

static void fuzz_one(const unsigned char *data, size_t size)
{
   if ((!data && size) || size > FUZZ_MAX || !initialize_seed())
      return;
   verify_one(seed_jwt, strlen(seed_jwt), seed_jwks); /* valid seeded corpus */
   verify_one((const char *)data, size, seed_jwks);
   size_t jwt_n = strlen(seed_jwt);
   char *mutated = malloc(jwt_n + 1);
   if (mutated)
   {
      memcpy(mutated, seed_jwt, jwt_n + 1);
      for (size_t i = 0; i + 1 < size && i < 256; i += 2)
         mutated[(size_t)data[i] % jwt_n] ^= data[i + 1];
      verify_one(mutated, jwt_n, seed_jwks);
      free(mutated);
   }
   char *jwks = malloc(size + 1);
   if (jwks)
   {
      memcpy(jwks, data, size);
      jwks[size] = '\0';
      verify_one(seed_jwt, strlen(seed_jwt), jwks);
      free(jwks);
   }
}

#ifndef FUZZ_STANDALONE
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
   fuzz_one(data, size);
   return 0;
}
#else
static int fuzz_file(const char *path)
{
   FILE *file = fopen(path, "rb");
   if (!file)
      return 1;
   unsigned char input[FUZZ_MAX];
   size_t size = fread(input, 1, sizeof(input), file);
   int too_large = size == sizeof(input) && fgetc(file) != EOF;
   int failed = ferror(file);
   fclose(file);
   if (too_large || failed)
      return failed ? 1 : 0;
   fuzz_one(input, size);
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      unsigned char input[FUZZ_MAX];
      fuzz_one(input, fread(input, 1, sizeof(input), stdin));
   }
   else
      for (int i = 1; i < argc; ++i)
         if (fuzz_file(argv[i]) != 0)
            return 1;
   printf("fuzz_server_mgmt_token: %d inputs ok\n", argc > 1 ? argc - 1 : 1);
   return 0;
}
#endif
