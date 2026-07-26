/* test_kb_oidc_login.c — the OIDC relying-party login core (src/kb/kb_oidc_login.c).
 *
 * The nonce path is exercised against REAL RS256 id_tokens signed by a real
 * keypair, because the whole point of the nonce check is what it does to a token
 * that verified: a mock string would prove nothing about the claim actually
 * being read out of a signed payload.
 *
 * What is pinned here:
 *   - configuration is refused before anyone can log in (non-https authorize
 *     endpoint, control bytes, missing fields), with loopback http allowed for
 *     the redirect only
 *   - state, verifier and nonce are three independent 43-char secrets, and a
 *     CSPRNG failure yields no pending login at all
 *   - the authorization URL carries response_type=code, S256, state, nonce, and
 *     defaults the scope to openid (without which no id_token comes back)
 *   - a wrong, short, long, empty or absent state is a mismatch, never a pass
 *   - a token whose nonce is absent, empty, or another login's is a mismatch
 *   - the identity key is issuer-scoped from the CONFIGURED issuer, so a token
 *     cannot nominate its own namespace
 */
#include "kb_oidc_login.h"

#include "kb_auth_oidc.h"
#include "oauth_pkce.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int random_failure;

/* Overrides the platform CSPRNG so a failure path is reachable. The sequence is
 * deliberately varying so the three draws differ. */
int platform_random_bytes(void *buf, size_t len)
{
   if (random_failure)
      return -1;
   static unsigned char sequence = 7;
   unsigned char *out = buf;
   for (size_t i = 0; i < len; ++i)
      out[i] = (unsigned char)(sequence++ * 31u + 5u);
   return 0;
}

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

static char *make_jwt(EVP_PKEY *pkey, const char *payload_json)
{
   char *h64 = b64url_str("{\"alg\":\"RS256\",\"kid\":\"test-key\"}");
   char *p64 = b64url_str(payload_json);
   char *input = malloc(strlen(h64) + strlen(p64) + 2);
   assert(input);
   sprintf(input, "%s.%s", h64, p64);
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   assert(md && EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, pkey) == 1);
   size_t siglen = 0;
   assert(EVP_DigestSign(md, NULL, &siglen, (unsigned char *)input, strlen(input)) == 1);
   unsigned char *sig = malloc(siglen);
   assert(sig && EVP_DigestSign(md, sig, &siglen, (unsigned char *)input, strlen(input)) == 1);
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

static kb_oidc_login_config_t good_config(void)
{
   kb_oidc_login_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.issuer, sizeof(cfg.issuer), "%s", "https://idp.example");
   snprintf(cfg.client_id, sizeof(cfg.client_id), "%s", "aimee-kb");
   snprintf(cfg.authorize_url, sizeof(cfg.authorize_url), "%s", "https://idp.example/authorize");
   snprintf(cfg.token_url, sizeof(cfg.token_url), "%s", "https://idp.example/token");
   snprintf(cfg.redirect_uri, sizeof(cfg.redirect_uri), "%s", "https://kb.example/oidc/callback");
   return cfg;
}

static void test_config_validation(void)
{
   kb_oidc_login_config_t cfg = good_config();
   assert(kb_oidc_login_config_valid(&cfg));
   assert(!kb_oidc_login_config_valid(NULL));

   /* A cleartext IdP hop is refused: a profile must not be able to quietly
    * downgrade the authorization request to http. */
   kb_oidc_login_config_t bad = cfg;
   snprintf(bad.authorize_url, sizeof(bad.authorize_url), "%s", "http://idp.example/authorize");
   assert(!kb_oidc_login_config_valid(&bad));
   /* Not even loopback for the authorize endpoint — that exception is for the
    * browser's own redirect, not for the hop to the IdP. */
   bad = cfg;
   snprintf(bad.authorize_url, sizeof(bad.authorize_url), "%s", "http://127.0.0.1/authorize");
   assert(!kb_oidc_login_config_valid(&bad));

   /* Loopback http IS allowed for the redirect: that is how a browser on the
    * operator's own machine completes the flow. */
   bad = cfg;
   snprintf(bad.redirect_uri, sizeof(bad.redirect_uri), "%s", "http://127.0.0.1:8765/cb");
   assert(kb_oidc_login_config_valid(&bad));
   snprintf(bad.redirect_uri, sizeof(bad.redirect_uri), "%s", "http://localhost:8765/cb");
   assert(kb_oidc_login_config_valid(&bad));
   /* But a non-loopback http redirect is not. */
   snprintf(bad.redirect_uri, sizeof(bad.redirect_uri), "%s", "http://kb.example/cb");
   assert(!kb_oidc_login_config_valid(&bad));

   /* "https://" with nothing after it is not a URL. */
   bad = cfg;
   snprintf(bad.authorize_url, sizeof(bad.authorize_url), "%s", "https://");
   assert(!kb_oidc_login_config_valid(&bad));

   /* Every required field is required. */
   bad = cfg;
   bad.issuer[0] = '\0';
   assert(!kb_oidc_login_config_valid(&bad));
   bad = cfg;
   bad.client_id[0] = '\0';
   assert(!kb_oidc_login_config_valid(&bad));
   bad = cfg;
   bad.redirect_uri[0] = '\0';
   assert(!kb_oidc_login_config_valid(&bad));

   /* A newline in any field would be a log/URL injection primitive. */
   bad = cfg;
   snprintf(bad.client_id, sizeof(bad.client_id), "%s", "aimee\nkb");
   assert(!kb_oidc_login_config_valid(&bad));
   bad = cfg;
   snprintf(bad.scope, sizeof(bad.scope), "%s", "openid\nemail");
   assert(!kb_oidc_login_config_valid(&bad));

   /* An absent scope is fine — it defaults to openid at start time. */
   bad = cfg;
   bad.scope[0] = '\0';
   assert(kb_oidc_login_config_valid(&bad));

   /* A scope IS a space-delimited list, so interior spaces are meaningful and
    * must be accepted — but nothing that would change the list the IdP sees. */
   snprintf(bad.scope, sizeof(bad.scope), "%s", "openid email profile");
   assert(kb_oidc_login_config_valid(&bad));
   snprintf(bad.scope, sizeof(bad.scope), "%s", " openid");
   assert(!kb_oidc_login_config_valid(&bad));
   snprintf(bad.scope, sizeof(bad.scope), "%s", "openid ");
   assert(!kb_oidc_login_config_valid(&bad));
   snprintf(bad.scope, sizeof(bad.scope), "%s", "openid  email");
   assert(!kb_oidc_login_config_valid(&bad));
   snprintf(bad.scope, sizeof(bad.scope), "%s", "openid\temail");
   assert(!kb_oidc_login_config_valid(&bad));
}

static void test_start(void)
{
   kb_oidc_login_config_t cfg = good_config();
   kb_oidc_login_pending_t pending;
   char url[KB_OIDC_LOGIN_URL_MAX];

   random_failure = 0;
   assert(kb_oidc_login_start(&cfg, &pending, url, sizeof(url)) == KB_OIDC_LOGIN_OK);

   /* Three independent secrets, each at full length. */
   assert(strlen(pending.state) == KB_OIDC_LOGIN_SECRET_LEN);
   assert(strlen(pending.code_verifier) == KB_OIDC_LOGIN_SECRET_LEN);
   assert(strlen(pending.nonce) == KB_OIDC_LOGIN_SECRET_LEN);
   assert(strcmp(pending.state, pending.code_verifier));
   assert(strcmp(pending.state, pending.nonce));
   assert(strcmp(pending.code_verifier, pending.nonce));
   /* The redirect_uri is retained, because the token exchange must present the
    * identical value. */
   assert(!strcmp(pending.redirect_uri, cfg.redirect_uri));

   /* The URL is a PKCE OIDC authorization request. */
   assert(!strncmp(url, "https://idp.example/authorize?", 29));
   assert(strstr(url, "response_type=code"));
   assert(strstr(url, "client_id=aimee-kb"));
   assert(strstr(url, "code_challenge_method=S256"));
   assert(strstr(url, "&scope=openid")); /* defaulted, not omitted */
   assert(strstr(url, "&state="));
   assert(strstr(url, "&nonce="));
   /* The code_verifier itself must NEVER appear in the URL — only its S256
    * challenge does. That is the entire point of PKCE. */
   assert(!strstr(url, pending.code_verifier));
   char challenge[OAUTH_PKCE_CHALLENGE_LEN + 1];
   assert(oauth_pkce_s256_challenge(pending.code_verifier, challenge, sizeof(challenge)) == 0);
   assert(strstr(url, challenge));
   /* State and nonce do appear (they are meant to round-trip) and are distinct. */
   assert(strstr(url, pending.state) && strstr(url, pending.nonce));

   /* An explicit scope is honoured verbatim. */
   snprintf(cfg.scope, sizeof(cfg.scope), "%s", "openid email");
   assert(kb_oidc_login_start(&cfg, &pending, url, sizeof(url)) == KB_OIDC_LOGIN_OK);
   assert(strstr(url, "&scope=openid%20email"));

   /* Bad arguments and a bad profile produce no pending login. */
   cfg = good_config();
   assert(kb_oidc_login_start(NULL, &pending, url, sizeof(url)) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_login_start(&cfg, NULL, url, sizeof(url)) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_login_start(&cfg, &pending, NULL, sizeof(url)) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_login_start(&cfg, &pending, url, 0) == KB_OIDC_LOGIN_INVALID);

   kb_oidc_login_config_t bad = cfg;
   snprintf(bad.authorize_url, sizeof(bad.authorize_url), "%s", "http://idp/authorize");
   memset(&pending, 0xaa, sizeof(pending));
   assert(kb_oidc_login_start(&bad, &pending, url, sizeof(url)) == KB_OIDC_LOGIN_INVALID);
   kb_oidc_login_pending_t zero;
   memset(&zero, 0, sizeof(zero));
   assert(!memcmp(&pending, &zero, sizeof(pending)));
   assert(url[0] == '\0');

   /* A buffer too small for the URL is UNAVAILABLE, and again leaves nothing
    * pending — a half-built request must not be startable. */
   char tiny[32];
   memset(&pending, 0xaa, sizeof(pending));
   assert(kb_oidc_login_start(&cfg, &pending, tiny, sizeof(tiny)) == KB_OIDC_LOGIN_UNAVAILABLE);
   assert(!memcmp(&pending, &zero, sizeof(pending)));
   assert(tiny[0] == '\0');

   /* A CSPRNG failure must never yield a weak login. */
   random_failure = 1;
   memset(&pending, 0xaa, sizeof(pending));
   assert(kb_oidc_login_start(&cfg, &pending, url, sizeof(url)) == KB_OIDC_LOGIN_UNAVAILABLE);
   assert(!memcmp(&pending, &zero, sizeof(pending)));
   random_failure = 0;
}

static void test_check_state(void)
{
   kb_oidc_login_config_t cfg = good_config();
   kb_oidc_login_pending_t pending;
   char url[KB_OIDC_LOGIN_URL_MAX];
   assert(kb_oidc_login_start(&cfg, &pending, url, sizeof(url)) == KB_OIDC_LOGIN_OK);

   assert(kb_oidc_login_check_state(&pending, pending.state) == KB_OIDC_LOGIN_OK);

   assert(kb_oidc_login_check_state(NULL, pending.state) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_login_check_state(&pending, NULL) == KB_OIDC_LOGIN_INVALID);
   /* An empty callback state is a mismatch, not an accepted "no state". */
   assert(kb_oidc_login_check_state(&pending, "") == KB_OIDC_LOGIN_STATE_MISMATCH);

   /* One flipped character. */
   char wrong[KB_OIDC_LOGIN_SECRET_LEN + 1];
   memcpy(wrong, pending.state, sizeof(wrong));
   wrong[0] = (char)(wrong[0] == 'A' ? 'B' : 'A');
   assert(kb_oidc_login_check_state(&pending, wrong) == KB_OIDC_LOGIN_STATE_MISMATCH);

   /* A prefix must not pass, and neither must an extension. */
   char shorter[KB_OIDC_LOGIN_SECRET_LEN];
   memcpy(shorter, pending.state, sizeof(shorter) - 1);
   shorter[sizeof(shorter) - 1] = '\0';
   assert(kb_oidc_login_check_state(&pending, shorter) == KB_OIDC_LOGIN_STATE_MISMATCH);
   char longer[KB_OIDC_LOGIN_SECRET_LEN + 3];
   snprintf(longer, sizeof(longer), "%sxx", pending.state);
   assert(kb_oidc_login_check_state(&pending, longer) == KB_OIDC_LOGIN_STATE_MISMATCH);

   /* A pending login that was never started cannot match anything, including
    * the empty string. */
   kb_oidc_login_pending_t empty;
   memset(&empty, 0, sizeof(empty));
   assert(kb_oidc_login_check_state(&empty, "") == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_login_check_state(&empty, pending.state) == KB_OIDC_LOGIN_INVALID);

   /* Another login's state does not satisfy this one. */
   kb_oidc_login_pending_t other;
   assert(kb_oidc_login_start(&cfg, &other, url, sizeof(url)) == KB_OIDC_LOGIN_OK);
   assert(kb_oidc_login_check_state(&pending, other.state) == KB_OIDC_LOGIN_STATE_MISMATCH);
}

static void test_check_nonce(EVP_PKEY *key, const char *jwks)
{
   kb_oidc_login_config_t cfg = good_config();
   kb_oidc_login_pending_t pending, other;
   char url[KB_OIDC_LOGIN_URL_MAX];
   assert(kb_oidc_login_start(&cfg, &pending, url, sizeof(url)) == KB_OIDC_LOGIN_OK);
   assert(kb_oidc_login_start(&cfg, &other, url, sizeof(url)) == KB_OIDC_LOGIN_OK);

   kb_oidc_config_t vcfg;
   memset(&vcfg, 0, sizeof(vcfg));
   snprintf(vcfg.issuer, sizeof(vcfg.issuer), "%s", "https://idp.example");
   snprintf(vcfg.audience, sizeof(vcfg.audience), "%s", "aimee-kb");
   snprintf(vcfg.jwks_json, sizeof(vcfg.jwks_json), "%s", jwks);

   char payload[1024];
   /* The happy path: a token that really verifies, carrying this login's nonce. */
   snprintf(payload, sizeof(payload),
            "{\"iss\":\"https://idp.example\",\"aud\":\"aimee-kb\",\"sub\":\"alice\","
            "\"iat\":%ld,\"exp\":%ld,\"nonce\":\"%s\"}",
            NOW, NOW + 300, pending.nonce);
   char *jwt = make_jwt(key, payload);
   kb_verify_result_t verified;
   assert(kb_oidc_verify_jwt(jwt, &vcfg, NOW, &verified) == 1);
   assert(kb_oidc_login_check_nonce(&pending, jwt) == KB_OIDC_LOGIN_OK);
   /* THE case this check exists for: the very same valid, signed token replayed
    * into a different login must be refused. */
   assert(kb_oidc_login_check_nonce(&other, jwt) == KB_OIDC_LOGIN_NONCE_MISMATCH);
   free(jwt);

   /* No nonce claim at all — a refusal, never a pass. */
   snprintf(payload, sizeof(payload),
            "{\"iss\":\"https://idp.example\",\"aud\":\"aimee-kb\",\"sub\":\"alice\","
            "\"iat\":%ld,\"exp\":%ld}",
            NOW, NOW + 300);
   jwt = make_jwt(key, payload);
   assert(kb_oidc_verify_jwt(jwt, &vcfg, NOW, &verified) == 1); /* still a valid token */
   assert(kb_oidc_login_check_nonce(&pending, jwt) == KB_OIDC_LOGIN_NONCE_MISMATCH);
   free(jwt);

   /* An empty nonce claim, and a non-string one. */
   snprintf(payload, sizeof(payload),
            "{\"iss\":\"https://idp.example\",\"aud\":\"aimee-kb\",\"sub\":\"alice\","
            "\"iat\":%ld,\"exp\":%ld,\"nonce\":\"\"}",
            NOW, NOW + 300);
   jwt = make_jwt(key, payload);
   assert(kb_oidc_login_check_nonce(&pending, jwt) == KB_OIDC_LOGIN_NONCE_MISMATCH);
   free(jwt);
   snprintf(payload, sizeof(payload),
            "{\"iss\":\"https://idp.example\",\"aud\":\"aimee-kb\",\"sub\":\"alice\","
            "\"iat\":%ld,\"exp\":%ld,\"nonce\":12345}",
            NOW, NOW + 300);
   jwt = make_jwt(key, payload);
   assert(kb_oidc_login_check_nonce(&pending, jwt) == KB_OIDC_LOGIN_NONCE_MISMATCH);
   free(jwt);

   /* A nonce that is a prefix of the real one. */
   char truncated[KB_OIDC_LOGIN_SECRET_LEN];
   memcpy(truncated, pending.nonce, sizeof(truncated) - 1);
   truncated[sizeof(truncated) - 1] = '\0';
   snprintf(payload, sizeof(payload),
            "{\"iss\":\"https://idp.example\",\"aud\":\"aimee-kb\",\"sub\":\"alice\","
            "\"iat\":%ld,\"exp\":%ld,\"nonce\":\"%s\"}",
            NOW, NOW + 300, truncated);
   jwt = make_jwt(key, payload);
   assert(kb_oidc_login_check_nonce(&pending, jwt) == KB_OIDC_LOGIN_NONCE_MISMATCH);
   free(jwt);

   /* Malformed input and an unstarted login. */
   assert(kb_oidc_login_check_nonce(&pending, "not-a-jwt") == KB_OIDC_LOGIN_NONCE_MISMATCH);
   assert(kb_oidc_login_check_nonce(&pending, NULL) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_login_check_nonce(NULL, "x.y.z") == KB_OIDC_LOGIN_INVALID);
   kb_oidc_login_pending_t empty;
   memset(&empty, 0, sizeof(empty));
   assert(kb_oidc_login_check_nonce(&empty, "x.y.z") == KB_OIDC_LOGIN_INVALID);
}

static void test_principal(void)
{
   kb_oidc_login_config_t cfg = good_config();
   kb_verify_result_t verified;
   memset(&verified, 0, sizeof(verified));
   snprintf(verified.subject, sizeof(verified.subject), "%s", "alice");

   kb_principal_t p;
   assert(kb_oidc_login_principal(&cfg, &verified, &p) == KB_OIDC_LOGIN_OK);
   assert(p.authenticated == 1 && p.kind == KB_PRIN_OIDC);
   assert(!strcmp(p.subject, "alice"));
   /* The issuer is the configured one, so the identity key is namespaced by the
    * IdP kb actually trusts rather than by anything the token said. */
   assert(!strcmp(p.issuer, "https://idp.example"));
   char key[700];
   assert(kb_identity_key(&p, key, sizeof(key)) == 0);
   assert(!strncmp(key, "oidc:", 5));
   assert(strstr(key, "alice"));

   /* A verified result with no subject cannot become a principal. */
   kb_verify_result_t empty;
   memset(&empty, 0, sizeof(empty));
   assert(kb_oidc_login_principal(&cfg, &empty, &p) == KB_OIDC_LOGIN_INVALID);
   assert(p.authenticated == 0);

   /* A bad profile cannot produce a principal either — the issuer would be
    * unusable as a namespace. */
   kb_oidc_login_config_t bad = cfg;
   bad.issuer[0] = '\0';
   assert(kb_oidc_login_principal(&bad, &verified, &p) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_login_principal(&cfg, &verified, NULL) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_login_principal(NULL, &verified, &p) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_login_principal(&cfg, NULL, &p) == KB_OIDC_LOGIN_INVALID);
}

static void test_pending_clear(void)
{
   kb_oidc_login_config_t cfg = good_config();
   kb_oidc_login_pending_t pending;
   char url[KB_OIDC_LOGIN_URL_MAX];
   assert(kb_oidc_login_start(&cfg, &pending, url, sizeof(url)) == KB_OIDC_LOGIN_OK);
   kb_oidc_login_pending_clear(&pending);
   kb_oidc_login_pending_t zero;
   memset(&zero, 0, sizeof(zero));
   assert(!memcmp(&pending, &zero, sizeof(pending)));
   kb_oidc_login_pending_clear(NULL); /* must not crash */
}

static void test_config_from_env(void)
{
   /* Unset everything first: these tests run in whatever environment CI hands
    * them, and a leaked AIMEE_KB_OIDC_* from another suite would make this
    * assert something other than what it reads. */
   unsetenv("AIMEE_KB_OIDC_LOGIN_CLIENT_ID");
   unsetenv("AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL");
   unsetenv("AIMEE_KB_OIDC_LOGIN_TOKEN_URL");
   unsetenv("AIMEE_KB_OIDC_LOGIN_REDIRECT_URI");
   unsetenv("AIMEE_KB_OIDC_LOGIN_SCOPE");
   unsetenv("AIMEE_KB_OIDC_ISSUER");

   kb_oidc_login_config_t cfg;
   /* No client id -> the login front end is OFF. A deliberate state, not an
    * error: a kb may verify bearers without offering a login, and PAM mode
    * leaves all of this unset. */
   assert(kb_oidc_login_config_from_env(&cfg) == KB_OIDC_LOGIN_DISABLED);
   assert(kb_oidc_login_config_from_env(NULL) == KB_OIDC_LOGIN_INVALID);

   setenv("AIMEE_KB_OIDC_LOGIN_CLIENT_ID", "aimee-kb", 1);
   /* Set-but-incomplete must be LOUD. An operator who configured a login and
    * left out the endpoint has to find out, not silently get no logins. */
   assert(kb_oidc_login_config_from_env(&cfg) == KB_OIDC_LOGIN_INVALID);
   kb_oidc_login_config_t zero;
   memset(&zero, 0, sizeof(zero));
   assert(!memcmp(&cfg, &zero, sizeof(cfg)));

   setenv("AIMEE_KB_OIDC_ISSUER", "https://idp.example", 1);
   setenv("AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL", "https://idp.example/authorize", 1);
   assert(kb_oidc_login_config_from_env(&cfg) == KB_OIDC_LOGIN_INVALID); /* no token url */
   setenv("AIMEE_KB_OIDC_LOGIN_TOKEN_URL", "https://idp.example/token", 1);
   assert(kb_oidc_login_config_from_env(&cfg) == KB_OIDC_LOGIN_INVALID); /* no redirect */
   setenv("AIMEE_KB_OIDC_LOGIN_REDIRECT_URI", "https://kb.example/oidc/callback", 1);
   assert(kb_oidc_login_config_from_env(&cfg) == KB_OIDC_LOGIN_OK);
   assert(!strcmp(cfg.token_url, "https://idp.example/token"));
   assert(!strcmp(cfg.client_id, "aimee-kb"));
   assert(!strcmp(cfg.authorize_url, "https://idp.example/authorize"));
   assert(!strcmp(cfg.redirect_uri, "https://kb.example/oidc/callback"));
   /* The issuer is the VERIFIER's AIMEE_KB_OIDC_ISSUER, deliberately shared: two
    * separate knobs could drift, and then the issuer a login trusts would not be
    * the issuer a bearer is verified against. */
   assert(!strcmp(cfg.issuer, "https://idp.example"));
   assert(cfg.scope[0] == '\0'); /* defaulted to openid at start time, not here */

   setenv("AIMEE_KB_OIDC_LOGIN_SCOPE", "openid email", 1);
   assert(kb_oidc_login_config_from_env(&cfg) == KB_OIDC_LOGIN_OK);
   assert(!strcmp(cfg.scope, "openid email"));

   /* A cleartext endpoint from the environment is refused exactly as one from a
    * struct is — env is not a trusted bypass of the profile rules. */
   setenv("AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL", "http://idp.example/authorize", 1);
   assert(kb_oidc_login_config_from_env(&cfg) == KB_OIDC_LOGIN_INVALID);
   setenv("AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL", "https://idp.example/authorize", 1);

   /* An oversize value is refused rather than truncated: a silently shortened
    * redirect_uri would fail at the IdP for no visible reason. */
   char huge[900];
   memset(huge, 'a', sizeof(huge) - 1);
   huge[sizeof(huge) - 1] = '\0';
   char url[1024];
   snprintf(url, sizeof(url), "https://kb.example/%s", huge);
   setenv("AIMEE_KB_OIDC_LOGIN_REDIRECT_URI", url, 1);
   assert(kb_oidc_login_config_from_env(&cfg) == KB_OIDC_LOGIN_INVALID);

   unsetenv("AIMEE_KB_OIDC_LOGIN_CLIENT_ID");
   unsetenv("AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL");
   unsetenv("AIMEE_KB_OIDC_LOGIN_TOKEN_URL");
   unsetenv("AIMEE_KB_OIDC_LOGIN_REDIRECT_URI");
   unsetenv("AIMEE_KB_OIDC_LOGIN_SCOPE");
   unsetenv("AIMEE_KB_OIDC_ISSUER");
}

static void test_token_url_split(void)
{
   char host[256], path[512];

   assert(kb_oidc_token_url_split("https://idp.example/oauth/token", host, sizeof(host), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_OK);
   assert(!strcmp(host, "idp.example"));
   assert(!strcmp(path, "/oauth/token"));

   /* No path means the root. */
   assert(kb_oidc_token_url_split("https://idp.example", host, sizeof(host), path, sizeof(path)) ==
          KB_OIDC_LOGIN_OK);
   assert(!strcmp(host, "idp.example") && !strcmp(path, "/"));
   assert(kb_oidc_token_url_split("https://idp.example/", host, sizeof(host), path, sizeof(path)) ==
          KB_OIDC_LOGIN_OK);
   assert(!strcmp(path, "/"));

   /* Validate-only mode, used by the profile check. */
   assert(kb_oidc_token_url_split("https://idp.example/token", NULL, 0, NULL, 0) ==
          KB_OIDC_LOGIN_OK);

   /* Cleartext, and no scheme at all. */
   assert(kb_oidc_token_url_split("http://idp.example/token", host, sizeof(host), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_token_url_split("idp.example/token", host, sizeof(host), path, sizeof(path)) ==
          KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_token_url_split(NULL, host, sizeof(host), path, sizeof(path)) ==
          KB_OIDC_LOGIN_INVALID);
   assert(host[0] == '\0' && path[0] == '\0');

   /* THE case worth refusing rather than coercing: an explicit port. The egress
    * client dials 443 regardless, so accepting ":8443" would silently send the
    * client secret to a different service than the operator named. Even ":443"
    * is refused, because accepting it would mean the parser has to be trusted to
    * tell the two apart. */
   assert(kb_oidc_token_url_split("https://idp.example:8443/token", host, sizeof(host), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_token_url_split("https://idp.example:443/token", host, sizeof(host), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_INVALID);

   /* Userinfo: how a URL is made to look like one host while resolving to
    * another. */
   assert(kb_oidc_token_url_split("https://idp.example@evil.test/token", host, sizeof(host), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_INVALID);

   /* An origin-form target carries no query or fragment. */
   assert(kb_oidc_token_url_split("https://idp.example/token?x=1", host, sizeof(host), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_token_url_split("https://idp.example/token#f", host, sizeof(host), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_INVALID);

   /* "//" would be read as an authority, not a path. */
   assert(kb_oidc_token_url_split("https://idp.example//token", host, sizeof(host), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_INVALID);

   /* Empty, malformed and non-ASCII hosts. */
   assert(kb_oidc_token_url_split("https:///token", host, sizeof(host), path, sizeof(path)) ==
          KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_token_url_split("https://.idp.example/t", host, sizeof(host), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_token_url_split("https://idp.example./t", host, sizeof(host), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_token_url_split("https://[::1]/t", host, sizeof(host), path, sizeof(path)) ==
          KB_OIDC_LOGIN_INVALID);
   assert(kb_oidc_token_url_split("https://idp_example/t", host, sizeof(host), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_INVALID);
   /* A space in the path would split the request line. */
   assert(kb_oidc_token_url_split("https://idp.example/to ken", host, sizeof(host), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_INVALID);

   /* Outputs too small refuse, and leave nothing partially written. */
   char tiny[4];
   assert(kb_oidc_token_url_split("https://idp.example/token", tiny, sizeof(tiny), path,
                                  sizeof(path)) == KB_OIDC_LOGIN_INVALID);
   assert(tiny[0] == '\0');
   assert(kb_oidc_token_url_split("https://idp.example/token", host, sizeof(host), tiny,
                                  sizeof(tiny)) == KB_OIDC_LOGIN_INVALID);
   assert(host[0] == '\0' && tiny[0] == '\0');

   /* And a profile naming an unusable token endpoint is refused as a whole, so
    * the failure lands at configuration time rather than at somebody's login. */
   kb_oidc_login_config_t cfg = good_config();
   assert(kb_oidc_login_config_valid(&cfg));
   snprintf(cfg.token_url, sizeof(cfg.token_url), "%s", "https://idp.example:8443/token");
   assert(!kb_oidc_login_config_valid(&cfg));
   cfg = good_config();
   cfg.token_url[0] = '\0';
   assert(!kb_oidc_login_config_valid(&cfg));
}

int main(void)
{
   EVP_PKEY *key = NULL;
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   assert(ctx && EVP_PKEY_keygen_init(ctx) == 1);
   assert(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1);
   assert(EVP_PKEY_keygen(ctx, &key) == 1);
   EVP_PKEY_CTX_free(ctx);
   char *jwks = make_jwks(key, "test-key");

   test_config_validation();
   test_start();
   test_check_state();
   test_check_nonce(key, jwks);
   test_principal();
   test_pending_clear();
   test_config_from_env();
   test_token_url_split();

   free(jwks);
   EVP_PKEY_free(key);
   printf("test_kb_oidc_login: ok\n");
   return 0;
}
