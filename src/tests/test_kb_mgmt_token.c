#include "kb_mgmt_token.h"
#include "server/server_mgmt_token.h"

#include <assert.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOW INT64_C(1900000000)

typedef struct
{
   EVP_PKEY *key;
   unsigned calls;
   int fail;
   size_t forced_len;
} signer_t;

static int sign_rs256(void *opaque, const unsigned char *input, size_t input_n,
                      unsigned char *signature, size_t cap, size_t *signature_n)
{
   signer_t *s = opaque;
   s->calls++;
   if (s->fail)
      return 0;
   if (s->forced_len)
   {
      memset(signature, 0xa5, cap);
      *signature_n = s->forced_len;
      return 1;
   }
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   EVP_PKEY_CTX *pk = NULL;
   size_t n = cap;
   int ok = md && EVP_DigestSignInit(md, &pk, EVP_sha256(), NULL, s->key) == 1 && pk &&
            EVP_PKEY_CTX_set_rsa_padding(pk, RSA_PKCS1_PADDING) == 1 &&
            EVP_PKEY_CTX_set_signature_md(pk, EVP_sha256()) == 1 &&
            EVP_DigestSignUpdate(md, input, input_n) == 1 &&
            EVP_DigestSignFinal(md, signature, &n) == 1;
   EVP_MD_CTX_free(md);
   if (ok)
      *signature_n = n;
   return ok;
}

static EVP_PKEY *new_rsa(unsigned bits)
{
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   EVP_PKEY *key = NULL;
   assert(ctx && EVP_PKEY_keygen_init(ctx) == 1 &&
          EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, (int)bits) == 1 && EVP_PKEY_keygen(ctx, &key) == 1);
   EVP_PKEY_CTX_free(ctx);
   return key;
}

static char *b64(const unsigned char *in, size_t n)
{
   static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   char *out = malloc((n * 4 + 2) / 3 + 1);
   assert(out);
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

static char *unb64(const char *in, size_t n)
{
   char *out = malloc(n + 1);
   assert(out);
   uint32_t acc = 0;
   unsigned bits = 0;
   size_t used = 0;
   for (size_t i = 0; i < n; ++i)
   {
      const char *p =
          strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", in[i]);
      assert(p);
      acc = (acc << 6) |
            (uint32_t)(p - "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         out[used++] = (char)(acc >> bits);
         acc &= bits ? ((UINT32_C(1) << bits) - 1) : 0;
      }
   }
   assert(acc == 0);
   out[used] = '\0';
   return out;
}

static char *jwks(EVP_PKEY *key)
{
   BIGNUM *n = NULL, *e = NULL;
   assert(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &n) == 1);
   assert(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &e) == 1);
   unsigned char nb[1024], eb[16];
   int nn = BN_bn2bin(n, nb), en = BN_bn2bin(e, eb);
   BN_free(n);
   BN_free(e);
   char *ns = b64(nb, (size_t)nn), *es = b64(eb, (size_t)en);
   size_t cap = strlen(ns) + strlen(es) + 160;
   char *out = malloc(cap);
   assert(out);
   snprintf(out, cap,
            "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"management-1\",\"use\":\"sig\","
            "\"alg\":\"RS256\",\"n\":\"%s\",\"e\":\"%s\"}]}",
            ns, es);
   free(ns);
   free(es);
   return out;
}

static kb_mgmt_token_claims_t claims(void)
{
   kb_mgmt_token_claims_t c;
   memset(&c, 0, sizeof(c));
   snprintf(c.issuer, sizeof(c.issuer), "https://kb.example.test/management");
   snprintf(c.audience, sizeof(c.audience), "server-01");
   snprintf(c.subject, sizeof(c.subject), "%s", "oidc:https%3A%25issuer:operator");
   c.team_id = 42;
   c.capability = KB_MGMT_TOKEN_CAP_REMOTE_WRITES;
   snprintf(c.jti, sizeof(c.jti), "0123456789abcdef01234567");
   snprintf(c.correlation_id, sizeof(c.correlation_id), "request-1");
   snprintf(c.request_sha256, sizeof(c.request_sha256),
            "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
   snprintf(c.peer_issuer, sizeof(c.peer_issuer), "CN={aimee}\\\" management CA");
   snprintf(c.peer_serial, sizeof(c.peer_serial), "01af");
   snprintf(c.peer_fingerprint, sizeof(c.peer_fingerprint),
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
   snprintf(c.kid, sizeof(c.kid), "management-1");
   c.issued_at = NOW;
   c.expires_at = NOW + 60;
   return c;
}

static void invalid_without_sign(kb_mgmt_token_claims_t *c, signer_t *signer)
{
   char out[KB_MGMT_TOKEN_WIRE_MAX + 1];
   memset(out, 'x', sizeof(out));
   size_t out_n = 17;
   signer->calls = 0;
   assert(kb_mgmt_token_build(c, sign_rs256, signer, out, sizeof(out), &out_n) ==
          KB_MGMT_TOKEN_INVALID);
   assert(signer->calls == 0 && out_n == 0 && out[0] == '\0');
}

static void changed_token(const kb_mgmt_token_claims_t *c, signer_t *signer, const char *baseline)
{
   char out[KB_MGMT_TOKEN_WIRE_MAX + 1];
   size_t out_n = 0;
   assert(kb_mgmt_token_build(c, sign_rs256, signer, out, sizeof(out), &out_n) == KB_MGMT_TOKEN_OK);
   assert(out_n > 0 && strcmp(out, baseline) != 0);
}

static void roundtrip(unsigned bits)
{
   EVP_PKEY *key = new_rsa(bits);
   signer_t signer = {key, 0, 0, 0};
   kb_mgmt_token_claims_t c = claims();
   char token[KB_MGMT_TOKEN_WIRE_MAX + 1], again[KB_MGMT_TOKEN_WIRE_MAX + 1];
   size_t token_n = 0, again_n = 0;
   assert(kb_mgmt_token_build(&c, sign_rs256, &signer, token, sizeof(token), &token_n) ==
          KB_MGMT_TOKEN_OK);
   assert(token_n == strlen(token) && signer.calls == 1);
   const char *dot1 = strchr(token, '.');
   const char *dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;
   assert(dot1 && dot2 && !strchr(dot2 + 1, '.'));
   char *header = unb64(token, (size_t)(dot1 - token));
   char *payload = unb64(dot1 + 1, (size_t)(dot2 - dot1 - 1));
   assert(strcmp(header, "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"management-1\"}") == 0);
   assert(strcmp(payload, "{\"v\":1,\"iss\":\"https://kb.example.test/management\","
                          "\"aud\":\"server-01\",\"sub\":\"oidc:https%3A%25issuer:operator\","
                          "\"team_id\":42,\"cap\":\"remote_writes\","
                          "\"jti\":\"0123456789abcdef01234567\","
                          "\"correlation_id\":\"request-1\","
                          "\"request_sha256\":"
                          "\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\","
                          "\"peer_issuer\":\"CN={aimee}\\\\\\\" management CA\","
                          "\"peer_serial\":\"01af\","
                          "\"peer_fingerprint\":"
                          "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
                          "\"iat\":1900000000,\"exp\":1900000060}") == 0);
   assert(!strchr(token, '=') && !strstr(payload, "cert_cn"));
   free(header);
   free(payload);
   assert(kb_mgmt_token_build(&c, sign_rs256, &signer, again, sizeof(again), &again_n) ==
          KB_MGMT_TOKEN_OK);
   assert(again_n == token_n && memcmp(again, token, token_n + 1) == 0 && signer.calls == 2);

   char *keys = jwks(key);
   server_mgmt_token_claims_t verified;
   assert(server_mgmt_token_verify(token, token_n, keys, c.issuer, c.audience, c.peer_issuer,
                                   c.peer_serial, c.peer_fingerprint, c.request_sha256, NOW,
                                   &verified));
   assert(verified.team_id == 42 && strcmp(verified.capability, "remote_writes") == 0 &&
          strcmp(verified.subject, c.subject) == 0);
   free(keys);
   EVP_PKEY_free(key);
}

int main(void)
{
   roundtrip(2048);
   roundtrip(4096);
   roundtrip(8192);

   EVP_PKEY *key = new_rsa(2048);
   signer_t signer = {key, 0, 0, 0};
   kb_mgmt_token_claims_t c = claims();
   char out[KB_MGMT_TOKEN_WIRE_MAX + 1];
   size_t out_n = 7;

   assert(kb_mgmt_token_build(&c, sign_rs256, &signer, out, 64, &out_n) ==
          KB_MGMT_TOKEN_OUTPUT_TOO_SMALL);
   assert(out_n == 0 && out[0] == '\0' && signer.calls == 0);
   c.capability = (kb_mgmt_token_capability_t)99;
   assert(kb_mgmt_token_build(&c, sign_rs256, &signer, out, sizeof(out), &out_n) ==
          KB_MGMT_TOKEN_INVALID);
   assert(signer.calls == 0);
   c = claims();
   c.expires_at = c.issued_at + 91;
   assert(kb_mgmt_token_build(&c, sign_rs256, &signer, out, sizeof(out), &out_n) ==
          KB_MGMT_TOKEN_INVALID);
   assert(signer.calls == 0);
   c = claims();
   signer.fail = 1;
   assert(kb_mgmt_token_build(&c, sign_rs256, &signer, out, sizeof(out), &out_n) ==
          KB_MGMT_TOKEN_SIGN_UNAVAILABLE);
   assert(out_n == 0 && out[0] == '\0' && signer.calls == 1);

   signer.fail = 0;
   signer.forced_len = 255;
   assert(kb_mgmt_token_build(&c, sign_rs256, &signer, out, sizeof(out), &out_n) ==
          KB_MGMT_TOKEN_SIGN_UNAVAILABLE);
   assert(out_n == 0 && out[0] == '\0' && signer.calls == 2);
   signer.forced_len = 1025;
   assert(kb_mgmt_token_build(&c, sign_rs256, &signer, out, sizeof(out), &out_n) ==
          KB_MGMT_TOKEN_SIGN_UNAVAILABLE);
   assert(out_n == 0 && out[0] == '\0' && signer.calls == 3);

   c = claims();
   memset(c.issuer, '"', sizeof(c.issuer) - 1);
   memset(c.audience, 'a', sizeof(c.audience) - 1);
   memcpy(c.subject, "oidc:", 5);
   for (size_t i = 5; i < sizeof(c.subject) - 1; ++i)
      c.subject[i] = i == 289 ? ':' : (i & 1 ? '"' : '\\');
   memset(c.jti, 'j', sizeof(c.jti) - 1);
   memset(c.correlation_id, 'c', sizeof(c.correlation_id) - 1);
   memset(c.peer_issuer, '\\', sizeof(c.peer_issuer) - 1);
   memset(c.peer_serial, 'a', sizeof(c.peer_serial) - 1);
   memset(c.kid, 'k', sizeof(c.kid) - 1);
   signer.forced_len = 256;
   assert(kb_mgmt_token_build(&c, sign_rs256, &signer, out, sizeof(out), &out_n) ==
          KB_MGMT_TOKEN_OK);
   assert(out_n <= KB_MGMT_TOKEN_WIRE_MAX && signer.calls == 4);

   c = claims();
   memset(c.kid, 'x', sizeof(c.kid));
   assert(kb_mgmt_token_build(&c, sign_rs256, &signer, out, sizeof(out), &out_n) ==
          KB_MGMT_TOKEN_INVALID);
   assert(signer.calls == 4);

   signer.forced_len = 0;
   c = claims();
   char baseline[KB_MGMT_TOKEN_WIRE_MAX + 1];
   size_t baseline_n = 0;
   assert(kb_mgmt_token_build(&c, sign_rs256, &signer, baseline, sizeof(baseline), &baseline_n) ==
          KB_MGMT_TOKEN_OK);
#define CHANGED(field, value)                                                                      \
   do                                                                                              \
   {                                                                                               \
      c = claims();                                                                                \
      memset(c.field, 0, sizeof(c.field));                                                         \
      snprintf(c.field, sizeof(c.field), "%s", value);                                             \
      changed_token(&c, &signer, baseline);                                                        \
   } while (0)
   CHANGED(issuer, "issuer-2");
   CHANGED(audience, "server-02");
   CHANGED(subject, "owner");
   c = claims();
   c.team_id++;
   changed_token(&c, &signer, baseline);
   CHANGED(jti, "1123456789abcdef01234567");
   CHANGED(correlation_id, "request-2");
   CHANGED(request_sha256, "bbcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
   CHANGED(peer_issuer, "CN=management-2");
   CHANGED(peer_serial, "01ae");
   CHANGED(peer_fingerprint, "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
   CHANGED(kid, "management-2");
   c = claims();
   c.issued_at--;
   c.expires_at--;
   changed_token(&c, &signer, baseline);
   c = claims();
   c.expires_at--;
   changed_token(&c, &signer, baseline);
#undef CHANGED

#define POISON(field)                                                                              \
   do                                                                                              \
   {                                                                                               \
      c = claims();                                                                                \
      c.field[1] = '\0';                                                                           \
      c.field[2] = 'x';                                                                            \
      invalid_without_sign(&c, &signer);                                                           \
   } while (0)
   POISON(issuer);
   POISON(audience);
   POISON(subject);
   POISON(jti);
   POISON(correlation_id);
   POISON(request_sha256);
   POISON(peer_issuer);
   POISON(peer_serial);
   POISON(peer_fingerprint);
   POISON(kid);
#undef POISON

   c = claims();
   c.team_id = 0;
   invalid_without_sign(&c, &signer);
   c = claims();
   c.team_id = INT64_C(9007199254740992);
   invalid_without_sign(&c, &signer);
   c = claims();
   c.issued_at = -1;
   invalid_without_sign(&c, &signer);
   c = claims();
   c.expires_at = c.issued_at;
   invalid_without_sign(&c, &signer);
   c = claims();
   snprintf(c.subject, sizeof(c.subject), "oidc:raw:colon:subject");
   invalid_without_sign(&c, &signer);
   c = claims();
   snprintf(c.subject, sizeof(c.subject), "oidc:bad%%3aescape:subject");
   invalid_without_sign(&c, &signer);
   c = claims();
   c.request_sha256[0] = 'A';
   invalid_without_sign(&c, &signer);
   c = claims();
   c.audience[0] = '/';
   invalid_without_sign(&c, &signer);
   EVP_PKEY_free(key);
   puts("kb management token tests passed");
   return 0;
}
