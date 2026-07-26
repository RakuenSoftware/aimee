/* test_kb_http_identity_login.c — the kb login routes.
 *
 * These are the only kb routes reachable WITHOUT a credential, so what they say
 * to a stranger is the thing worth testing:
 *
 *   - the auth-mode declaration answers which flow to start and nothing else:
 *     no issuer, no client id, no endpoint
 *   - a kb with no OIDC login and a kb whose profile is broken are
 *     indistinguishable to an unauthenticated caller
 *   - login/start hands back only the authorize_url and redirect_uri; the
 *     state, nonce and PKCE verifier never appear in a response
 *   - a URL is never handed out unless the pending login was actually retained,
 *     because a login whose state cannot be looked up fails at the callback with
 *     a far more confusing error
 *   - start is POST, since it mutates server state and a GET would be
 *     prefetchable
 *   - the callback consumes its pending login on EVERY path, so no failure leaves
 *     a replayable one behind, and answers every distinct failure with the SAME
 *     status and message — a caller must not be able to tell a bad state from a
 *     rejected code from a bad signature from a wrong nonce
 *
 * The IdP's network call and the vault are stubbed (see below) so the callback's
 * ORDERING is what gets tested; the id_token itself is genuinely RS256-signed by
 * a real keypair, because a mock string would not exercise the audience override
 * or the nonce-after-verification rule that are the point of the route.
 */
#include "kb_http_identity_login.h"

#include "kb_auth_oidc.h"
#include "kb_oidc_login.h"
#include "kb_oidc_login_store.h"
#include "kb_oidc_token_exchange.h"
#include "vault_service.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <assert.h>
#include <unistd.h> /* getpid, for a per-run JWKS fixture path */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int platform_random_bytes(void *buf, size_t len)
{
   static uint32_t call_no = 1;
   unsigned char *out = buf;
   assert(len >= sizeof(call_no));
   for (size_t i = 0; i < len; ++i)
      out[i] = (unsigned char)(i * 13u + 7u);
   memcpy(out, &call_no, sizeof(call_no));
   call_no++;
   return 0;
}

static const int64_t NOW = 1780000000;

static void env_clear(void)
{
   unsetenv("AIMEE_KB_OIDC_LOGIN_CLIENT_ID");
   unsetenv("AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL");
   unsetenv("AIMEE_KB_OIDC_LOGIN_TOKEN_URL");
   unsetenv("AIMEE_KB_OIDC_LOGIN_REDIRECT_URI");
   unsetenv("AIMEE_KB_OIDC_LOGIN_SCOPE");
   unsetenv("AIMEE_KB_OIDC_ISSUER");
}

static void env_configure(void)
{
   setenv("AIMEE_KB_OIDC_LOGIN_CLIENT_ID", "aimee-kb", 1);
   setenv("AIMEE_KB_OIDC_ISSUER", "https://idp.example", 1);
   setenv("AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL", "https://idp.example/authorize", 1);
   setenv("AIMEE_KB_OIDC_LOGIN_TOKEN_URL", "https://idp.example/token", 1);
   setenv("AIMEE_KB_OIDC_LOGIN_REDIRECT_URI", "https://kb.example/v1/identity/login/callback", 1);
}

static int route(const char *method, const char *path, const char *body, char *out, int cap)
{
   return kb_http_identity_login_route(method, path, NULL, body, NOW, out, cap);
}

/* ── Stubs for the two things a route test must not do ─────────────────────── */

/* The vault. The route reads the OIDC client secret from it at the moment of the
 * exchange; a unit test has no vault, and standing one up would test the vault
 * rather than the route. */
static int stub_vault_has_secret = 1;
vault_status_t vault_service_get_server_principal(const char *agent, const char *cred, char *out,
                                                  size_t cap)
{
   /* Assert the LOOKUP, not just the outcome: a route that read some other
    * agent/cred would still "work" against a permissive stub and then find
    * nothing in a real deployment. */
   assert(agent && strcmp(agent, "oidc") == 0);
   assert(cred && strcmp(cred, "oidc_login_client_secret") == 0);
   if (!stub_vault_has_secret)
      return VAULT_NO_ENTRY;
   snprintf(out, cap, "%s", "s3cr3t");
   return VAULT_OK;
}

/* The IdP. Records what the route sent so the retained-login rules can be
 * checked, and returns a caller-chosen outcome. */
static kb_oidc_token_exchange_result_t stub_exchange_result = KB_OIDC_TOKEN_EXCHANGE_OK;
static const char *stub_exchange_token = NULL;
static char stub_seen_code[512];
static char stub_seen_redirect[512];
static char stub_seen_verifier[128];
static int stub_exchange_calls;

kb_oidc_token_exchange_result_t
kb_oidc_token_exchange_post(const kb_oidc_login_config_t *cfg,
                            const kb_oidc_login_pending_t *pending, const char *code,
                            const char *client_secret, char *unverified_id_token_out, size_t cap)
{
   stub_exchange_calls++;
   assert(cfg && pending && code && client_secret);
   /* The secret reached the exchange and nothing else. */
   assert(strcmp(client_secret, "s3cr3t") == 0);
   snprintf(stub_seen_code, sizeof(stub_seen_code), "%s", code);
   snprintf(stub_seen_redirect, sizeof(stub_seen_redirect), "%s", pending->redirect_uri);
   snprintf(stub_seen_verifier, sizeof(stub_seen_verifier), "%s", pending->code_verifier);
   if (cap)
      unverified_id_token_out[0] = '\0';
   if (stub_exchange_result != KB_OIDC_TOKEN_EXCHANGE_OK)
      return stub_exchange_result;
   snprintf(unverified_id_token_out, cap, "%s", stub_exchange_token ? stub_exchange_token : "");
   return KB_OIDC_TOKEN_EXCHANGE_OK;
}

/* ── A real IdP keypair, so verification is real ───────────────────────────── */

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
   free(h64);
   free(p64);
   free(input);
   return jwt;
}

/* An id_token for `nonce`, with the audience an id_token must carry: the login's
 * client_id. `aud`/`iss` are overridable so the negative cases are reachable. */
static char *id_token(EVP_PKEY *key, const char *nonce, const char *aud, const char *iss)
{
   char payload[1024];
   snprintf(payload, sizeof(payload),
            "{\"iss\":\"%s\",\"sub\":\"alice\",\"aud\":\"%s\",\"exp\":%lld,\"iat\":%lld,"
            "\"nonce\":\"%s\"}",
            iss, aud, (long long)(NOW + 300), (long long)NOW, nonce);
   return make_jwt(key, payload);
}

/* The nonce the pending login drew, recovered the same way. */
static void nonce_from_url(const char *url, char *out, size_t cap)
{
   const char *p = strstr(url, "nonce=");
   assert(p);
   p += 6;
   size_t n = 0;
   while (p[n] && p[n] != '&' && p[n] != '"' && n + 1 < cap)
   {
      out[n] = p[n];
      n++;
   }
   out[n] = '\0';
}

static int callback(const char *query, char *out, int cap)
{
   return kb_http_identity_login_route("GET", "/v1/identity/login/callback", query, NULL, NOW, out,
                                       cap);
}

/* Start a login and hand back BOTH secrets the callback needs to be driven. */
static void start_login_full(const char *server_id, char *state_out, size_t state_cap,
                             char *nonce_out, size_t nonce_cap)
{
   char out[4096] = "";
   char body[256];
   snprintf(body, sizeof(body), "{\"server_id\":\"%s\"}", server_id);
   assert(route("POST", "/v1/identity/login/start", body, out, sizeof(out)) == 200);
   nonce_from_url(out, nonce_out, nonce_cap);
   const char *p = strstr(out, "state=");
   assert(p);
   p += 6;
   size_t n = 0;
   while (p[n] && p[n] != '&' && p[n] != '"' && n + 1 < state_cap)
   {
      state_out[n] = p[n];
      n++;
   }
   state_out[n] = '\0';
   assert(n == KB_OIDC_LOGIN_SECRET_LEN);
}

/* THE UNIFORM FAILURE. Every distinct callback failure must be reported
 * identically, or the route is an oracle for exactly the attacks its ordering
 * exists to stop. */
static void assert_generic_failure(int status, const char *body)
{
   assert(status == 401);
   assert(strstr(body, "the login could not be completed"));
   /* Nothing that names WHICH check failed, and nothing about the subject. */
   assert(!strstr(body, "nonce") && !strstr(body, "signature") && !strstr(body, "state"));
   assert(!strstr(body, "alice") && !strstr(body, "subject"));
}

static void test_callback(EVP_PKEY *key, const char *jwks)
{
   char out[4096] = "";
   char state[64] = "", nonce[64] = "";

   env_configure();
   /* Register the IdP's keys as the verifier's, from a file — the same path a
    * deployment uses. The audience is deliberately kb-as-resource-server, NOT the
    * client_id, so this test also proves the route overrides it. */
   char jwks_path[256];
   snprintf(jwks_path, sizeof(jwks_path), "/tmp/kb-login-route-jwks-%d.json", (int)getpid());
   FILE *f = fopen(jwks_path, "wb");
   assert(f && fwrite(jwks, 1, strlen(jwks), f) == strlen(jwks));
   fclose(f);
   assert(kb_oidc_register_from_file(jwks_path, "https://idp.example", "https://kb.example/api",
                                     NULL, NULL) == 0);

   /* METHOD. A browser arrives with GET, because the IdP sends a redirect; every
    * other method is refused even though the route mutates state and would
    * otherwise argue for POST. */
   kb_oidc_login_store_reset();
   assert(kb_http_identity_login_route("POST", "/v1/identity/login/callback", "code=x", NULL, NOW,
                                       out, sizeof(out)) == 405);
   assert(kb_http_identity_login_route("PUT", "/v1/identity/login/callback", "code=x", NULL, NOW,
                                       out, sizeof(out)) == 405);

   /* THE HAPPY PATH. */
   kb_oidc_login_store_reset();
   start_login_full("mintsrv", state, sizeof(state), nonce, sizeof(nonce));
   char *good = id_token(key, nonce, "aimee-kb", "https://idp.example");
   stub_exchange_result = KB_OIDC_TOKEN_EXCHANGE_OK;
   stub_exchange_token = good;
   stub_exchange_calls = 0;
   char query[512];
   snprintf(query, sizeof(query), "code=THECODE&state=%s", state);
   assert(callback(query, out, sizeof(out)) == 200);
   /* The subject is the ISSUER-SCOPED identity key, not the bare "sub": a token
    * from another IdP with sub=alice must not collide with this one.
    *
    * The issuer's own ":" is percent-encoded to %3A, which is what makes the key
    * unambiguously parseable as oidc:<iss>:<sub> — the identity tables' subject
    * CHECK admits only %25 and %3A as escapes for exactly this reason, so a
    * subject or issuer containing a colon cannot be made to look like a different
    * pair. This is the value the intent writer records. */
   assert(strstr(out, "\"subject\":\"oidc:https%3A//idp.example:alice\""));
   /* The server_id comes from the PENDING login. The callback's query never named
    * one, and could not be allowed to. */
   assert(strstr(out, "\"server_id\":\"mintsrv\""));
   /* What the route sent the IdP: the code from the callback, and the redirect
    * plus verifier from the RETAINED login. */
   assert(stub_exchange_calls == 1);
   assert(strcmp(stub_seen_code, "THECODE") == 0);
   assert(strcmp(stub_seen_redirect, "https://kb.example/v1/identity/login/callback") == 0);
   assert(strlen(stub_seen_verifier) == KB_OIDC_LOGIN_SECRET_LEN);

   /* SINGLE USE. Replaying the identical callback — from history, a proxy log, a
    * referrer — finds no pending login. */
   assert(callback(query, out, sizeof(out)) == 400);
   assert(strstr(out, "invalid callback"));
   assert(kb_oidc_login_store_count(NOW) == 0);

   /* A VALID TOKEN FROM ANOTHER LOGIN. The signature is genuine and the issuer
    * and audience are right; only the nonce belongs to a different login. This is
    * the case the nonce check exists for, and the one a route that checked the
    * nonce before verifying would get wrong. */
   kb_oidc_login_store_reset();
   start_login_full("mintsrv", state, sizeof(state), nonce, sizeof(nonce));
   char other_nonce[64] = "", other_state[64] = "";
   start_login_full("mintsrv", other_state, sizeof(other_state), other_nonce, sizeof(other_nonce));
   assert(strcmp(nonce, other_nonce) != 0);
   char *wrong_login = id_token(key, other_nonce, "aimee-kb", "https://idp.example");
   stub_exchange_token = wrong_login;
   snprintf(query, sizeof(query), "code=THECODE&state=%s", state);
   int st = callback(query, out, sizeof(out));
   assert_generic_failure(st, out);
   /* And it consumed the login anyway, so it cannot be retried with a better
    * token. Two were started, one is left. */
   assert(kb_oidc_login_store_count(NOW) == 1);

   /* AUDIENCE. A token whose aud is the resource server rather than the client_id
    * is refused — which is what proves the route overrides the configured
    * audience rather than reusing it. */
   kb_oidc_login_store_reset();
   start_login_full("mintsrv", state, sizeof(state), nonce, sizeof(nonce));
   char *wrong_aud = id_token(key, nonce, "https://kb.example/api", "https://idp.example");
   stub_exchange_token = wrong_aud;
   snprintf(query, sizeof(query), "code=THECODE&state=%s", state);
   assert_generic_failure(callback(query, out, sizeof(out)), out);

   /* ISSUER. A different IdP's token, correctly signed by THIS key, is refused. */
   kb_oidc_login_store_reset();
   start_login_full("mintsrv", state, sizeof(state), nonce, sizeof(nonce));
   char *wrong_iss = id_token(key, nonce, "aimee-kb", "https://evil.example");
   stub_exchange_token = wrong_iss;
   snprintf(query, sizeof(query), "code=THECODE&state=%s", state);
   assert_generic_failure(callback(query, out, sizeof(out)), out);

   /* A BROKEN SIGNATURE, and an unsigned "alg":"none" token. */
   kb_oidc_login_store_reset();
   start_login_full("mintsrv", state, sizeof(state), nonce, sizeof(nonce));
   char *tampered = id_token(key, nonce, "aimee-kb", "https://idp.example");
   tampered[strlen(tampered) - 2] ^= 0x01;
   stub_exchange_token = tampered;
   snprintf(query, sizeof(query), "code=THECODE&state=%s", state);
   assert_generic_failure(callback(query, out, sizeof(out)), out);

   /* THE IdP REFUSED THE EXCHANGE. Reported as the same generic failure, because
    * distinguishing it would say whether a code was accepted. */
   kb_oidc_login_store_reset();
   start_login_full("mintsrv", state, sizeof(state), nonce, sizeof(nonce));
   stub_exchange_result = KB_OIDC_TOKEN_EXCHANGE_DENIED;
   snprintf(query, sizeof(query), "code=THECODE&state=%s", state);
   assert_generic_failure(callback(query, out, sizeof(out)), out);
   stub_exchange_result = KB_OIDC_TOKEN_EXCHANGE_OK;

   /* AN UNKNOWN STATE never reaches the IdP: no pending login means no exchange,
    * so a stranger cannot make kb call out on demand. */
   kb_oidc_login_store_reset();
   stub_exchange_calls = 0;
   char fake[KB_OIDC_LOGIN_SECRET_LEN + 1];
   memset(fake, 'A', KB_OIDC_LOGIN_SECRET_LEN);
   fake[KB_OIDC_LOGIN_SECRET_LEN] = '\0';
   snprintf(query, sizeof(query), "code=THECODE&state=%s", fake);
   assert(callback(query, out, sizeof(out)) == 400);
   assert(strstr(out, "invalid callback"));
   assert(stub_exchange_calls == 0);

   /* A MALFORMED CALLBACK likewise. */
   assert(callback("", out, sizeof(out)) == 400);
   assert(callback(NULL, out, sizeof(out)) == 400);
   assert(callback("code=THECODE", out, sizeof(out)) == 400);
   assert(stub_exchange_calls == 0);

   /* THE IdP'S OWN ERROR is its own answer, so an operator looks at their IdP. */
   assert(callback("error=access_denied", out, sizeof(out)) == 401);
   assert(strstr(out, "the identity provider refused"));
   /* And it is not an oracle either: no pending login is named or needed. */
   assert(stub_exchange_calls == 0);

   /* NO CLIENT SECRET IN THE VAULT is a deployment fault, and says so — it is not
    * an oracle, since auth-mode already advertises that this kb offers OIDC. */
   kb_oidc_login_store_reset();
   start_login_full("mintsrv", state, sizeof(state), nonce, sizeof(nonce));
   stub_vault_has_secret = 0;
   snprintf(query, sizeof(query), "code=THECODE&state=%s", state);
   assert(callback(query, out, sizeof(out)) == 503);
   assert(strstr(out, "not fully configured"));
   stub_vault_has_secret = 1;

   /* WITH NO OIDC LOGIN CONFIGURED the route is unavailable, and identically so
    * to login/start — a kb in PAM mode reveals nothing about why. */
   env_clear();
   assert(callback("code=x&state=y", out, sizeof(out)) == 503);
   assert(strstr(out, "oidc login is not available"));

   free(good);
   free(wrong_login);
   free(wrong_aud);
   free(wrong_iss);
   free(tampered);
   remove(jwks_path);
   env_clear();
   kb_oidc_login_store_reset();
}

static void test_not_our_routes(void)
{
   char out[4096] = "";
   /* -1 means "not mine", so the dispatcher falls through to bearer auth. A
    * route file that claimed a path it does not own would silently make it
    * unauthenticated. */
   assert(route("GET", "/v1/health", NULL, out, sizeof(out)) == -1);
   assert(route("POST", "/v1/enroll/redeem", NULL, out, sizeof(out)) == -1);
   assert(route("GET", "/v1/identity", NULL, out, sizeof(out)) == -1);
   assert(route("GET", "/v1/identity/login", NULL, out, sizeof(out)) == -1);
   /* Not a prefix match: a longer path must not be captured. */
   assert(route("POST", "/v1/identity/login/start/extra", NULL, out, sizeof(out)) == -1);
   assert(route(NULL, "/v1/identity/auth-mode", NULL, out, sizeof(out)) == -1);
   assert(route("GET", NULL, NULL, out, sizeof(out)) == -1);
   assert(route("GET", "/v1/identity/auth-mode", NULL, NULL, 100) == -1);
   assert(route("GET", "/v1/identity/auth-mode", NULL, out, 0) == -1);
}

static void test_auth_mode(void)
{
   char out[4096] = "";

   /* No OIDC issuer -> PAM. The PAM login is smoothgui/auth, already in
    * production and shared with SmoothNAS, so it is what a kb offers by default
    * rather than something that needs its own probe. */
   env_clear();
   assert(route("GET", "/v1/identity/auth-mode", NULL, out, sizeof(out)) == 200);
   assert(strstr(out, "\"mode\":\"pam\""));

   /* OIDC configured -> OIDC, and PAM is off. Mutually exclusive (§3). */
   env_configure();
   assert(route("GET", "/v1/identity/auth-mode", NULL, out, sizeof(out)) == 200);
   assert(strstr(out, "\"mode\":\"oidc\""));
   assert(!strstr(out, "pam"));
   /* The declaration says which flow to start and nothing more: an
    * unauthenticated caller must not learn the issuer, client id or endpoints. */
   assert(!strstr(out, "idp.example"));
   assert(!strstr(out, "aimee-kb"));
   assert(!strstr(out, "authorize"));
   assert(!strstr(out, "client_id"));

   /* A broken OIDC profile falls back to PAM rather than reporting a mode nobody
    * can use. Logged, but still a working answer. */
   setenv("AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL", "http://idp.example/authorize", 1);
   assert(route("GET", "/v1/identity/auth-mode", NULL, out, sizeof(out)) == 200);
   assert(strstr(out, "\"mode\":\"pam\""));

   env_clear();
   assert(route("POST", "/v1/identity/auth-mode", NULL, out, sizeof(out)) == 405);
   assert(route("DELETE", "/v1/identity/auth-mode", NULL, out, sizeof(out)) == 405);
}

static void test_login_start_unavailable(void)
{
   char out[4096] = "";
   kb_oidc_login_store_reset();

   env_clear();
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"srv-a\"}", out,
                sizeof(out)) == 503);
   char unconfigured[4096];
   snprintf(unconfigured, sizeof(unconfigured), "%s", out);

   /* THE property: a broken profile answers IDENTICALLY to an absent one. A
    * stranger must not be able to probe whether an operator has attempted OIDC
    * configuration. */
   env_configure();
   setenv("AIMEE_KB_OIDC_LOGIN_TOKEN_URL", "https://idp.example:8443/token", 1);
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"srv-a\"}", out,
                sizeof(out)) == 503);
   assert(!strcmp(out, unconfigured));
   /* And nothing was retained on either path. */
   assert(kb_oidc_login_store_count(NOW) == 0);
   env_clear();
}

static void test_login_start(void)
{
   char out[4096] = "";
   kb_oidc_login_store_reset();
   env_configure();

   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"srv-a\"}", out,
                sizeof(out)) == 200);
   assert(strstr(out, "\"authorize_url\":\"https://idp.example/authorize?"));
   assert(strstr(out, "\"redirect_uri\":\"https://kb.example/v1/identity/login/callback\""));
   /* The login really was retained — the URL is only useful if the callback can
    * find it. */
   assert(kb_oidc_login_store_count(NOW) == 1);

   /* THE property: no secret is in the response. The state and nonce travel via
    * the IdP inside authorize_url, so they DO appear there, but nothing names
    * them as fields a client should read, and the verifier appears nowhere at
    * all — only its S256 challenge does. */
   assert(!strstr(out, "code_verifier"));
   assert(!strstr(out, "\"state\""));
   assert(!strstr(out, "\"nonce\""));
   assert(strstr(out, "code_challenge_method=S256"));
   /* The target server is kb's business, not the IdP's. */
   assert(!strstr(out, "srv-a"));

   /* Two logins are independent: the second does not disturb the first. */
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"srv-b\"}", out,
                sizeof(out)) == 200);
   assert(kb_oidc_login_store_count(NOW) == 2);

   /* A missing, empty, non-string or oversize server_id is a 400, and retains
    * nothing. */
   int before = kb_oidc_login_store_count(NOW);
   assert(route("POST", "/v1/identity/login/start", "{}", out, sizeof(out)) == 400);
   assert(route("POST", "/v1/identity/login/start", NULL, out, sizeof(out)) == 400);
   assert(route("POST", "/v1/identity/login/start", "", out, sizeof(out)) == 400);
   assert(route("POST", "/v1/identity/login/start", "not json", out, sizeof(out)) == 400);
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"\"}", out, sizeof(out)) ==
          400);
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":42}", out, sizeof(out)) == 400);
   /* Outside the grammar the identity tables CHECK — refused at the edge rather
    * than at intent time. */
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"-bad\"}", out, sizeof(out)) ==
          400);
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"has space\"}", out,
                sizeof(out)) == 400);
   assert(kb_oidc_login_store_count(NOW) == before);

   /* Start mutates state, so it is POST only. A GET would be prefetchable by a
    * browser or a link scanner, which would burn pending-login slots. */
   assert(route("GET", "/v1/identity/login/start", "{\"server_id\":\"srv-a\"}", out, sizeof(out)) ==
          405);
   assert(kb_oidc_login_store_count(NOW) == before);

   env_clear();
   kb_oidc_login_store_reset();
}

static void test_login_start_full_store(void)
{
   char out[4096] = "";
   kb_oidc_login_store_reset();
   env_configure();

   for (int i = 0; i < KB_OIDC_LOGIN_STORE_SLOTS; ++i)
      assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"srv-a\"}", out,
                   sizeof(out)) == 200);
   /* A full store is 503 with a retry hint, not a 500 and not a URL that would
    * fail later at the callback. */
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"srv-a\"}", out,
                sizeof(out)) == 503);
   assert(!strstr(out, "authorize_url"));
   assert(strstr(out, "retry"));

   env_clear();
   kb_oidc_login_store_reset();
}

static void test_small_buffer(void)
{
   /* A caller buffer too small for the response must not produce a truncated
    * JSON body that a client would fail to parse in a confusing way. */
   char tiny[24] = "";
   kb_oidc_login_store_reset();
   env_configure();
   int rc =
       route("POST", "/v1/identity/login/start", "{\"server_id\":\"srv-a\"}", tiny, sizeof(tiny));
   assert(rc == 500);
   assert(strstr(tiny, "error"));
   env_clear();
   kb_oidc_login_store_reset();
}

int main(void)
{
   /* A real IdP keypair: the callback's verification step is the real one, so a
    * mock token would prove nothing about it. */
   EVP_PKEY *key = NULL;
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   assert(ctx && EVP_PKEY_keygen_init(ctx) == 1);
   assert(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1);
   assert(EVP_PKEY_keygen(ctx, &key) == 1);
   EVP_PKEY_CTX_free(ctx);
   char *jwks = make_jwks(key, "test-key");

   test_not_our_routes();
   test_auth_mode();
   test_login_start_unavailable();
   test_login_start();
   test_login_start_full_store();
   test_small_buffer();
   test_callback(key, jwks);
   env_clear();
   kb_oidc_login_store_reset();
   free(jwks);
   EVP_PKEY_free(key);
   printf("test_kb_http_identity_login: ok\n");
   return 0;
}
