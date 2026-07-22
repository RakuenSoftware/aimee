#include "server/server_mgmt_token.h"

#include <assert.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOW INT64_C(1900000000)

static const char *issuer = "https://kb.example.test/management";
static const char *audience = "server-01";
/* Braces exercise the raw numeric lexer: structural bytes inside a JSON string
 * must not alter its top-level depth. */
static const char *peer_issuer = "CN={aimee}-management-ca";
static const char *peer_serial = "01af";
static const char *fingerprint = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char *request_hash =
    "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

static char *b64(const void *raw, size_t n)
{
   static const char alphabet[] =
       "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   const unsigned char *in = raw;
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
      out[o++] = alphabet[(v >> 18) & 63];
      out[o++] = alphabet[(v >> 12) & 63];
      if (left > 1)
         out[o++] = alphabet[(v >> 6) & 63];
      if (left > 2)
         out[o++] = alphabet[v & 63];
   }
   out[o] = '\0';
   return out;
}

static char *make_jwks(EVP_PKEY *key)
{
   BIGNUM *n = NULL, *e = NULL;
   assert(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &n) == 1);
   assert(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &e) == 1);
   unsigned char nb[1024], eb[16];
   int nn = BN_bn2bin(n, nb), en = BN_bn2bin(e, eb);
   BN_free(n);
   BN_free(e);
   char *n64 = b64(nb, (size_t)nn), *e64 = b64(eb, (size_t)en);
   size_t cap = strlen(n64) + strlen(e64) + 160;
   char *out = malloc(cap);
   assert(out);
   snprintf(out, cap,
            "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"management-1\",\"use\":\"sig\","
            "\"alg\":\"RS256\",\"n\":\"%s\",\"e\":\"%s\"}]}",
            n64, e64);
   free(n64);
   free(e64);
   return out;
}

static char *mint(EVP_PKEY *key, const char *header, const char *payload)
{
   char *h = b64(header, strlen(header)), *p = b64(payload, strlen(payload));
   size_t input_n = strlen(h) + strlen(p) + 2;
   char *input = malloc(input_n);
   assert(input);
   snprintf(input, input_n, "%s.%s", h, p);
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   EVP_PKEY_CTX *pk = NULL;
   assert(md && EVP_DigestSignInit(md, &pk, EVP_sha256(), NULL, key) == 1);
   assert(EVP_PKEY_CTX_set_rsa_padding(pk, RSA_PKCS1_PADDING) == 1);
   size_t sn = 0;
   assert(EVP_DigestSign(md, NULL, &sn, (unsigned char *)input, strlen(input)) == 1);
   unsigned char *sig = malloc(sn);
   assert(sig && EVP_DigestSign(md, sig, &sn, (unsigned char *)input, strlen(input)) == 1);
   EVP_MD_CTX_free(md);
   char *s = b64(sig, sn);
   size_t jwt_n = strlen(input) + strlen(s) + 2;
   char *jwt = malloc(jwt_n);
   assert(jwt);
   snprintf(jwt, jwt_n, "%s.%s", input, s);
   free(sig);
   free(s);
   free(input);
   free(h);
   free(p);
   return jwt;
}

static char *payload(const char *extra, const char *subject, int64_t iat, int64_t exp)
{
   size_t cap = 2048 + strlen(extra);
   char *out = malloc(cap);
   assert(out);
   snprintf(out, cap,
            "{\"v\":1,\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"%s\",\"team_id\":7,"
            "\"cap\":\"remote_writes\",\"jti\":\"0123456789abcdef\","
            "\"correlation_id\":\"request-1\",\"request_sha256\":\"%s\","
            "\"peer_issuer\":\"%s\",\"peer_serial\":\"%s\","
            "\"peer_fingerprint\":\"%s\",\"iat\":%lld,\"exp\":%lld%s}",
            issuer, audience, subject, request_hash, peer_issuer, peer_serial, fingerprint,
            (long long)iat, (long long)exp, extra);
   return out;
}

static int verify(const char *jwt, size_t n, const char *jwks, int64_t now,
                  server_mgmt_token_claims_t *out)
{
   return server_mgmt_token_verify(jwt, n, jwks, issuer, audience, peer_issuer, peer_serial,
                                   fingerprint, request_hash, now, out);
}

static void expect_payload_reject(EVP_PKEY *key, const char *jwks, const char *raw)
{
   char *jwt = mint(key, "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"management-1\"}", raw);
   server_mgmt_token_claims_t claims;
   memset(&claims, 0xa5, sizeof(claims));
   assert(!verify(jwt, strlen(jwt), jwks, NOW, &claims));
   const unsigned char *p = (const unsigned char *)&claims;
   for (size_t i = 0; i < sizeof(claims); ++i)
      assert(p[i] == 0);
   free(jwt);
}

static char *replace_once(const char *input, const char *needle, const char *replacement)
{
   const char *at = strstr(input, needle);
   assert(at);
   size_t prefix = (size_t)(at - input);
   size_t n = prefix + strlen(replacement) + strlen(at + strlen(needle)) + 1;
   char *out = malloc(n);
   assert(out);
   memcpy(out, input, prefix);
   strcpy(out + prefix, replacement);
   strcpy(out + prefix + strlen(replacement), at + strlen(needle));
   return out;
}

static void expect_header_reject(EVP_PKEY *key, const char *jwks, const char *header,
                                 const char *raw)
{
   char *jwt = mint(key, header, raw);
   server_mgmt_token_claims_t claims;
   assert(!verify(jwt, strlen(jwt), jwks, NOW, &claims));
   free(jwt);
}

typedef struct
{
   const char *needle;
   const char *replacement;
} mutation_t;

static void test_header_matrix(EVP_PKEY *key, const char *jwks, const char *raw)
{
   static const char *const bad[] = {
       "{\"alg\":\"none\",\"typ\":\"JWT\",\"kid\":\"management-1\"}",
       "{\"alg\":1,\"typ\":\"JWT\",\"kid\":\"management-1\"}",
       "{\"alg\":\"RS256\",\"typ\":\"jwt\",\"kid\":\"management-1\"}",
       "{\"alg\":\"RS256\",\"typ\":1,\"kid\":\"management-1\"}",
       "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":1}",
       "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"\"}",
       "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"management/1\"}",
       "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"management-1\",\"jku\":\"x\"}",
       "{\"alg\":\"RS256\",\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"management-1\"}",
       "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"typ\":\"JWT\",\"kid\":\"management-1\"}",
       "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"management-1\",\"kid\":\"management-1\"}",
   };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
      expect_header_reject(key, jwks, bad[i], raw);
   char kid[66];
   memset(kid, 'a', 65);
   kid[65] = '\0';
   char header[160];
   snprintf(header, sizeof(header), "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"%s\"}", kid);
   expect_header_reject(key, jwks, header, raw);
}

static void test_payload_type_and_numeric_matrix(EVP_PKEY *key, const char *jwks, const char *raw)
{
   static const mutation_t types[] = {
       {"\"v\":1", "\"v\":\"1\""},
       {"\"iss\":\"https://kb.example.test/management\"", "\"iss\":1"},
       {"\"aud\":\"server-01\"", "\"aud\":[\"server-01\"]"},
       {"\"sub\":\"oidc:https%3A//idp.example:user%3A42\"", "\"sub\":1"},
       {"\"team_id\":7", "\"team_id\":\"7\""},
       {"\"cap\":\"remote_writes\"", "\"cap\":1"},
       {"\"jti\":\"0123456789abcdef\"", "\"jti\":1"},
       {"\"correlation_id\":\"request-1\"", "\"correlation_id\":1"},
       {"\"request_sha256\":\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\"",
        "\"request_sha256\":1"},
       {"\"peer_issuer\":\"CN={aimee}-management-ca\"", "\"peer_issuer\":1"},
       {"\"peer_serial\":\"01af\"", "\"peer_serial\":1"},
       {"\"peer_fingerprint\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"",
        "\"peer_fingerprint\":1"},
       {"\"iat\":1900000000", "\"iat\":\"1900000000\""},
       {"\"exp\":1900000090", "\"exp\":\"1900000090\""},
   };
   for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
   {
      char *bad = replace_once(raw, types[i].needle, types[i].replacement);
      expect_payload_reject(key, jwks, bad);
      free(bad);
   }
   static const mutation_t numbers[] = {
       {"\"team_id\":7", "\"team_id\":0"},
       {"\"team_id\":7", "\"team_id\":-1"},
       {"\"team_id\":7", "\"team_id\":7.0"},
       {"\"team_id\":7", "\"team_id\":7e0"},
       {"\"team_id\":7", "\"team_id\":01"},
       {"\"team_id\":7", "\"team_id\":9007199254740992"},
       {"\"iat\":1900000000", "\"iat\":1900000000.0"},
       {"\"exp\":1900000090", "\"exp\":1900000090e0"},
   };
   for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i)
   {
      char *bad = replace_once(raw, numbers[i].needle, numbers[i].replacement);
      expect_payload_reject(key, jwks, bad);
      free(bad);
   }
}

static void test_payload_duplicates(EVP_PKEY *key, const char *jwks, const char *raw)
{
   static const char *const duplicate[] = {
       ",\"v\":1",
       ",\"iss\":\"https://kb.example.test/management\"",
       ",\"aud\":\"server-01\"",
       ",\"sub\":\"owner\"",
       ",\"team_id\":7",
       ",\"cap\":\"remote_writes\"",
       ",\"jti\":\"fedcba9876543210\"",
       ",\"correlation_id\":\"request-2\"",
       ",\"request_sha256\":\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\"",
       ",\"peer_issuer\":\"CN={aimee}-management-ca\"",
       ",\"peer_serial\":\"01af\"",
       ",\"peer_fingerprint\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"",
       ",\"iat\":1900000000",
       ",\"exp\":1900000090",
   };
   size_t n = strlen(raw);
   assert(raw[n - 1] == '}');
   for (size_t i = 0; i < sizeof(duplicate) / sizeof(duplicate[0]); ++i)
   {
      char *bad = malloc(n + strlen(duplicate[i]) + 1);
      assert(bad);
      memcpy(bad, raw, n - 1);
      strcpy(bad + n - 1, duplicate[i]);
      strcat(bad, "}");
      expect_payload_reject(key, jwks, bad);
      free(bad);
   }
}

static void test_claim_bounds_bindings_and_actor(EVP_PKEY *key, const char *jwks, const char *raw)
{
   static const mutation_t bads[] = {
       {"\"cap\":\"remote_writes\"", "\"cap\":\"\""},
       {"\"cap\":\"remote_writes\"", "\"cap\":\"remote/writes\""},
       {"\"jti\":\"0123456789abcdef\"", "\"jti\":\"too-short\""},
       {"\"correlation_id\":\"request-1\"", "\"correlation_id\":\"bad id\""},
       {"\"request_sha256\":\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\"",
        "\"request_sha256\":\"bbcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\""},
       {"\"peer_issuer\":\"CN={aimee}-management-ca\"", "\"peer_issuer\":\"other\""},
       {"\"peer_serial\":\"01af\"", "\"peer_serial\":\"01AF\""},
       {"\"peer_fingerprint\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"",
        "\"peer_fingerprint\":"
        "\"1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\""},
   };
   for (size_t i = 0; i < sizeof(bads) / sizeof(bads[0]); ++i)
   {
      char *bad = replace_once(raw, bads[i].needle, bads[i].replacement);
      expect_payload_reject(key, jwks, bad);
      free(bad);
   }
   struct
   {
      const char *needle;
      const char *field;
      size_t length;
   } overlong[] = {
       {"\"sub\":\"oidc:https%3A//idp.example:user%3A42\"", "sub", 577},
       {"\"cap\":\"remote_writes\"", "cap", 65},
       {"\"jti\":\"0123456789abcdef\"", "jti", 129},
       {"\"correlation_id\":\"request-1\"", "correlation_id", 129},
       {"\"peer_issuer\":\"CN={aimee}-management-ca\"", "peer_issuer", 512},
       {"\"peer_serial\":\"01af\"", "peer_serial", 80},
   };
   for (size_t i = 0; i < sizeof(overlong) / sizeof(overlong[0]); ++i)
   {
      size_t cap = strlen(overlong[i].field) + overlong[i].length + 8;
      char *replacement = malloc(cap);
      assert(replacement);
      int prefix = snprintf(replacement, cap, "\"%s\":\"", overlong[i].field);
      assert(prefix > 0);
      memset(replacement + prefix, overlong[i].field[0] == 'p' ? 'a' : 'A', overlong[i].length);
      replacement[prefix + overlong[i].length] = '"';
      replacement[prefix + overlong[i].length + 1] = '\0';
      char *bad = replace_once(raw, overlong[i].needle, replacement);
      expect_payload_reject(key, jwks, bad);
      free(bad);
      free(replacement);
   }
   static const char *const bad_actor[] = {
       "",
       "Owner",
       "bare-subject",
       "oidc::subject",
       "oidc:issuer:",
       "oidc:issuer:sub:extra",
       "oidc:bad%3aissuer:subject",
       "oidc:bad%20issuer:subject",
       "cert:issuer:01AF",
       "cert:issuer:nothex",
       "cert::01af",
   };
   for (size_t i = 0; i < sizeof(bad_actor) / sizeof(bad_actor[0]); ++i)
   {
      char *bad = payload("", bad_actor[i], NOW, NOW + 1);
      expect_payload_reject(key, jwks, bad);
      free(bad);
   }
   static const char *const good_actor[] = {"owner", "oidc:issuer:subject", "oidc:a%3Ab:c%25d",
                                            "cert:issuer:01af"};
   for (size_t i = 0; i < sizeof(good_actor) / sizeof(good_actor[0]); ++i)
   {
      char *good_raw = payload("", good_actor[i], NOW, NOW + 1);
      char *good =
          mint(key, "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"management-1\"}", good_raw);
      server_mgmt_token_claims_t claims;
      assert(verify(good, strlen(good), jwks, NOW, &claims));
      free(good);
      free(good_raw);
   }
}

static void test_time_boundaries(EVP_PKEY *key, const char *jwks)
{
   struct
   {
      int64_t iat, exp, now;
      int accept;
   } cases[] = {{NOW, NOW + 1, NOW, 1},     {NOW, NOW + 90, NOW, 1}, {NOW + 1, NOW + 2, NOW, 0},
                {NOW, NOW, NOW, 0},         {NOW, NOW + 91, NOW, 0}, {NOW - 1, NOW, NOW, 0},
                {NOW - 90, NOW + 1, NOW, 0}};
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
   {
      char *raw = payload("", "owner", cases[i].iat, cases[i].exp);
      char *jwt = mint(key, "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"management-1\"}", raw);
      server_mgmt_token_claims_t claims;
      assert(verify(jwt, strlen(jwt), jwks, cases[i].now, &claims) == cases[i].accept);
      free(jwt);
      free(raw);
   }
}

static void test_compact_and_signature(EVP_PKEY *key, const char *jwks, const char *raw,
                                       const char *jwt)
{
   server_mgmt_token_claims_t claims;
   static const char *const malformed[] = {
       "", "one", "one.two", "one.two.three.four", ".two.three", "one..three", "one.two."};
   for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i)
      assert(!verify(malformed[i], strlen(malformed[i]), jwks, NOW, &claims));

   char *corrupt = strdup(jwt);
   char *sig = strrchr(corrupt, '.') + 1;
   sig[10] = sig[10] == 'A' ? 'B' : 'A';
   assert(!verify(corrupt, strlen(corrupt), jwks, NOW, &claims));
   free(corrupt);

   /* A 2048-bit RSA signature encodes to 342 chars: its final character has
    * four unused low bits. B therefore gives a noncanonical trailing spelling. */
   corrupt = strdup(jwt);
   corrupt[strlen(corrupt) - 1] = 'B';
   assert(!verify(corrupt, strlen(corrupt), jwks, NOW, &claims));
   free(corrupt);

   char *large_header = malloc(1400);
   assert(large_header);
   memset(large_header, 'a', 1399);
   large_header[0] = '{';
   large_header[1398] = '}';
   large_header[1399] = '\0';
   char *large_jwt = mint(key, large_header, raw);
   assert(!verify(large_jwt, strlen(large_jwt), jwks, NOW, &claims));
   free(large_jwt);
   free(large_header);

   char *large_payload = malloc(5002);
   assert(large_payload);
   memset(large_payload, 'a', 5000);
   large_payload[0] = '{';
   large_payload[4999] = '}';
   large_payload[5000] = '\0';
   large_jwt =
       mint(key, "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"management-1\"}", large_payload);
   assert(!verify(large_jwt, strlen(large_jwt), jwks, NOW, &claims));
   free(large_jwt);
   free(large_payload);

   /* Signature segment decoded size > 512 and total wire > 8192 both deny. */
   char *dots = strdup(jwt);
   char *last = strrchr(dots, '.');
   size_t prefix = (size_t)(last + 1 - dots);
   char *huge_sig = malloc(prefix + 700 + 1);
   memcpy(huge_sig, dots, prefix);
   memset(huge_sig + prefix, 'A', 700);
   huge_sig[prefix + 700] = '\0';
   assert(!verify(huge_sig, strlen(huge_sig), jwks, NOW, &claims));
   free(huge_sig);
   free(dots);
   char *huge_wire = malloc(8201);
   memset(huge_wire, 'A', 8200);
   huge_wire[8200] = '\0';
   assert(!verify(huge_wire, 8200, jwks, NOW, &claims));
   free(huge_wire);
}

static void test_jwks_matrix(const char *jwks, const char *jwt)
{
   server_mgmt_token_claims_t claims;
   static const mutation_t changes[] = {
       {"{\"keys\":[", "{\"other\":[],\"keys\":["},       {"\"kty\":\"RSA\"", "\"kty\":\"EC\""},
       {"\"kid\":\"management-1\"", "\"kid\":\"other\""}, {"\"use\":\"sig\"", "\"use\":\"enc\""},
       {"\"alg\":\"RS256\"", "\"alg\":\"RS512\""},
   };
   for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); ++i)
   {
      char *bad = replace_once(jwks, changes[i].needle, changes[i].replacement);
      assert(!verify(jwt, strlen(jwt), bad, NOW, &claims));
      free(bad);
   }
   assert(!verify(jwt, strlen(jwt), "{}", NOW, &claims));
   assert(!verify(jwt, strlen(jwt), "{\"keys\":{}}", NOW, &claims));
   assert(!verify(jwt, strlen(jwt), "{\"keys\":[]}", NOW, &claims));

   char *bad = replace_once(jwks, "\"e\":\"AQAB\"", "\"e\":\"AQ\"");
   assert(!verify(jwt, strlen(jwt), bad, NOW, &claims)); /* exponent 1 */
   free(bad);
   const char *n_at = strstr(jwks, "\"n\":\"");
   assert(n_at);
   const char *n_end = strchr(n_at + strlen("\"n\":\""), '"');
   assert(n_end);
   size_t old_n = (size_t)(n_end - n_at) + 1;
   char *old = malloc(old_n + 1);
   memcpy(old, n_at, old_n);
   old[old_n] = '\0';
   bad = replace_once(jwks, old, "\"n\":\"AQ\"");
   assert(!verify(jwt, strlen(jwt), bad, NOW, &claims)); /* weak modulus */
   free(old);
   free(bad);

   const char *object = strchr(jwks, '[') + 1;
   const char *object_end = strrchr(jwks, ']');
   size_t object_n = (size_t)(object_end - object);
   char *duplicate = malloc(strlen(jwks) + object_n + 2);
   size_t prefix = (size_t)(object_end - jwks);
   memcpy(duplicate, jwks, prefix);
   duplicate[prefix] = ',';
   memcpy(duplicate + prefix + 1, object, object_n);
   strcpy(duplicate + prefix + 1 + object_n, object_end);
   assert(!verify(jwt, strlen(jwt), duplicate, NOW, &claims));
   free(duplicate);

   bad = replace_once(jwks, "\"kty\":\"RSA\"", "\"kty\":\"RSA\",\"x5u\":\"x\"");
   assert(!verify(jwt, strlen(jwt), bad, NOW, &claims));
   free(bad);
}

static void test_legacy_rejected(EVP_PKEY *key, const char *jwks)
{
   char legacy[512];
   snprintf(legacy, sizeof(legacy),
            "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"owner\","
            "\"cert_cn\":\"kb\",\"cap\":\"remote_writes\","
            "\"jti\":\"0123456789abcdef\",\"iat\":%lld,\"exp\":%lld}",
            issuer, audience, (long long)NOW, (long long)(NOW + 1));
   expect_payload_reject(key, jwks, legacy);
}

int main(void)
{
   EVP_PKEY *key = EVP_RSA_gen(2048);
   assert(key);
   char *jwks = make_jwks(key);
   char *raw = payload("", "oidc:https%3A//idp.example:user%3A42", NOW, NOW + 90);
   char *jwt = mint(key, "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"management-1\"}", raw);
   server_mgmt_token_claims_t claims;
   assert(verify(jwt, strlen(jwt), jwks, NOW, &claims));
   assert(claims.version == 1 && claims.team_id == 7 && claims.issued_at == NOW &&
          claims.expires_at == NOW + 90);
   assert(strcmp(claims.subject, "oidc:https%3A//idp.example:user%3A42") == 0);
   assert(strcmp(claims.kid, "management-1") == 0);

   test_header_matrix(key, jwks, raw);
   test_payload_type_and_numeric_matrix(key, jwks, raw);
   test_payload_duplicates(key, jwks, raw);
   test_claim_bounds_bindings_and_actor(key, jwks, raw);
   test_time_boundaries(key, jwks);
   test_compact_and_signature(key, jwks, raw, jwt);
   test_jwks_matrix(jwks, jwt);
   test_legacy_rejected(key, jwks);

   /* The explicit wire length makes an embedded NUL plus trailing bytes invalid. */
   size_t jwt_n = strlen(jwt);
   char *with_nul = malloc(jwt_n + 2);
   memcpy(with_nul, jwt, jwt_n + 1);
   with_nul[jwt_n + 1] = 'x';
   assert(!verify(with_nul, jwt_n + 2, jwks, NOW, &claims));
   free(with_nul);

   char *bad = payload(",\"unknown\":1", "owner", NOW, NOW + 1);
   expect_payload_reject(key, jwks, bad);
   free(bad);
   bad = payload(",\"jti\":\"fedcba9876543210\"", "owner", NOW, NOW + 1);
   expect_payload_reject(key, jwks, bad); /* duplicate member */
   free(bad);
   bad = payload("", "user-without-issuer", NOW, NOW + 1);
   expect_payload_reject(key, jwks, bad);
   free(bad);
   bad = payload("", "owner", NOW + 1, NOW + 2);
   expect_payload_reject(key, jwks, bad);
   free(bad);
   bad = payload("", "owner", NOW, NOW + 91);
   expect_payload_reject(key, jwks, bad);
   free(bad);

   char *wrong_header = mint(key,
                             "{\"alg\":\"RS256\",\"typ\":\"JWT\","
                             "\"kid\":\"management-1\",\"jku\":\"x\"}",
                             raw);
   assert(!verify(wrong_header, strlen(wrong_header), jwks, NOW, &claims));
   free(wrong_header);

   /* An otherwise usable JWK with an interpretation field outside the exact
    * authenticated contract is rejected. */
   char *jwks_extra = malloc(strlen(jwks) + 32);
   strcpy(jwks_extra, jwks);
   char *close = strstr(jwks_extra, "}]}");
   assert(close);
   strcpy(close, ",\"key_ops\":[\"verify\"]}]}");
   assert(!verify(jwt, strlen(jwt), jwks_extra, NOW, &claims));
   free(jwks_extra);

   free(jwt);
   free(raw);
   free(jwks);
   EVP_PKEY_free(key);
   puts("server management token verifier: ok");
   return 0;
}
