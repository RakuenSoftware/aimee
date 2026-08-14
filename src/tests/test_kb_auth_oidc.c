/* test_kb_auth_oidc.c — unit tests for the BYO OIDC/JWT verifier
 * (src/kb/auth_oidc.c). Mints a real RSA keypair, builds a JWKS from it, signs
 * RS256 JWTs, and drives kb_oidc_verify_jwt through accept + every reject path
 * (bad signature, wrong key, tampered payload, alg confusion, expiry, iss/aud).
 * Also checks the seam registration wires the verifier in additively. */
#include "kb_auth_oidc.h"
#include "kb_verifier.h"
#include "oauth_pkce.h" /* oauth_pkce_base64url_encode */

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* b64url-encode bytes into a fresh malloc'd NUL-terminated string. */
static char *b64url(const unsigned char *in, size_t inlen)
{
   char *out = malloc(inlen * 2 + 8);
   assert(out);
   assert(oauth_pkce_base64url_encode(in, inlen, out, inlen * 2 + 8) == 0);
   return out;
}

static char *b64url_str(const char *s)
{
   return b64url((const unsigned char *)s, strlen(s));
}

/* Build a JWKS JSON ({"keys":[{kty,kid,n,e}]}) from the public part of pkey. */
static char *make_jwks(EVP_PKEY *pkey, const char *kid)
{
   BIGNUM *n = NULL, *e = NULL;
   assert(EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n) == 1);
   assert(EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e) == 1);
   unsigned char nbuf[1024], ebuf[16];
   int nlen = BN_bn2bin(n, nbuf);
   int elen = BN_bn2bin(e, ebuf);
   BN_free(n);
   BN_free(e);
   char *n64 = b64url(nbuf, (size_t)nlen);
   char *e64 = b64url(ebuf, (size_t)elen);
   char *jwks = malloc(strlen(n64) + strlen(e64) + strlen(kid) + 128);
   assert(jwks);
   sprintf(jwks, "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"%s\",\"n\":\"%s\",\"e\":\"%s\"}]}", kid,
           n64, e64);
   free(n64);
   free(e64);
   return jwks;
}

/* Sign "header_b64.payload_b64" with RS256 and assemble the compact JWS. The
 * header is provided raw so tests can forge alg/kid. */
static char *make_jwt(EVP_PKEY *pkey, const char *header_json, const char *payload_json)
{
   char *h64 = b64url_str(header_json);
   char *p64 = b64url_str(payload_json);
   char *input = malloc(strlen(h64) + strlen(p64) + 2);
   assert(input);
   sprintf(input, "%s.%s", h64, p64);

   EVP_MD_CTX *md = EVP_MD_CTX_new();
   assert(md);
   assert(EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, pkey) == 1);
   size_t siglen = 0;
   assert(EVP_DigestSign(md, NULL, &siglen, (unsigned char *)input, strlen(input)) == 1);
   unsigned char *sig = malloc(siglen);
   assert(sig);
   assert(EVP_DigestSign(md, sig, &siglen, (unsigned char *)input, strlen(input)) == 1);
   EVP_MD_CTX_free(md);

   char *s64 = b64url(sig, siglen);
   char *jwt = malloc(strlen(input) + strlen(s64) + 2);
   assert(jwt);
   sprintf(jwt, "%s.%s", input, s64);
   free(sig);
   free(s64);
   free(input);
   free(h64);
   free(p64);
   return jwt;
}

static const long NOW = 1780000000L;
static const char *HDR = "{\"alg\":\"RS256\",\"kid\":\"test-key\"}";

/* Mint a JWT from `payload` and verify it against cfg at NOW; returns the verify
 * result (1 accept / 0 reject). */
static int verify_pl(EVP_PKEY *key, const kb_oidc_config_t *cfg, const char *payload)
{
   char *jwt = make_jwt(key, HDR, payload);
   kb_verify_result_t r;
   int rc = kb_oidc_verify_jwt(jwt, cfg, NOW, &r);
   free(jwt);
   return rc;
}

static void test_accept_and_scope(EVP_PKEY *key, const char *jwks)
{
   kb_oidc_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.issuer, sizeof(cfg.issuer), "https://idp.example.com");
   snprintf(cfg.audience, sizeof(cfg.audience), "aimee-kb");
   snprintf(cfg.scope_claim, sizeof(cfg.scope_claim), "project");
   snprintf(cfg.scope_kind, sizeof(cfg.scope_kind), "project");
   snprintf(cfg.jwks_json, sizeof(cfg.jwks_json), "%s", jwks);

   char payload[512];
   sprintf(payload,
           "{\"iss\":\"https://idp.example.com\",\"aud\":\"aimee-kb\",\"sub\":\"user-42\","
           "\"project\":\"alpha\",\"iat\":%ld,\"exp\":%ld}",
           NOW, NOW + 3600);
   char *jwt = make_jwt(key, HDR, payload);

   kb_verify_result_t r;
   assert(kb_oidc_verify_jwt(jwt, &cfg, NOW, &r) == 1);
   assert(strcmp(r.subject, "user-42") == 0);
   assert(strcmp(r.scope_kind, "project") == 0);
   assert(strcmp(r.scope_id, "alpha") == 0);
   assert(r.expiry == NOW + 3600);

   /* aud as a JSON array containing the audience is accepted. */
   char payload2[512];
   sprintf(payload2,
           "{\"iss\":\"https://idp.example.com\",\"aud\":[\"other\",\"aimee-kb\"],"
           "\"sub\":\"u\",\"iat\":%ld,\"exp\":%ld}",
           NOW, NOW + 10);
   char *jwt2 = make_jwt(key, HDR, payload2);
   assert(kb_oidc_verify_jwt(jwt2, &cfg, NOW, &r) == 1);
   /* No "project" claim -> unscoped identity. */
   assert(r.scope_kind[0] == '\0');

   free(jwt);
   free(jwt2);
   printf("  accept_and_scope: ok\n");
}

static void test_reject_paths(EVP_PKEY *key, const char *jwks)
{
   kb_oidc_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.issuer, sizeof(cfg.issuer), "https://idp.example.com");
   snprintf(cfg.audience, sizeof(cfg.audience), "aimee-kb");
   snprintf(cfg.jwks_json, sizeof(cfg.jwks_json), "%s", jwks);
   kb_verify_result_t r;
   char payload[512];

   /* expired */
   sprintf(payload, "{\"iss\":\"https://idp.example.com\",\"aud\":\"aimee-kb\",\"exp\":%ld}",
           NOW - 3600);
   char *jwt = make_jwt(key, HDR, payload);
   assert(kb_oidc_verify_jwt(jwt, &cfg, NOW, &r) == 0);
   free(jwt);

   /* wrong issuer */
   sprintf(payload,
           "{\"iss\":\"https://evil.example.com\",\"aud\":\"aimee-kb\",\"iat\":%ld,\"exp\":%ld}",
           NOW, NOW + 3600);
   jwt = make_jwt(key, HDR, payload);
   assert(kb_oidc_verify_jwt(jwt, &cfg, NOW, &r) == 0);
   free(jwt);

   /* wrong audience */
   sprintf(payload,
           "{\"iss\":\"https://idp.example.com\",\"aud\":\"someone-else\",\"iat\":%ld,\"exp\":%ld}",
           NOW, NOW + 3600);
   jwt = make_jwt(key, HDR, payload);
   assert(kb_oidc_verify_jwt(jwt, &cfg, NOW, &r) == 0);
   free(jwt);

   /* missing exp */
   jwt = make_jwt(key, HDR, "{\"iss\":\"https://idp.example.com\",\"aud\":\"aimee-kb\"}");
   assert(kb_oidc_verify_jwt(jwt, &cfg, NOW, &r) == 0);
   free(jwt);

   /* alg confusion: header says "none" */
   sprintf(payload, "{\"iss\":\"https://idp.example.com\",\"aud\":\"aimee-kb\",\"exp\":%ld}",
           NOW + 3600);
   jwt = make_jwt(key, "{\"alg\":\"none\",\"kid\":\"test-key\"}", payload);
   assert(kb_oidc_verify_jwt(jwt, &cfg, NOW, &r) == 0);
   free(jwt);

   /* unknown kid */
   jwt = make_jwt(key, "{\"alg\":\"RS256\",\"kid\":\"other-key\"}", payload);
   assert(kb_oidc_verify_jwt(jwt, &cfg, NOW, &r) == 0);
   free(jwt);

   /* tampered payload: keep a valid signature but swap the payload segment */
   {
      char *good = make_jwt(key, HDR, payload);
      char forged_payload[512];
      sprintf(forged_payload,
              "{\"iss\":\"https://idp.example.com\",\"aud\":\"aimee-kb\",\"sub\":\"admin\",\"exp\":"
              "%ld}",
              NOW + 3600);
      char *fp64 = b64url_str(forged_payload);
      /* good = h64.p64.s64 ; replace p64 with fp64, keep h64 + s64 */
      char *d1 = strchr(good, '.');
      char *d2 = strchr(d1 + 1, '.');
      char tampered[2048];
      int hlen = (int)(d1 - good);
      sprintf(tampered, "%.*s.%s%s", hlen, good, fp64, d2);
      assert(kb_oidc_verify_jwt(tampered, &cfg, NOW, &r) == 0);
      free(fp64);
      free(good);
   }

   /* signed by a different key (signature valid but not in the JWKS) */
   {
      EVP_PKEY *other = EVP_RSA_gen(2048);
      assert(other);
      jwt = make_jwt(other, HDR, payload);
      assert(kb_oidc_verify_jwt(jwt, &cfg, NOW, &r) == 0);
      free(jwt);
      EVP_PKEY_free(other);
   }

   /* malformed: not three segments */
   assert(kb_oidc_verify_jwt("only.two", &cfg, NOW, &r) == 0);
   assert(kb_oidc_verify_jwt("", &cfg, NOW, &r) == 0);
   printf("  reject_paths: ok\n");
}

/* The OIDC verifier registers additively behind the owner kb-token verifier. */
static void test_seam_registration(EVP_PKEY *key, const char *jwks)
{
   kb_verifier_reset();
   kb_oidc_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.audience, sizeof(cfg.audience), "aimee-kb");
   snprintf(cfg.jwks_json, sizeof(cfg.jwks_json), "%s", jwks);
   assert(kb_oidc_verifier_register(&cfg) == 0);

   char payload[256];
   sprintf(payload, "{\"aud\":\"aimee-kb\",\"sub\":\"u\",\"iat\":%ld,\"exp\":%ld}",
           (long)time(NULL), (long)2000000000L);
   char *jwt = make_jwt(key, HDR, payload);

   kb_verify_result_t r;
   char which[32] = "";
   /* The owner token still wins first. */
   assert(kb_verifier_authenticate("owner-tok", "owner-tok", &r, which, sizeof(which)) == 1);
   assert(strcmp(which, "kb-token") == 0);
   /* A JWT the owner verifier rejects is accepted by the OIDC verifier. */
   assert(kb_verifier_authenticate(jwt, "owner-tok", &r, which, sizeof(which)) == 1);
   assert(strcmp(which, "oidc") == 0);
   assert(strcmp(r.subject, "u") == 0);
   /* Junk is rejected by both. */
   assert(kb_verifier_authenticate("junk.junk.junk", "owner-tok", &r, which, sizeof(which)) == 0);

   free(jwt);
   kb_verifier_reset();
   printf("  seam_registration: ok\n");
}

/* kb_oidc_register_from_file reads the JWKS off disk and wires it into the seam,
 * with the claim policy applied. Also covers the config-error paths. */
static void test_register_from_file(EVP_PKEY *key, const char *jwks)
{
   kb_verifier_reset();

   char path[128];
   sprintf(path, "/tmp/aimee_oidc_jwks_%d.json", (int)getpid());
   FILE *f = fopen(path, "wb");
   assert(f);
   assert(fwrite(jwks, 1, strlen(jwks), f) == strlen(jwks));
   fclose(f);

   /* issuer/audience/scope policy is applied from the args. */
   assert(kb_oidc_register_from_file(path, "https://idp.example.com", "aimee-kb", "project",
                                     "project") == 0);

   char payload[512];
   sprintf(payload,
           "{\"iss\":\"https://idp.example.com\",\"aud\":\"aimee-kb\",\"sub\":\"u9\","
           "\"project\":\"beta\",\"iat\":%ld,\"exp\":%ld}",
           (long)time(NULL), (long)2000000000L);
   char *jwt = make_jwt(key, HDR, payload);

   kb_verify_result_t r;
   char which[32] = "";
   assert(kb_verifier_authenticate(jwt, "owner-tok", &r, which, sizeof(which)) == 1);
   assert(strcmp(which, "oidc") == 0);
   assert(strcmp(r.subject, "u9") == 0);
   assert(strcmp(r.scope_kind, "project") == 0 && strcmp(r.scope_id, "beta") == 0);
   /* A JWT with the wrong issuer is rejected (policy came from the file load). */
   sprintf(payload,
           "{\"iss\":\"https://evil.example.com\",\"aud\":\"aimee-kb\",\"iat\":%ld,\"exp\":%ld}",
           (long)time(NULL), (long)2000000000L);
   char *bad = make_jwt(key, HDR, payload);
   assert(kb_verifier_authenticate(bad, "owner-tok", &r, which, sizeof(which)) == 0);

   /* config-error paths: missing path, nonexistent file, empty file. */
   assert(kb_oidc_register_from_file(NULL, NULL, NULL, NULL, NULL) == -1);
   assert(kb_oidc_register_from_file("/no/such/aimee/jwks.json", NULL, NULL, NULL, NULL) == -1);
   char empty[128];
   sprintf(empty, "/tmp/aimee_oidc_empty_%d.json", (int)getpid());
   FILE *ef = fopen(empty, "wb");
   assert(ef);
   fclose(ef);
   assert(kb_oidc_register_from_file(empty, NULL, NULL, NULL, NULL) == -1);

   /* oversized file (> the 8192-byte jwks_json buffer) is a config error. */
   char big[128];
   sprintf(big, "/tmp/aimee_oidc_big_%d.json", (int)getpid());
   FILE *bf = fopen(big, "wb");
   assert(bf);
   for (int i = 0; i < 9000; i++)
      fputc('x', bf);
   fclose(bf);
   assert(kb_oidc_register_from_file(big, NULL, NULL, NULL, NULL) == -1);

   free(jwt);
   free(bad);
   remove(path);
   remove(empty);
   remove(big);
   kb_verifier_reset();
   printf("  register_from_file: ok\n");
}

/* P1 I9: the hard token-age ceiling (now - iat), enforced regardless of exp. */
static void test_iat_ceiling(EVP_PKEY *key, const char *jwks)
{
   kb_oidc_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.issuer, sizeof(cfg.issuer), "https://idp.example.com");
   snprintf(cfg.audience, sizeof(cfg.audience), "aimee-kb");
   snprintf(cfg.jwks_json, sizeof(cfg.jwks_json), "%s", jwks);
   cfg.max_token_age_secs = 900; /* 15 min */
   char pl[512];
   const char *ISS_AUD = "\"iss\":\"https://idp.example.com\",\"aud\":\"aimee-kb\"";

/* helper: mint {iss,aud,iat,exp} and verify at NOW */
#define VERIFY_IAT(iat_v, exp_v)                                                                   \
   (sprintf(pl, "{%s,\"iat\":%ld,\"exp\":%ld}", ISS_AUD, (long)(iat_v), (long)(exp_v)),            \
    verify_pl(key, &cfg, pl))

   /* fresh token: iat = now, exp far out -> accept */
   assert(VERIFY_IAT(NOW, NOW + 3600) == 1);
   /* exactly at the ceiling boundary (now - iat == 900) -> accept */
   assert(VERIFY_IAT(NOW - 900, NOW + 3600) == 1);
   /* OVER the ceiling (age 901) even though exp is far in the future -> reject */
   assert(VERIFY_IAT(NOW - 901, NOW + 100000) == 0);
   /* future iat beyond skew (now + 61) -> reject (no underflow) */
   assert(VERIFY_IAT(NOW + 61, NOW + 3600) == 0);
   /* future iat within skew (now + 60) -> accept */
   assert(VERIFY_IAT(NOW + 60, NOW + 3600) == 1);
   /* missing iat -> reject */
   sprintf(pl, "{%s,\"exp\":%ld}", ISS_AUD, (long)(NOW + 3600));
   assert(verify_pl(key, &cfg, pl) == 0);
   /* malformed (string) iat -> reject */
   sprintf(pl, "{%s,\"iat\":\"soon\",\"exp\":%ld}", ISS_AUD, (long)(NOW + 3600));
   assert(verify_pl(key, &cfg, pl) == 0);
#undef VERIFY_IAT
   printf("  iat_ceiling: ok\n");
}

/* The service-connection profile is narrower than ordinary OIDC. It requires
 * a complete issuer+audience policy, a subject, and typ=at+jwt in the signed
 * protected header so a management JWT cannot be replayed as an access token. */
static void test_service_profile(EVP_PKEY *key, const char *jwks)
{
   kb_oidc_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.issuer, sizeof(cfg.issuer), "https://idp.example.com");
   snprintf(cfg.audience, sizeof(cfg.audience), "aimee-kb");
   snprintf(cfg.jwks_json, sizeof(cfg.jwks_json), "%s", jwks);
   assert(kb_oidc_verifier_register(&cfg) == 0);
   assert(kb_oidc_service_mode() == 1);

   char payload[512];
   sprintf(payload,
           "{\"iss\":\"https://idp.example.com\",\"aud\":\"aimee-kb\","
           "\"sub\":\"aimee-server\",\"iat\":%ld,\"exp\":%ld}",
           NOW, NOW + 3600);
   kb_verify_result_t result;
   char *jwt =
       make_jwt(key, "{\"alg\":\"RS256\",\"typ\":\"at+jwt\",\"kid\":\"test-key\"}", payload);
   assert(kb_oidc_verify_service_token(jwt, NOW, &result) == 1);
   assert(strcmp(result.subject, "aimee-server") == 0);
   free(jwt);

   const char *bad_headers[] = {
       "{\"alg\":\"RS256\",\"kid\":\"test-key\"}",
       "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"test-key\"}",
       "{\"alg\":\"RS256\",\"typ\":\"at+jwt\",\"typ\":\"at+jwt\",\"kid\":\"test-key\"}",
   };
   for (size_t i = 0; i < sizeof(bad_headers) / sizeof(bad_headers[0]); ++i)
   {
      jwt = make_jwt(key, bad_headers[i], payload);
      assert(kb_oidc_verify_service_token(jwt, NOW, &result) == 0);
      free(jwt);
   }

   sprintf(payload,
           "{\"iss\":\"https://idp.example.com\",\"aud\":\"wrong\","
           "\"sub\":\"aimee-server\",\"iat\":%ld,\"exp\":%ld}",
           NOW, NOW + 3600);
   jwt = make_jwt(key, "{\"alg\":\"RS256\",\"typ\":\"at+jwt\",\"kid\":\"test-key\"}", payload);
   assert(kb_oidc_verify_service_token(jwt, NOW, &result) == 0);
   free(jwt);

   sprintf(payload,
           "{\"iss\":\"https://idp.example.com\",\"aud\":\"aimee-kb\","
           "\"iat\":%ld,\"exp\":%ld}",
           NOW, NOW + 3600);
   jwt = make_jwt(key, "{\"alg\":\"RS256\",\"typ\":\"at+jwt\",\"kid\":\"test-key\"}", payload);
   assert(kb_oidc_verify_service_token(jwt, NOW, &result) == 0);
   free(jwt);

   cfg.audience[0] = '\0';
   assert(kb_oidc_verifier_register(&cfg) == 0);
   assert(kb_oidc_service_mode() == -1);
   printf("  service_profile: ok\n");
}

int main(void)
{
   printf("kb_auth_oidc:\n");
   EVP_PKEY *key = EVP_RSA_gen(2048);
   assert(key);
   char *jwks = make_jwks(key, "test-key");

   test_accept_and_scope(key, jwks);
   test_reject_paths(key, jwks);
   test_iat_ceiling(key, jwks);
   test_seam_registration(key, jwks);
   test_service_profile(key, jwks);
   test_register_from_file(key, jwks);

   free(jwks);
   EVP_PKEY_free(key);
   printf("All kb_auth_oidc tests passed.\n");
   return 0;
}
