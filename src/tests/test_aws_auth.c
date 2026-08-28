/* test_aws_auth.c: P6a AWS-auth core unit tests (pure/offline). Covers:
 *   (a) SigV4 vs the PUBLISHED AWS aws-sig-v4-test-suite vectors (get-vanilla,
 *       get-vanilla-query-order-key-case, get-header-value-trim, normalize-path/
 *       get-space) — canonical-request hash + string-to-sign + final signature
 *       matched EXACTLY; plus the three payload modes (exact-bytes / empty / UNSIGNED).
 *   (b) web-identity JWT validation — VERIFIES THE SIGNATURE (RS256) against an
 *       in-test JWKS: valid accepted; wrong-signature / wrong-iss / wrong-aud /
 *       expired rejected.
 *   (c) AssumeRole (has ExternalId) vs AssumeRoleWithWebIdentity (NO ExternalId);
 *       both DurationSeconds=900.
 *   (d) STS XML parse — valid; missing-field error; duplicate-field error; trailing
 *       alternate <Credentials> rejected (never leaks the alternate); XXE rejected.
 *   (e) bedrock_session_policy for all five target types — correct action (never
 *       bedrock:Converse) + exact ARN set; fail-closed (unknown / missing region /
 *       missing FMs) never emitting a "*" resource or "InvokeModel*".
 *   (f) STS cache isolation negative matrix + TTL expiry + generation-bump.
 *
 * NOTE: the aws-sig-v4-test-suite secret is "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"
 * (with '+'); the "/"-variant is the S3-docs key and does NOT reproduce the vectors. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include "modules/aws/aws_sigv4.h"
#include "modules/aws/aws_sts.h"
#include "modules/aws/bedrock_policy.h"
#include "modules/aws/sts_cache.h"

/* ============================ (a) SigV4 vectors ============================ */

#define TS_SECRET "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"
#define TS_AKID   "AKIDEXAMPLE"
#define TS_DATE   "20150830"
#define TS_AMZ    "20150830T123600Z"
#define TS_REGION "us-east-1"
#define TS_SVC    "service"

static int sign_test(const aws_sigv4_request_t *request, aws_sigv4_result_t *result)
{
   aws_sigv4_request_t req = *request;
   if (!req.access_key_id_len && req.access_key_id)
      req.access_key_id_len = strlen(req.access_key_id);
   if (!req.secret_access_key_len && req.secret_access_key)
      req.secret_access_key_len = strlen(req.secret_access_key);
   if (!req.session_token_len && req.session_token)
      req.session_token_len = strlen(req.session_token);
   return aws_sigv4_sign(&req, result);
}

static void check_vector(const char *name, const aws_sigv4_request_t *req, const char *want_crhash,
                         const char *want_sts, const char *want_sig)
{
   aws_sigv4_result_t r;
   int rc = sign_test(req, &r);
   assert(rc == 0);
   if (strcmp(r.canonical_request_hash, want_crhash) != 0)
   {
      fprintf(stderr, "  [%s] CR HASH mismatch\n    got:  %s\n    want: %s\n    CR:\n%s\n", name,
              r.canonical_request_hash, want_crhash, r.canonical_request);
      assert(0);
   }
   if (strcmp(r.string_to_sign, want_sts) != 0)
   {
      fprintf(stderr, "  [%s] STS mismatch\n    got:  %s\n    want: %s\n", name, r.string_to_sign,
              want_sts);
      assert(0);
   }
   if (strcmp(r.signature, want_sig) != 0)
   {
      fprintf(stderr, "  [%s] SIG mismatch\n    got:  %s\n    want: %s\n", name, r.signature,
              want_sig);
      assert(0);
   }
   printf("  sigv4 vector [%s]: exact match\n", name);
}

static void test_sigv4_vectors(void)
{
   const char *empty = AWS_SIGV4_EMPTY_BODY_SHA256;

   /* get-vanilla */
   {
      aws_sigv4_kv_t h[] = {{"Host", "example.amazonaws.com"}, {"X-Amz-Date", TS_AMZ}};
      aws_sigv4_request_t req = {.method = "GET",
                                 .raw_path = "/",
                                 .headers = h,
                                 .n_headers = 2,
                                 .payload_hash = empty,
                                 .amz_date = TS_AMZ,
                                 .date = TS_DATE,
                                 .region = TS_REGION,
                                 .service = TS_SVC,
                                 .access_key_id = TS_AKID,
                                 .secret_access_key = TS_SECRET};
      check_vector(
          "get-vanilla", &req, "bb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63",
          "AWS4-HMAC-SHA256\n" TS_AMZ "\n" TS_DATE "/" TS_REGION "/" TS_SVC "/aws4_request\n"
          "bb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63",
          "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");
   }

   /* get-vanilla-query-order-key-case (input already-sorted; asserts canonical form) */
   {
      aws_sigv4_kv_t h[] = {{"Host", "example.amazonaws.com"}, {"X-Amz-Date", TS_AMZ}};
      aws_sigv4_kv_t q[] = {{"Param1", "value1"}, {"Param2", "value2"}};
      aws_sigv4_request_t req = {.method = "GET",
                                 .raw_path = "/",
                                 .query = q,
                                 .n_query = 2,
                                 .headers = h,
                                 .n_headers = 2,
                                 .payload_hash = empty,
                                 .amz_date = TS_AMZ,
                                 .date = TS_DATE,
                                 .region = TS_REGION,
                                 .service = TS_SVC,
                                 .access_key_id = TS_AKID,
                                 .secret_access_key = TS_SECRET};
      check_vector("get-vanilla-query-order-key-case", &req,
                   "816cd5b414d056048ba4f7c5386d6e0533120fb1fcfa93762cf0fc39e2cf19e0",
                   "AWS4-HMAC-SHA256\n" TS_AMZ "\n" TS_DATE "/" TS_REGION "/" TS_SVC
                   "/aws4_request\n"
                   "816cd5b414d056048ba4f7c5386d6e0533120fb1fcfa93762cf0fc39e2cf19e0",
                   "b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500");
   }

   /* get-header-value-trim (inner-whitespace collapse; my-header2 value "a   b   c") */
   {
      aws_sigv4_kv_t h[] = {{"Host", "example.amazonaws.com"},
                            {"My-Header1", "value1"},
                            {"My-Header2", "\"a   b   c\""},
                            {"X-Amz-Date", TS_AMZ}};
      aws_sigv4_request_t req = {.method = "GET",
                                 .raw_path = "/",
                                 .headers = h,
                                 .n_headers = 4,
                                 .payload_hash = empty,
                                 .amz_date = TS_AMZ,
                                 .date = TS_DATE,
                                 .region = TS_REGION,
                                 .service = TS_SVC,
                                 .access_key_id = TS_AKID,
                                 .secret_access_key = TS_SECRET};
      check_vector("get-header-value-trim", &req,
                   "a726db9b0df21c14f559d0a978e563112acb1b9e05476f0a6a1c7d68f28605c7",
                   "AWS4-HMAC-SHA256\n" TS_AMZ "\n" TS_DATE "/" TS_REGION "/" TS_SVC
                   "/aws4_request\n"
                   "a726db9b0df21c14f559d0a978e563112acb1b9e05476f0a6a1c7d68f28605c7",
                   "acc3ed3afb60bb290fc8d2dd0098b9911fcaa05412b367055dee359757a9c736");
   }

   /* normalize-path/get-space — RAW path "/example space/" -> "/example%20space/" */
   {
      aws_sigv4_kv_t h[] = {{"Host", "example.amazonaws.com"}, {"X-Amz-Date", TS_AMZ}};
      aws_sigv4_request_t req = {.method = "GET",
                                 .raw_path = "/example space/",
                                 .headers = h,
                                 .n_headers = 2,
                                 .payload_hash = empty,
                                 .amz_date = TS_AMZ,
                                 .date = TS_DATE,
                                 .region = TS_REGION,
                                 .service = TS_SVC,
                                 .access_key_id = TS_AKID,
                                 .secret_access_key = TS_SECRET};
      aws_sigv4_result_t r;
      assert(sign_test(&req, &r) == 0);
      /* second line of the canonical request is the encoded URI */
      assert(strstr(r.canonical_request, "\n/example%20space/\n") != NULL);
      check_vector("normalize-path/get-space", &req,
                   "63ee75631ed7234ae61b5f736dfc7754cdccfedbff4b5128a915706ee9390d86",
                   "AWS4-HMAC-SHA256\n" TS_AMZ "\n" TS_DATE "/" TS_REGION "/" TS_SVC
                   "/aws4_request\n"
                   "63ee75631ed7234ae61b5f736dfc7754cdccfedbff4b5128a915706ee9390d86",
                   "652487583200325589f1fba4c7e578f72c47cb61beeca81406b39ddec1366741");
   }
}

static void test_sigv4_payload_modes(void)
{
   /* empty-body digest is the documented constant. */
   char h_empty[65];
   aws_sha256_hex((const unsigned char *)"", 0, h_empty);
   assert(strcmp(h_empty, AWS_SIGV4_EMPTY_BODY_SHA256) == 0);

   /* exact-body SHA differs from empty and is deterministic. */
   char h_body[65];
   aws_sha256_hex((const unsigned char *)"hello", 5, h_body);
   assert(strcmp(h_body, AWS_SIGV4_EMPTY_BODY_SHA256) != 0);
   assert(strcmp(h_body, AWS_SIGV4_UNSIGNED_PAYLOAD) != 0);

   /* The three modes yield DISTINCT signed canonical requests. */
   aws_sigv4_kv_t h[] = {{"Host", "bedrock.us-east-1.amazonaws.com"}, {"X-Amz-Date", TS_AMZ}};
   const char *modes[3] = {h_body, AWS_SIGV4_EMPTY_BODY_SHA256, AWS_SIGV4_UNSIGNED_PAYLOAD};
   char sigs[3][65];
   for (int i = 0; i < 3; i++)
   {
      aws_sigv4_request_t req = {.method = "POST",
                                 .raw_path = "/model/x/invoke",
                                 .headers = h,
                                 .n_headers = 2,
                                 .payload_hash = modes[i],
                                 .amz_date = TS_AMZ,
                                 .date = TS_DATE,
                                 .region = TS_REGION,
                                 .service = "bedrock",
                                 .access_key_id = TS_AKID,
                                 .secret_access_key = TS_SECRET};
      aws_sigv4_result_t r;
      assert(sign_test(&req, &r) == 0);
      /* the exact hashed-payload appears as the last line of the canonical request */
      assert(strstr(r.canonical_request, modes[i]) != NULL);
      snprintf(sigs[i], sizeof(sigs[i]), "%s", r.signature);
   }
   assert(strcmp(sigs[0], sigs[1]) != 0);
   assert(strcmp(sigs[1], sigs[2]) != 0);
   assert(strcmp(sigs[0], sigs[2]) != 0);
   printf("  sigv4 payload modes: distinct + correct\n");
}

static void test_sigv4_security_token(void)
{
   /* When a session token is present it is signed as x-amz-security-token and echoed. */
   aws_sigv4_kv_t h[] = {{"Host", "sts.amazonaws.com"}, {"X-Amz-Date", TS_AMZ}};
   aws_sigv4_request_t req = {.method = "POST",
                              .raw_path = "/",
                              .headers = h,
                              .n_headers = 2,
                              .payload_hash = AWS_SIGV4_EMPTY_BODY_SHA256,
                              .amz_date = TS_AMZ,
                              .date = TS_DATE,
                              .region = TS_REGION,
                              .service = "sts",
                              .access_key_id = TS_AKID,
                              .secret_access_key = TS_SECRET,
                              .session_token = "FQoGZXIvYXdzTOKEN=="};
   aws_sigv4_result_t r;
   assert(sign_test(&req, &r) == 0);
   assert(r.has_security_token == 1);
   assert(strstr(r.signed_headers, "x-amz-security-token") != NULL);
   assert(strcmp(r.security_token, "FQoGZXIvYXdzTOKEN==") == 0);
   assert(strstr(r.canonical_request, "x-amz-security-token:FQoGZXIvYXdzTOKEN==") != NULL);
   printf("  sigv4 security-token signed + emitted: ok\n");
}

/* =============== (b) web-identity JWT verify (RS256, in-test) =============== */

static char *b64url(const unsigned char *in, size_t len)
{
   static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   char *out = malloc(((len + 2) / 3) * 4 + 1);
   size_t o = 0;
   for (size_t i = 0; i < len; i += 3)
   {
      unsigned int n = in[i] << 16;
      if (i + 1 < len)
         n |= in[i + 1] << 8;
      if (i + 2 < len)
         n |= in[i + 2];
      out[o++] = t[(n >> 18) & 63];
      out[o++] = t[(n >> 12) & 63];
      if (i + 1 < len)
         out[o++] = t[(n >> 6) & 63];
      if (i + 2 < len)
         out[o++] = t[n & 63];
   }
   out[o] = '\0';
   return out;
}

static char *b64url_str(const char *s)
{
   return b64url((const unsigned char *)s, strlen(s));
}

static EVP_PKEY *gen_rsa(void)
{
   EVP_PKEY *pkey = NULL;
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
   assert(ctx);
   assert(EVP_PKEY_keygen_init(ctx) > 0);
   assert(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0);
   assert(EVP_PKEY_keygen(ctx, &pkey) > 0);
   EVP_PKEY_CTX_free(ctx);
   return pkey;
}

/* Build a single-key RSA JWKS document for `pkey` with kid "k1". */
static char *rsa_jwks(EVP_PKEY *pkey)
{
   BIGNUM *n = NULL, *e = NULL;
   assert(EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n));
   assert(EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e));
   unsigned char nb[512], eb[16];
   int nl = BN_bn2bin(n, nb);
   int el = BN_bn2bin(e, eb);
   BN_free(n);
   BN_free(e);
   char *ns = b64url(nb, (size_t)nl);
   char *es = b64url(eb, (size_t)el);
   char *out = malloc(strlen(ns) + strlen(es) + 128);
   sprintf(out, "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"k1\",\"n\":\"%s\",\"e\":\"%s\"}]}", ns, es);
   free(ns);
   free(es);
   return out;
}

/* Assemble a compact RS256 JWS: base64url(header).base64url(payload).sig, signed
 * with `signer` (which may differ from the JWKS key to forge a bad signature). */
static char *make_jwt(const char *payload_json, EVP_PKEY *signer)
{
   char *h = b64url_str("{\"alg\":\"RS256\",\"kid\":\"k1\",\"typ\":\"JWT\"}");
   char *p = b64url_str(payload_json);
   char *si = malloc(strlen(h) + strlen(p) + 2);
   sprintf(si, "%s.%s", h, p);

   EVP_MD_CTX *md = EVP_MD_CTX_new();
   assert(EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, signer) == 1);
   size_t siglen = 0;
   assert(EVP_DigestSign(md, NULL, &siglen, (const unsigned char *)si, strlen(si)) == 1);
   unsigned char *sig = malloc(siglen);
   assert(EVP_DigestSign(md, sig, &siglen, (const unsigned char *)si, strlen(si)) == 1);
   EVP_MD_CTX_free(md);

   char *sb = b64url(sig, siglen);
   char *jwt = malloc(strlen(si) + strlen(sb) + 2);
   sprintf(jwt, "%s.%s", si, sb);
   free(h);
   free(p);
   free(si);
   free(sig);
   free(sb);
   return jwt;
}

static void test_web_identity(void)
{
   EVP_PKEY *key = gen_rsa();
   EVP_PKEY *other = gen_rsa(); /* a different key -> wrong-signature */
   char *jwks = rsa_jwks(key);
   const char *iss = "https://token.actions.githubusercontent.com";
   const char *aud = "sts.amazonaws.com";
   const long now = 1000000;

   char pl[512];
   snprintf(pl, sizeof(pl),
            "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"repo:acme/app:ref:refs/heads/main\","
            "\"exp\":%ld,\"iat\":%ld}",
            iss, aud, now + 300, now - 10);

   /* valid, correctly signed -> OK */
   {
      char *jwt = make_jwt(pl, key);
      aws_webid_claims_t c;
      aws_webid_status_t st = aws_webidentity_validate(jwt, jwks, iss, aud, now, &c);
      assert(st == AWS_WEBID_OK);
      assert(strcmp(c.issuer, iss) == 0);
      assert(strcmp(c.subject, "repo:acme/app:ref:refs/heads/main") == 0);
      free(jwt);
   }
   /* wrong signature (signed by a different key) -> BAD_SIGNATURE */
   {
      char *jwt = make_jwt(pl, other);
      aws_webid_status_t st = aws_webidentity_validate(jwt, jwks, iss, aud, now, NULL);
      assert(st == AWS_WEBID_ERR_BAD_SIGNATURE);
      free(jwt);
   }
   /* wrong iss -> ERR_ISS */
   {
      char *jwt = make_jwt(pl, key);
      aws_webid_status_t st =
          aws_webidentity_validate(jwt, jwks, "https://evil.example", aud, now, NULL);
      assert(st == AWS_WEBID_ERR_ISS);
      free(jwt);
   }
   /* wrong aud -> ERR_AUD */
   {
      char *jwt = make_jwt(pl, key);
      aws_webid_status_t st = aws_webidentity_validate(jwt, jwks, iss, "wrong-audience", now, NULL);
      assert(st == AWS_WEBID_ERR_AUD);
      free(jwt);
   }
   /* expired -> ERR_EXPIRED */
   {
      char plx[512];
      snprintf(plx, sizeof(plx),
               "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"s\",\"exp\":%ld,\"iat\":%ld}", iss, aud,
               now - 500, now - 800);
      char *jwt = make_jwt(plx, key);
      aws_webid_status_t st = aws_webidentity_validate(jwt, jwks, iss, aud, now, NULL);
      assert(st == AWS_WEBID_ERR_EXPIRED);
      free(jwt);
   }
   /* garbage token -> MALFORMED (never a spurious OK) */
   {
      aws_webid_status_t st = aws_webidentity_validate("not-a-jwt", jwks, iss, aud, now, NULL);
      assert(st != AWS_WEBID_OK);
   }
   /* Nonfinite/out-of-long-range NumericDates are rejected before conversion and
    * every failure clears a caller-provided claims object. */
   {
      const char *bad_dates[] = {
          "{\"iss\":\"https://token.actions.githubusercontent.com\","
          "\"aud\":\"sts.amazonaws.com\",\"sub\":\"s\",\"exp\":1e999,\"iat\":1}",
          "{\"iss\":\"https://token.actions.githubusercontent.com\","
          "\"aud\":\"sts.amazonaws.com\",\"sub\":\"s\",\"exp\":1e20,\"iat\":1}",
          "{\"iss\":\"https://token.actions.githubusercontent.com\","
          "\"aud\":\"sts.amazonaws.com\",\"sub\":\"s\",\"exp\":1000300,\"iat\":1e999}",
      };
      for (size_t i = 0; i < sizeof(bad_dates) / sizeof(bad_dates[0]); ++i)
      {
         char *jwt = make_jwt(bad_dates[i], key);
         aws_webid_claims_t claims;
         memset(&claims, 0xa5, sizeof(claims));
         assert(aws_webidentity_validate(jwt, jwks, iss, aud, now, &claims) != AWS_WEBID_OK);
         const unsigned char *bytes = (const unsigned char *)&claims;
         for (size_t j = 0; j < sizeof(claims); ++j)
            assert(bytes[j] == 0);
         free(jwt);
      }
   }
   /* Reject an active JSON NUL escape, but not printable literal backslash-u text. */
   {
      char *jwt = make_jwt("{\"iss\":\"https://token.actions.githubusercontent.com\","
                           "\"aud\":\"sts.amazonaws.com\",\"sub\":\"bad\\u0000suffix\","
                           "\"exp\":1000300,\"iat\":999990}",
                           key);
      assert(aws_webidentity_validate(jwt, jwks, iss, aud, now, NULL) != AWS_WEBID_OK);
      free(jwt);
      jwt = make_jwt("{\"iss\":\"https://token.actions.githubusercontent.com\","
                     "\"aud\":\"sts.amazonaws.com\",\"sub\":\"literal\\\\u0000\","
                     "\"exp\":1000300,\"iat\":999990}",
                     key);
      aws_webid_claims_t claims;
      assert(aws_webidentity_validate(jwt, jwks, iss, aud, now, &claims) == AWS_WEBID_OK);
      assert(strcmp(claims.subject, "literal\\u0000") == 0);
      free(jwt);
   }
   /* fail-CLOSED on empty/NULL expected iss or aud: a validly-signed token whose
    * iss/aud match the token itself must still be REJECTED, never accept-any. */
   {
      char *jwt = make_jwt(pl, key);
      assert(aws_webidentity_validate(jwt, jwks, "", aud, now, NULL) != AWS_WEBID_OK);
      assert(aws_webidentity_validate(jwt, jwks, iss, "", now, NULL) != AWS_WEBID_OK);
      assert(aws_webidentity_validate(jwt, jwks, NULL, aud, now, NULL) != AWS_WEBID_OK);
      assert(aws_webidentity_validate(jwt, jwks, iss, NULL, now, NULL) != AWS_WEBID_OK);
      free(jwt);
   }
   free(jwks);
   EVP_PKEY_free(key);
   EVP_PKEY_free(other);
   printf("  web-identity: valid ok; wrong-sig/iss/aud/expired/empty-expected rejected\n");
}

/* ================= (c) STS body construction (mode distinction) ============= */

static void test_sts_bodies(void)
{
   const char *policy = "{\"Version\":\"2012-10-17\"}";
   char b_assume[2048], b_web[2048];

   assert(aws_sts_assume_role_body(b_assume, sizeof(b_assume),
                                   "arn:aws:iam::123456789012:role/egress", "aimee-sess",
                                   "ext-tenant-77", policy) == 0);
   /* AssumeRole DOES carry ExternalId + DurationSeconds=900 */
   assert(strstr(b_assume, "ExternalId=ext-tenant-77") != NULL);
   assert(strstr(b_assume, "DurationSeconds=900") != NULL);
   assert(strstr(b_assume, "Action=AssumeRole") != NULL);

   assert(aws_sts_assume_role_web_identity_body(b_web, sizeof(b_web),
                                                "arn:aws:iam::123456789012:role/egress",
                                                "aimee-sess", "eyJhbGci.header.sig", policy) == 0);
   /* AssumeRoleWithWebIdentity carries NO ExternalId, has the token + 900 */
   assert(strstr(b_web, "ExternalId") == NULL);
   assert(strstr(b_web, "DurationSeconds=900") != NULL);
   assert(strstr(b_web, "Action=AssumeRoleWithWebIdentity") != NULL);
   assert(strstr(b_web, "WebIdentityToken=eyJhbGci.header.sig") != NULL);

   /* mode b requires ExternalId (fail-closed on NULL) */
   assert(aws_sts_assume_role_body(b_assume, sizeof(b_assume), "arn:...:role/x", "s", NULL, NULL) ==
          -1);

   /* the signed AssumeRole helper produces a valid SigV4 over the exact body */
   aws_sts_signed_request_t sr;
   assert(aws_sts_build_signed_assume_role(&sr, TS_REGION, "sts.us-east-1.amazonaws.com", TS_AKID,
                                           TS_SECRET, NULL, "arn:aws:iam::123456789012:role/egress",
                                           "aimee-sess", "ext-77", policy, TS_AMZ, TS_DATE) == 0);
   assert(strstr(sr.sig.authorization, "AWS4-HMAC-SHA256 Credential=" TS_AKID "/") != NULL);
   assert(strstr(sr.sig.signed_headers, "content-type") != NULL);
   assert(strstr(sr.sig.signed_headers, "host") != NULL);
   printf("  sts bodies: AssumeRole has ExternalId; WebIdentity has none; both 900\n");
}

/* ===================== (d) STS XML parse (hostile-safe) ===================== */

static const char *k_valid_xml = "<AssumeRoleResponse><AssumeRoleResult><Credentials>"
                                 "<AccessKeyId>ASIAEXAMPLE</AccessKeyId>"
                                 "<SecretAccessKey>secretPART/plus+chars</SecretAccessKey>"
                                 "<SessionToken>FQoGtokenVALUE==</SessionToken>"
                                 "<Expiration>2026-07-20T12:00:00Z</Expiration>"
                                 "</Credentials></AssumeRoleResult></AssumeRoleResponse>";

static void test_sts_parse(void)
{
   aws_sts_credentials_t c;
   assert(aws_sts_parse_assume_response(k_valid_xml, &c) == 0);
   assert(strcmp(c.access_key_id, "ASIAEXAMPLE") == 0);
   assert(strcmp(c.secret_access_key, "secretPART/plus+chars") == 0);
   assert(strcmp(c.session_token, "FQoGtokenVALUE==") == 0);
   assert(strcmp(c.expiration, "2026-07-20T12:00:00Z") == 0);

   /* missing SessionToken -> error */
   const char *missing =
       "<AssumeRoleResult><Credentials>"
       "<AccessKeyId>A</AccessKeyId><SecretAccessKey>S</SecretAccessKey>"
       "<Expiration>2026-07-20T12:00:00Z</Expiration></Credentials></AssumeRoleResult>";
   assert(aws_sts_parse_assume_response(missing, &c) == -1);

   /* duplicate AccessKeyId within the block -> error */
   const char *dup =
       "<AssumeRoleResult><Credentials>"
       "<AccessKeyId>A</AccessKeyId><AccessKeyId>B</AccessKeyId>"
       "<SecretAccessKey>S</SecretAccessKey><SessionToken>T</SessionToken>"
       "<Expiration>2026-07-20T12:00:00Z</Expiration></Credentials></AssumeRoleResult>";
   assert(aws_sts_parse_assume_response(dup, &c) == -1);

   /* trailing alternate <Credentials> (smuggled) -> rejected, never leaks the alt */
   const char *trailing =
       "<AssumeRoleResult><Credentials>"
       "<AccessKeyId>GOOD</AccessKeyId><SecretAccessKey>S</SecretAccessKey>"
       "<SessionToken>T</SessionToken><Expiration>2026-07-20T12:00:00Z</Expiration>"
       "</Credentials></AssumeRoleResult>"
       "<AssumeRoleResult><Credentials>"
       "<AccessKeyId>EVIL</AccessKeyId><SecretAccessKey>S2</SecretAccessKey>"
       "<SessionToken>T2</SessionToken><Expiration>2026-07-20T12:00:00Z</Expiration>"
       "</Credentials></AssumeRoleResult>";
   memset(&c, 0, sizeof(c));
   int rc = aws_sts_parse_assume_response(trailing, &c);
   assert(rc == -1);
   assert(strcmp(c.access_key_id, "EVIL") != 0); /* the alternate NEVER wins */

   /* XXE / entity declaration -> rejected outright */
   const char *xxe =
       "<!DOCTYPE foo [<!ENTITY x SYSTEM \"file:///etc/passwd\">]>"
       "<AssumeRoleResult><Credentials>"
       "<AccessKeyId>&x;</AccessKeyId><SecretAccessKey>S</SecretAccessKey>"
       "<SessionToken>T</SessionToken><Expiration>E</Expiration></Credentials></AssumeRoleResult>";
   assert(aws_sts_parse_assume_response(xxe, &c) == -1);
   printf("  sts xml: valid ok; missing/dup/trailing/XXE rejected\n");
}

/* ===================== (e) bedrock session policy ========================== */

static void assert_no_wildcard(const char *json)
{
   assert(strstr(json, "\"*\"") == NULL);
   assert(strstr(json, "InvokeModel*") == NULL);
   assert(strstr(json, "bedrock:Converse") == NULL);
}

static int count_text(const char *haystack, const char *needle)
{
   int n = 0;
   size_t z = strlen(needle);
   while ((haystack = strstr(haystack, needle)) != NULL)
   {
      n++;
      haystack += z;
   }
   return n;
}

static void test_bedrock_policy(void)
{
   char out[4096];
   const char *regions1[] = {"us-east-1"};
   const char *regions2[] = {"us-east-1", "us-west-2"};

   /* foundation, non-streaming -> InvokeModel + foundation-model ARN (no account) */
   {
      bedrock_target_t t = {.type = BEDROCK_TARGET_FOUNDATION,
                            .partition = "aws",
                            .invoke_region = "us-east-1",
                            .region_set = regions1,
                            .n_regions = 1,
                            .id = "anthropic.claude-3-sonnet-20240229-v1:0"};
      assert(bedrock_session_policy(&t, 0, out, sizeof(out)) == 0);
      assert(strstr(out, "\"bedrock:InvokeModel\"") != NULL);
      assert(strstr(out, "arn:aws:bedrock:us-east-1::foundation-model/"
                         "anthropic.claude-3-sonnet-20240229-v1:0") != NULL);
      assert_no_wildcard(out);
   }
   /* foundation, streaming -> InvokeModelWithResponseStream */
   {
      bedrock_target_t t = {.type = BEDROCK_TARGET_FOUNDATION,
                            .partition = "aws",
                            .invoke_region = "us-east-1",
                            .region_set = regions1,
                            .n_regions = 1,
                            .id = "meta.llama3"};
      assert(bedrock_session_policy(&t, 1, out, sizeof(out)) == 0);
      assert(strstr(out, "\"bedrock:InvokeModelWithResponseStream\"") != NULL);
      assert(strstr(out, "\"bedrock:InvokeModel\"") == NULL);
      assert_no_wildcard(out);
   }
   /* provisioned -> provisioned-model ARN with account */
   {
      bedrock_target_t t = {.type = BEDROCK_TARGET_PROVISIONED,
                            .partition = "aws",
                            .invoke_region = "us-east-1",
                            .region_set = regions1,
                            .n_regions = 1,
                            .account = "123456789012",
                            .id = "abcd1234"};
      assert(bedrock_session_policy(&t, 0, out, sizeof(out)) == 0);
      assert(strstr(out, "arn:aws:bedrock:us-east-1:123456789012:provisioned-model/abcd1234") !=
             NULL);
      assert_no_wildcard(out);
   }
   /* custom -> custom-model ARN */
   {
      bedrock_target_t t = {.type = BEDROCK_TARGET_CUSTOM,
                            .partition = "aws-us-gov",
                            .invoke_region = "us-east-1",
                            .region_set = regions1,
                            .n_regions = 1,
                            .account = "123456789012",
                            .id = "myft"};
      assert(bedrock_session_policy(&t, 0, out, sizeof(out)) == 0);
      assert(strstr(out, "arn:aws-us-gov:bedrock:us-east-1:123456789012:custom-model/myft") !=
             NULL);
      assert_no_wildcard(out);
   }
   /* application-inference-profile -> profile ARN + underlying FM ARNs */
   {
      const char *fms[] = {"arn:aws:bedrock:us-east-1::foundation-model/anthropic.claude",
                           "arn:aws:bedrock:us-west-2::foundation-model/anthropic.claude"};
      bedrock_target_t t = {.type = BEDROCK_TARGET_APP_INFERENCE_PROFILE,
                            .partition = "aws",
                            .invoke_region = "us-east-1",
                            .region_set = regions2,
                            .n_regions = 2,
                            .account = "123456789012",
                            .id = "myprofile",
                            .underlying_fm_arns = fms,
                            .n_underlying = 2};
      assert(bedrock_session_policy(&t, 0, out, sizeof(out)) == 0);
      assert(strstr(out, "arn:aws:bedrock:us-east-1:123456789012:application-inference-profile/"
                         "myprofile") != NULL);
      assert(strstr(out, "arn:aws:bedrock:us-west-2::foundation-model/anthropic.claude") != NULL);
      assert_no_wildcard(out);
   }
   /* cross-region-inference-profile -> inference-profile ARN + each dest FM ARN */
   {
      const char *fms[] = {"arn:aws:bedrock:us-east-1::foundation-model/anthropic.claude",
                           "arn:aws:bedrock:us-west-2::foundation-model/anthropic.claude"};
      bedrock_target_t t = {.type = BEDROCK_TARGET_CROSS_REGION_INFERENCE_PROFILE,
                            .partition = "aws",
                            .invoke_region = "us-west-2",
                            .region_set = regions2,
                            .n_regions = 2,
                            .account = "123456789012",
                            .id = "us.anthropic.claude",
                            .underlying_fm_arns = fms,
                            .n_underlying = 2};
      assert(bedrock_session_policy(&t, 1, out, sizeof(out)) == 0);
      assert(strstr(out, "inference-profile/us.anthropic.claude") != NULL);
      assert(count_text(out, "arn:aws:bedrock:us-west-2:123456789012:inference-profile/") == 1);
      assert(strstr(out, "arn:aws:bedrock:us-east-1:123456789012:inference-profile/") == NULL);
      assert(strstr(out, "\"bedrock:InvokeModelWithResponseStream\"") != NULL);
      assert_no_wildcard(out);
   }

   /* ---- fail-closed cases: return -1, output empty, never a wildcard ---- */
   {
      /* unknown type */
      bedrock_target_t t = {.type = (bedrock_target_type_t)999,
                            .partition = "aws",
                            .invoke_region = "us-east-1",
                            .region_set = regions1,
                            .n_regions = 1,
                            .id = "x"};
      assert(bedrock_session_policy(&t, 0, out, sizeof(out)) == -1);
      assert(out[0] == '\0');
   }
   {
      /* missing region set */
      bedrock_target_t t = {.type = BEDROCK_TARGET_FOUNDATION,
                            .partition = "aws",
                            .invoke_region = "us-east-1",
                            .n_regions = 0,
                            .id = "x"};
      assert(bedrock_session_policy(&t, 0, out, sizeof(out)) == -1);
      assert(out[0] == '\0');
   }
   {
      /* profile with empty underlying FM set */
      bedrock_target_t t = {.type = BEDROCK_TARGET_CROSS_REGION_INFERENCE_PROFILE,
                            .partition = "aws",
                            .invoke_region = "us-east-1",
                            .region_set = regions1,
                            .n_regions = 1,
                            .account = "123456789012",
                            .id = "p",
                            .n_underlying = 0};
      assert(bedrock_session_policy(&t, 0, out, sizeof(out)) == -1);
      assert(out[0] == '\0');
   }
   {
      /* invalid partition */
      bedrock_target_t t = {.type = BEDROCK_TARGET_FOUNDATION,
                            .partition = "gcp",
                            .invoke_region = "us-east-1",
                            .region_set = regions1,
                            .n_regions = 1,
                            .id = "x"};
      assert(bedrock_session_policy(&t, 0, out, sizeof(out)) == -1);
   }
   {
      /* a profile whose underlying ARN is malformed / cross-service / wildcard ->
       * fail closed (the bad ARN must never leak into the Resource set). */
      const char *bad_wildcard[] = {"arn:aws:bedrock:us-east-1::foundation-model/*"};
      const char *bad_service[] = {"arn:aws:s3:us-east-1::foundation-model/x"};
      const char *bad_shape[] = {"arn:aws:bedrock:us-east-1::provisioned-model/x"};
      const char *bad_account[] = {"arn:aws:bedrock:us-east-1:evil::foundation-model/x"};
      const char *cases[4];
      cases[0] = bad_wildcard[0];
      cases[1] = bad_service[0];
      cases[2] = bad_shape[0];
      cases[3] = bad_account[0];
      for (int i = 0; i < 4; i++)
      {
         const char *one[] = {cases[i]};
         bedrock_target_t t = {.type = BEDROCK_TARGET_CROSS_REGION_INFERENCE_PROFILE,
                               .partition = "aws",
                               .invoke_region = "us-east-1",
                               .region_set = regions1,
                               .n_regions = 1,
                               .account = "123456789012",
                               .id = "p",
                               .underlying_fm_arns = one,
                               .n_underlying = 1};
         assert(bedrock_session_policy(&t, 0, out, sizeof(out)) == -1);
         assert(out[0] == '\0');
      }
   }
   {
      /* Authoritative destination regions and underlying ARN regions must agree. */
      const char *fms[] = {"arn:aws:bedrock:us-west-2::foundation-model/anthropic.claude"};
      bedrock_target_t t = {.type = BEDROCK_TARGET_CROSS_REGION_INFERENCE_PROFILE,
                            .partition = "aws",
                            .invoke_region = "us-east-1",
                            .region_set = regions1,
                            .n_regions = 1,
                            .account = "123456789012",
                            .id = "p",
                            .underlying_fm_arns = fms,
                            .n_underlying = 1};
      assert(bedrock_session_policy(&t, 0, out, sizeof(out)) == -1);
      assert(out[0] == '\0');
   }
   {
      /* The documented maximum is profile ARN + 64 underlying resources. */
      char arns[64][128];
      const char *arnp[64];
      char large[20000];
      for (size_t i = 0; i < 64; i++)
      {
         snprintf(arns[i], sizeof(arns[i]),
                  "arn:aws:bedrock:us-east-1::foundation-model/test.model-%zu", i);
         arnp[i] = arns[i];
      }
      bedrock_target_t t = {.type = BEDROCK_TARGET_APP_INFERENCE_PROFILE,
                            .partition = "aws",
                            .invoke_region = "us-east-1",
                            .region_set = regions1,
                            .n_regions = 1,
                            .account = "123456789012",
                            .id = "p",
                            .underlying_fm_arns = arnp,
                            .n_underlying = 64};
      assert(bedrock_session_policy(&t, 0, large, sizeof(large)) == 0);
      assert(count_text(large, "application-inference-profile/p") == 1);
      assert(count_text(large, "::foundation-model/test.model-") == 64);
   }
   printf("  bedrock policy: 5 types exact + fail-closed (no wildcard, no Converse, "
          "bad-underlying-ARN rejected)\n");
}

/* ===================== (f) STS cache isolation matrix ====================== */

static aws_sts_credentials_t make_creds(const char *ak)
{
   aws_sts_credentials_t c;
   memset(&c, 0, sizeof(c));
   snprintf(c.access_key_id, sizeof(c.access_key_id), "%s", ak);
   return c;
}

/* A fully-populated mode-a (web-identity) baseline key. */
static sts_cache_key_t base_key(void)
{
   sts_cache_key_t k;
   sts_cache_key_init(&k);
   k.federation_mode = STS_FED_WEB_IDENTITY;
   snprintf(k.org_team, sizeof(k.org_team), "%s", "org1/teamA");
   snprintf(k.key_slot, sizeof(k.key_slot), "%s", "slot0");
   k.credential_generation = 7;
   snprintf(k.role_arn, sizeof(k.role_arn), "%s", "arn:aws:iam::1:role/r");
   snprintf(k.role_session_name, sizeof(k.role_session_name), "%s", "sess");
   snprintf(k.issuer, sizeof(k.issuer), "%s", "https://idp");
   snprintf(k.audience, sizeof(k.audience), "%s", "sts.amazonaws.com");
   snprintf(k.subject, sizeof(k.subject), "%s", "repo:acme/app");
   snprintf(k.partition, sizeof(k.partition), "%s", "aws");
   const char *regions[] = {"us-west-2", "us-east-1"};
   sts_cache_region_set(&k, regions, 2);
   snprintf(k.target_id, sizeof(k.target_id), "%s", "anthropic.claude");
   sts_cache_set_policy_hash(&k, "{\"Version\":\"2012-10-17\",\"policy\":\"A\"}");
   return k;
}

static void test_cache_isolation(void)
{
   const long now = 1000;
   const long exp = now + 900;
   const long gen = 7;

   /* region-set is sorted, so order-of-input does not matter (a HIT). */
   {
      static sts_cache_t c;
      sts_cache_init(&c);
      sts_cache_key_t k = base_key();
      aws_sts_credentials_t creds = make_creds("HIT");
      assert(sts_cache_put(&c, &k, &creds, now, exp, gen) == 0);
      sts_cache_key_t k2 = base_key();
      const char *r2[] = {"us-east-1", "us-west-2"}; /* reversed input order */
      sts_cache_region_set(&k2, r2, 2);
      const aws_sts_credentials_t *hit = sts_cache_get(&c, &k2, now, gen);
      assert(hit && strcmp(hit->access_key_id, "HIT") == 0);
   }

   /* Negative matrix: perturbing ANY single field -> miss. */
#define PERTURB(mut)                                                                               \
   do                                                                                              \
   {                                                                                               \
      static sts_cache_t c;                                                                        \
      sts_cache_init(&c);                                                                          \
      sts_cache_key_t k = base_key();                                                              \
      aws_sts_credentials_t creds = make_creds("X");                                               \
      assert(sts_cache_put(&c, &k, &creds, now, exp, gen) == 0);                                   \
      sts_cache_key_t q = base_key();                                                              \
      mut;                                                                                         \
      assert(sts_cache_get(&c, &q, now, gen) == NULL);                                             \
   } while (0)

   PERTURB(q.federation_mode = STS_FED_ASSUME_ROLE);
   PERTURB(snprintf(q.org_team, sizeof(q.org_team), "%s", "org1/teamB"));
   PERTURB(snprintf(q.key_slot, sizeof(q.key_slot), "%s", "slot1"));
   PERTURB(snprintf(q.role_arn, sizeof(q.role_arn), "%s", "arn:aws:iam::1:role/other"));
   PERTURB(snprintf(q.role_session_name, sizeof(q.role_session_name), "%s", "sess2"));
   PERTURB(snprintf(q.issuer, sizeof(q.issuer), "%s", "https://other-idp"));
   PERTURB(snprintf(q.audience, sizeof(q.audience), "%s", "other-aud"));
   PERTURB(snprintf(q.subject, sizeof(q.subject), "%s", "repo:acme/other"));
   PERTURB(snprintf(q.partition, sizeof(q.partition), "%s", "aws-us-gov"));
   PERTURB(snprintf(q.target_id, sizeof(q.target_id), "%s", "meta.llama"));
   {
      const char *r[] = {"us-east-1", "eu-west-1"};
      PERTURB(sts_cache_region_set(&q, r, 2));
   }
   PERTURB(sts_cache_set_policy_hash(&q, "{\"Version\":\"2012-10-17\",\"policy\":\"B\"}"));
#undef PERTURB

   /* credential-generation bump -> miss (rotation / entitlement revocation). */
   {
      static sts_cache_t c;
      sts_cache_init(&c);
      sts_cache_key_t k = base_key();
      aws_sts_credentials_t creds = make_creds("X");
      assert(sts_cache_put(&c, &k, &creds, now, exp, gen) == 0);
      assert(sts_cache_get(&c, &k, now, gen) != NULL);     /* current gen hits */
      assert(sts_cache_get(&c, &k, now, gen + 1) == NULL); /* bumped gen misses */
   }

   /* TTL expiry -> miss. */
   {
      static sts_cache_t c;
      sts_cache_init(&c);
      sts_cache_key_t k = base_key();
      aws_sts_credentials_t creds = make_creds("X");
      assert(sts_cache_put(&c, &k, &creds, now, now + 100, gen) == 0);
      assert(sts_cache_get(&c, &k, now + 50, gen) != NULL);  /* still live */
      assert(sts_cache_get(&c, &k, now + 100, gen) == NULL); /* now >= expiration */
      assert(sts_cache_get(&c, &k, now + 200, gen) == NULL);
   }

   /* TTL ceiling: an over-long expiry is clamped to now + 900. */
   {
      static sts_cache_t c;
      sts_cache_init(&c);
      sts_cache_key_t k = base_key();
      aws_sts_credentials_t creds = make_creds("X");
      assert(sts_cache_put(&c, &k, &creds, now, now + 100000, gen) == 0);
      assert(sts_cache_get(&c, &k, now + STS_CACHE_TTL_MAX, gen) == NULL); /* clamped */
   }
   printf("  sts cache: exact hit; every-field isolation; gen-bump + TTL invalidation\n");
}

int main(void)
{
   printf("test_aws_auth:\n");
   test_sigv4_vectors();
   test_sigv4_payload_modes();
   test_sigv4_security_token();
   test_web_identity();
   test_sts_bodies();
   test_sts_parse();
   test_bedrock_policy();
   test_cache_isolation();
   printf("test_aws_auth: all passed\n");
   return 0;
}
