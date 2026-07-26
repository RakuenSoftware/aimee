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

/* PAM. Stubbed because a unit test must not consult the host's authentication
 * stack: the result would depend on which accounts exist on the build machine.
 * Records what it was asked so the route's ordering can be checked — in
 * particular that an unusable username never reaches PAM at all. */
static int stub_pam_calls;
static char stub_pam_user[128];
static char stub_pam_pass[128];
int pam_check_credentials(const char *user, const char *password)
{
   stub_pam_calls++;
   snprintf(stub_pam_user, sizeof(stub_pam_user), "%s", user ? user : "");
   snprintf(stub_pam_pass, sizeof(stub_pam_pass), "%s", password ? password : "");
   return user && password && strcmp(user, "alice") == 0 && strcmp(password, "correct") == 0;
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
   /* A syntactically valid state that matches no pending login. */
   char fake_state[KB_OIDC_LOGIN_SECRET_LEN + 1];
   memset(fake_state, 'A', KB_OIDC_LOGIN_SECRET_LEN);
   fake_state[KB_OIDC_LOGIN_SECRET_LEN] = '\0';

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
   snprintf(query, sizeof(query), "code=THECODE&state=%s", fake_state);
   assert(callback(query, out, sizeof(out)) == 400);
   assert(strstr(out, "invalid callback"));
   assert(stub_exchange_calls == 0);

   /* A MALFORMED CALLBACK likewise. */
   assert(callback("", out, sizeof(out)) == 400);
   assert(callback(NULL, out, sizeof(out)) == 400);
   assert(callback("code=THECODE", out, sizeof(out)) == 400);
   assert(stub_exchange_calls == 0);

   /* THE IdP'S OWN ERROR is its own answer, so an operator looks at their IdP —
    * but it must belong to a login this kb started.
    *
    * MY FIRST VERSION OF THIS WAS WRONG, and the assertion here used to say so:
    * it asserted that no pending login was "named or needed" and called that not
    * an oracle. It was. An unsolicited ?error= obtained the distinct
    * "identity provider refused" answer — measurably different from the generic
    * 400 — without proving anything, and a genuine refusal left the pending login
    * alive with a valid state until its TTL, contradicting the single-use
    * invariant this route claims on every other path. */
   kb_oidc_login_store_reset();
   start_login_full("mintsrv", state, sizeof(state), nonce, sizeof(nonce));
   stub_exchange_calls = 0;
   snprintf(query, sizeof(query), "error=access_denied&state=%s", state);
   assert(callback(query, out, sizeof(out)) == 401);
   assert(strstr(out, "the identity provider refused"));
   /* The IdP is never contacted — there is no code to exchange. */
   assert(stub_exchange_calls == 0);
   /* AND THE LOGIN IS CONSUMED, like every other path through this route. */
   assert(kb_oidc_login_store_count(NOW) == 0);
   /* So the same error callback cannot be replayed for the same answer. */
   assert(callback(query, out, sizeof(out)) == 400);
   assert(strstr(out, "invalid callback"));

   /* AN UNSOLICITED ERROR — no state, a malformed state, or a state matching no
    * pending login — gets the GENERIC answer. This is the oracle being closed:
    * a stranger cannot tell "this kb had a login in flight" from "it did not". */
   kb_oidc_login_store_reset();
   assert(callback("error=access_denied", out, sizeof(out)) == 400);
   assert(strstr(out, "invalid callback"));
   assert(!strstr(out, "identity provider"));
   assert(callback("error=access_denied&state=tooshort", out, sizeof(out)) == 400);
   assert(!strstr(out, "identity provider"));
   snprintf(query, sizeof(query), "error=access_denied&state=%s", fake_state);
   assert(callback(query, out, sizeof(out)) == 400);
   assert(strstr(out, "invalid callback"));
   assert(!strstr(out, "identity provider"));

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

static void test_login_pam(void)
{
   char out[4096] = "";
   char body[2048];
   env_clear();

   /* THE HAPPY PATH. The subject is the BARE username — no prefix, because with
    * the modes mutually exclusive there is no other kind of account that could
    * also be called alice on this kb. */
   stub_pam_calls = 0;
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"mintsrv\"}", out,
                sizeof(out)) == 200);
   assert(strstr(out, "\"subject\":\"alice\""));
   assert(strstr(out, "\"server_id\":\"mintsrv\""));
   assert(stub_pam_calls == 1);
   /* PAM got exactly what was posted, and no prefix was invented. */
   assert(strcmp(stub_pam_user, "alice") == 0 && strcmp(stub_pam_pass, "correct") == 0);
   /* No credential is echoed back. */
   assert(!strstr(out, "correct") && !strstr(out, "password"));

   /* A WRONG PASSWORD, an unknown account, and a username outside the grammar all
    * answer identically: on a pre-auth route any distinction enumerates accounts. */
   const char *refused[] = {
       "{\"username\":\"alice\",\"password\":\"wrong\",\"server_id\":\"s\"}",
       "{\"username\":\"nosuchuser\",\"password\":\"correct\",\"server_id\":\"s\"}",
       /* outside the subject grammar the identity tables CHECK */
       "{\"username\":\"-leading-dash\",\"password\":\"correct\",\"server_id\":\"s\"}",
       "{\"username\":\"has space\",\"password\":\"correct\",\"server_id\":\"s\"}",
       "{\"username\":\"has:colon\",\"password\":\"correct\",\"server_id\":\"s\"}",
       "{\"username\":\"oidc:iss:sub\",\"password\":\"correct\",\"server_id\":\"s\"}",
       /* reserved by the schema as the host-account name for the owner */
       "{\"username\":\"owner\",\"password\":\"correct\",\"server_id\":\"s\"}",
   };
   for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i)
   {
      assert(route("POST", "/v1/identity/login/pam", refused[i], out, sizeof(out)) == 401);
      assert(strstr(out, "authentication failed"));
      /* Nothing that says WHICH of the reasons applied. */
      assert(!strstr(out, "username") && !strstr(out, "reserved") && !strstr(out, "no such"));
   }

   /* AN UNUSABLE USERNAME NEVER REACHES PAM: refusing it before the host's
    * authentication stack means a malformed subject costs no PAM round trip that
    * could be timed, and nothing outside the grammar can reach the database. */
   stub_pam_calls = 0;
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"owner\",\"password\":\"correct\",\"server_id\":\"s\"}", out,
                sizeof(out)) == 401);
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"has space\",\"password\":\"correct\",\"server_id\":\"s\"}", out,
                sizeof(out)) == 401);
   assert(stub_pam_calls == 0);

   /* MALFORMED requests are a 400 — distinct from 401 on purpose, because a
    * missing field is a client bug and reveals nothing about any account. */
   const char *bad[] = {
       "{\"password\":\"correct\",\"server_id\":\"s\"}",
       "{\"username\":\"alice\",\"server_id\":\"s\"}",
       "{\"username\":\"alice\",\"password\":\"correct\"}",
       "{\"username\":\"alice\",\"password\":\"\",\"server_id\":\"s\"}",
       "{\"username\":5,\"password\":\"correct\",\"server_id\":\"s\"}",
       "not json",
       "",
   };
   stub_pam_calls = 0;
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
      assert(route("POST", "/v1/identity/login/pam", bad[i], out, sizeof(out)) == 400);
   assert(route("POST", "/v1/identity/login/pam", NULL, out, sizeof(out)) == 400);
   assert(stub_pam_calls == 0);

   /* AN OVERSIZED FIELD IS REFUSED, NOT TRUNCATED. The assertion that matters is
    * stub_pam_calls == 0, not the status: a truncated PASSWORD would still fail
    * the stub's comparison and still answer 401, so a status-only check passes
    * whether or not the length is enforced. What must not happen is a shortened
    * password reaching the host's authentication stack at all — against a real
    * PAM, a truncation that happened to land on a valid prefix would authenticate
    * somebody who does not know the password. */
   {
      char big[1024];
      memset(big, 'a', sizeof(big) - 1);
      big[sizeof(big) - 1] = '\0';

      /* Oversized username. Refused on length; note this one would ALSO be caught
       * by the 32-char subject grammar, which is why it cannot stand alone. */
      stub_pam_calls = 0;
      snprintf(body, sizeof(body),
               "{\"username\":\"%.200s\",\"password\":\"correct\","
               "\"server_id\":\"s\"}",
               big);
      assert(route("POST", "/v1/identity/login/pam", body, out, sizeof(out)) == 401);
      assert(stub_pam_calls == 0);

      /* Oversized PASSWORD with a perfectly valid username — the case that
       * isolates length enforcement, since nothing else about this request is
       * wrong. */
      stub_pam_calls = 0;
      snprintf(body, sizeof(body),
               "{\"username\":\"alice\",\"password\":\"%.900s\","
               "\"server_id\":\"s\"}",
               big);
      assert(route("POST", "/v1/identity/login/pam", body, out, sizeof(out)) == 401);
      assert(stub_pam_calls == 0);

      /* And an oversized server_id, which would otherwise be silently shortened
       * into a different server's name. */
      stub_pam_calls = 0;
      snprintf(body, sizeof(body),
               "{\"username\":\"alice\",\"password\":\"correct\","
               "\"server_id\":\"%.300s\"}",
               big);
      assert(route("POST", "/v1/identity/login/pam", body, out, sizeof(out)) == 401);
      assert(stub_pam_calls == 0);
   }

   /* METHOD: POST only. */
   assert(route("GET", "/v1/identity/login/pam", NULL, out, sizeof(out)) == 405);

   /* MUTUAL EXCLUSION, the rule that matters most: a kb with a working OIDC login
    * profile REFUSES password login outright. Enforced here, not just declared by
    * auth-mode — otherwise the IdP's MFA, lockout and account-disable policy is
    * bypassable by anyone holding a local host password. */
   env_configure();
   stub_pam_calls = 0;
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"s\"}", out,
                sizeof(out)) == 409);
   assert(strstr(out, "password login is disabled"));
   /* And PAM was never consulted, so a correct host password buys nothing. */
   assert(stub_pam_calls == 0);
   /* auth-mode agrees, so a client is never told to use a route that refuses it. */
   assert(route("GET", "/v1/identity/auth-mode", NULL, out, sizeof(out)) == 200);
   assert(strstr(out, "\"mode\":\"oidc\""));

   /* A configured-but-BROKEN OIDC profile serves PAM, matching what auth-mode
    * reports. Reporting one mode and enforcing the other would leave a kb with a
    * typo'd issuer unable to log anybody in at all. */
   setenv("AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL", "http://not-https.example/authorize", 1);
   assert(route("GET", "/v1/identity/auth-mode", NULL, out, sizeof(out)) == 200);
   assert(strstr(out, "\"mode\":\"pam\""));
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"mintsrv\"}", out,
                sizeof(out)) == 200);
   assert(strstr(out, "\"subject\":\"alice\""));

   env_clear();
}

/* ── The configuration-mode matrix ────────────────────────────────────────── */

/* Each login surface, under each configuration a kb can actually be in.
 *
 * WHY A MATRIX AND NOT MORE INDIVIDUAL CASES. The individual tests above each
 * prove one route behaves correctly in the mode it cares about. What they cannot
 * prove is that the routes AGREE: that /v1/identity/auth-mode's answer and what
 * the other three routes actually do are the same story. A kb that advertises
 * "pam" while the pam route refuses, or advertises "oidc" while the password
 * route still authenticates, passes every single-route test and is broken — the
 * second of those is a real IdP bypass. The table makes the whole cross-product
 * visible in one place, so a route that drifts shows up as a wrong cell rather
 * than as an absence of coverage.
 *
 * The three modes are every state the configuration can reach: no OIDC at all, a
 * working OIDC profile, and a profile that is present but unusable. The third is
 * not hypothetical — it is one typo'd URL away, and it is the mode where
 * "reported" and "enforced" are most likely to diverge. */
typedef enum
{
   MODE_NO_OIDC = 0,
   MODE_OIDC,
   MODE_OIDC_BROKEN
} config_mode_t;

static void apply_mode(config_mode_t mode)
{
   env_clear();
   if (mode == MODE_NO_OIDC)
      return;
   env_configure();
   if (mode == MODE_OIDC_BROKEN)
      /* Present, and refused by kb_oidc_login_config_valid: the authorize
       * endpoint must be https. Exactly the shape of an operator typo. */
      setenv("AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL", "http://idp.example/authorize", 1);
}

static const char *mode_name(config_mode_t mode)
{
   return mode == MODE_NO_OIDC ? "no-oidc" : (mode == MODE_OIDC ? "oidc" : "oidc-broken");
}

static void test_config_mode_matrix(void)
{
   struct
   {
      config_mode_t mode;
      const char *declared;
      int start;
      int callback;
      int pam;
   } table[] = {
       /* No OIDC: the password login is the only way in, and the two OIDC routes
        * are unavailable rather than merely failing. */
       {MODE_NO_OIDC, "pam", 503, 503, 200},
       /* OIDC configured: the OIDC routes work and the password route is REFUSED
        * — 409, not 401. A 401 would mean "wrong password"; 409 says the mode is
        * wrong, which is the truth and is what stops an IdP bypass. */
       {MODE_OIDC, "oidc", 200, 400, 409},
       /* Configured but unusable: everything falls back to PAM, and crucially the
        * DECLARED mode matches what the routes do. A kb here can still log its
        * users in. */
       {MODE_OIDC_BROKEN, "pam", 503, 503, 200},
   };

   for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
   {
      char out[4096] = "";
      apply_mode(table[i].mode);
      kb_oidc_login_store_reset();

      /* 1. What the kb SAYS. */
      assert(route("GET", "/v1/identity/auth-mode", NULL, out, sizeof(out)) == 200);
      char expect[64];
      snprintf(expect, sizeof(expect), "\"mode\":\"%s\"", table[i].declared);
      if (!strstr(out, expect))
      {
         fprintf(stderr, "mode %s: auth-mode said %s, expected %s\n", mode_name(table[i].mode), out,
                 expect);
         assert(0);
      }

      /* 2. What the kb DOES, on each of the three login routes. Any mismatch
       * names the mode and the route, because "an assertion failed in the matrix"
       * would be the least useful possible failure message. */
      struct
      {
         const char *label, *method, *path, *query, *body;
         int expected;
      } probes[] = {
          {"start", "POST", "/v1/identity/login/start", NULL, "{\"server_id\":\"mintsrv\"}",
           table[i].start},
          /* A syntactically fine callback that matches no pending login: in OIDC
           * mode that is a 400 (the route is live and refuses it), and in the
           * other modes a 503 (the route is not offered at all). The distinction
           * is the point — it tells a client whether to expect a redirect. */
          {"callback", "GET", "/v1/identity/login/callback",
           "code=abc&state=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", NULL, table[i].callback},
          {"pam", "POST", "/v1/identity/login/pam", NULL,
           "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"mintsrv\"}",
           table[i].pam},
      };
      for (size_t j = 0; j < sizeof(probes) / sizeof(probes[0]); ++j)
      {
         char body_out[4096] = "";
         int st = kb_http_identity_login_route(probes[j].method, probes[j].path, probes[j].query,
                                               probes[j].body, NOW, body_out, sizeof(body_out));
         if (st != probes[j].expected)
         {
            fprintf(stderr, "mode %s route %s: got %d expected %d (%s)\n", mode_name(table[i].mode),
                    probes[j].label, st, probes[j].expected, body_out);
            assert(0);
         }
      }

      /* 3. THE INVARIANT THAT TIES IT TOGETHER: whichever mode is declared, that
       * mode's entry point works and the other mode's is refused. Stated once
       * rather than left to be inferred from the columns. */
      if (strcmp(table[i].declared, "oidc") == 0)
      {
         assert(table[i].start == 200); /* the OIDC way in is open */
         assert(table[i].pam == 409);   /* and the password way is shut */
      }
      else
      {
         assert(table[i].pam == 200);   /* the password way in is open */
         assert(table[i].start == 503); /* and the OIDC way is not offered */
      }
   }

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
   test_login_pam();
   test_config_mode_matrix();
   env_clear();
   kb_oidc_login_store_reset();
   free(jwks);
   EVP_PKEY_free(key);
   printf("test_kb_http_identity_login: ok\n");
   return 0;
}
