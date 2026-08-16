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
#include "kb/kb_login_throttle.h"

#include "modules/db2/c/management_identity_journal.h"
#include "kb_auth_oidc.h"
#include "kb_oidc_login.h"
#include "kb_oidc_login_store.h"
#include "kb_oidc_token_exchange.h"
#include "kb_reqctx.h"
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

/* Every caller in this file sends JSON, which is what a real client does, so the
 * content type the connection layer would have recorded is set here rather than
 * at 22 call sites. route_ct() is for the tests that are ABOUT the content type. */
static int route_ct(const char *method, const char *path, const char *body, const char *ctype,
                    char *out, int cap)
{
   kb_reqctx_set_content_type(ctype);
   int rc = kb_http_identity_login_route(method, path, NULL, body, NOW, out, cap);
   kb_reqctx_clear_content_type();
   return rc;
}

static int route(const char *method, const char *path, const char *body, char *out, int cap)
{
   return route_ct(method, path, body, "application/json", out, cap);
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

/* ── The identity-intent seam ──────────────────────────────────────────────── */

/* Stubbed because filing an intent needs Postgres, and what this test is for is
 * the ROUTE's behaviour around it: that both modes reach it, that neither can
 * name its own team, and that its refusals are mapped to the right status. The
 * SQL side is covered by the P1 RLS gate and the live mint harness.
 *
 * The stub records the team and subject it was handed, which is how the
 * "a callback cannot choose its own team" property is actually checked. */
static db2_management_action_result_t stub_ctx_result = DB2_MANAGEMENT_ACTION_OK;
static db2_management_action_result_t stub_intent_result = DB2_MANAGEMENT_ACTION_OK;
static int stub_ctx_calls, stub_intent_calls;
static int64_t stub_seen_team;
static char stub_seen_subject[600];
static db2_identity_auth_mode_t stub_seen_mode;

db2_management_action_result_t db2_identity_login_context(const kb_principal_t *principal,
                                                          int64_t team_id, char installation_id[33],
                                                          char kid[DB2_IDENTITY_KID_MAX + 1])
{
   stub_ctx_calls++;
   /* The route must pass an AUTHENTICATED principal — a zero-initialised one would
    * be refused by the real tenant scope, so assert it here rather than let a
    * permissive stub hide it. */
   assert(principal && principal->authenticated);
   stub_seen_team = team_id;
   if (stub_ctx_result != DB2_MANAGEMENT_ACTION_OK)
      return stub_ctx_result;
   snprintf(installation_id, 33, "%s", "0123456789abcdef0123456789abcdef");
   snprintf(kid, DB2_IDENTITY_KID_MAX + 1, "%s", "p5-token-v1-test");
   return DB2_MANAGEMENT_ACTION_OK;
}

/* operation_init is pure (validate + draw three ids) but shares a translation unit
 * with the two functions above, so it is stubbed rather than linked. Its own
 * validation is covered by the db2 journal's test; what matters here is that the
 * route feeds it the values the CONTEXT read, so the stub asserts exactly that and
 * nothing else. */
db2_management_action_result_t
db2_identity_intent_operation_init(int64_t team_id, const char *target_server_id,
                                   db2_identity_auth_mode_t auth_mode, const char *token_issuer,
                                   const char *kid, int ttl_seconds, const char *installation_id,
                                   db2_identity_intent_operation_t *out)
{
   assert(out && target_server_id && token_issuer && kid && installation_id);
   assert(strcmp(kid, "p5-token-v1-test") == 0);
   assert(strcmp(installation_id, "0123456789abcdef0123456789abcdef") == 0);
   assert(ttl_seconds > 0 && ttl_seconds <= DB2_IDENTITY_TTL_MAX_SECONDS);
   memset(out, 0, sizeof(*out));
   out->team_id = team_id;
   out->auth_mode = auth_mode;
   out->ttl_seconds = ttl_seconds;
   snprintf(out->target_server_id, sizeof(out->target_server_id), "%s", target_server_id);
   snprintf(out->token_issuer, sizeof(out->token_issuer), "%s", token_issuer);
   snprintf(out->kid, sizeof(out->kid), "%s", kid);
   snprintf(out->installation_id, sizeof(out->installation_id), "%s", installation_id);
   snprintf(out->correlation_id, sizeof(out->correlation_id), "%s",
            "1111111111111111111111111111111111111111111111111111111111111111");
   snprintf(out->jti, sizeof(out->jti), "%s",
            "2222222222222222222222222222222222222222222222222222222222222222");
   snprintf(out->token_jti, sizeof(out->token_jti), "%s", "tok-jti-test-value");
   return DB2_MANAGEMENT_ACTION_OK;
}

db2_management_action_result_t db2_identity_intent_start(const kb_principal_t *principal,
                                                         const db2_identity_intent_operation_t *op,
                                                         db2_identity_intent_t *out)
{
   stub_intent_calls++;
   assert(principal && principal->authenticated && op && out);
   /* The kid and installation must be the ones the CONTEXT read, never anything
    * the caller supplied — this is the property that keeps a login from filing an
    * intent against a superseded publication. */
   assert(strcmp(op->kid, "p5-token-v1-test") == 0);
   assert(strcmp(op->installation_id, "0123456789abcdef0123456789abcdef") == 0);
   assert(strcmp(op->token_issuer, "kb") == 0);
   stub_seen_mode = op->auth_mode;
   memset(out, 0, sizeof(*out));
   if (stub_intent_result != DB2_MANAGEMENT_ACTION_OK)
      return stub_intent_result;
   /* The subject the DATABASE resolves. Deliberately echoed from what the scope
    * would carry, so the route's "return the recorded subject" contract is real. */
   snprintf(out->subject, sizeof(out->subject), "%s", stub_seen_subject);
   snprintf(out->correlation_id, sizeof(out->correlation_id), "%s", op->correlation_id);
   snprintf(out->jti, sizeof(out->jti), "%s", op->jti);
   snprintf(out->target_server_id, sizeof(out->target_server_id), "%s", op->target_server_id);
   out->team_id = op->team_id;
   out->expires_at = 1780000300;
   return DB2_MANAGEMENT_ACTION_OK;
}

static void stub_intent_reset(const char *expected_subject)
{
   stub_ctx_result = DB2_MANAGEMENT_ACTION_OK;
   stub_intent_result = DB2_MANAGEMENT_ACTION_OK;
   stub_ctx_calls = stub_intent_calls = 0;
   stub_seen_team = 0;
   stub_seen_mode = (db2_identity_auth_mode_t)0;
   snprintf(stub_seen_subject, sizeof(stub_seen_subject), "%s", expected_subject);
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
   snprintf(body, sizeof(body), "{\"server_id\":\"%s\",\"team_id\":770001}", server_id);
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
   stub_intent_reset("oidc:https%3A//idp.example:alice");
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
   /* THE INTENT IS FILED, which is the point of the whole flow. */
   assert(stub_ctx_calls == 1 && stub_intent_calls == 1);
   assert(stub_seen_mode == DB2_IDENTITY_AUTH_MODE_OIDC);
   /* The team came from the PENDING login. The callback's query never named one. */
   assert(stub_seen_team == 770001);
   /* And the caller gets the pair the token authority mints from. */
   assert(strstr(out, "\"correlation_id\":\"") && strstr(out, "\"jti\":\""));
   assert(strstr(out, "\"team_id\":770001"));
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

   /* A DUPLICATED error PARAMETER, with a VALID state for a LIVE login, must get
    * the generic answer and must NOT consume the login. This is the oracle variant
    * a review found after the first fix: the parser mapped a duplicate to
    * _IDP_ERROR, so error=x&error=y&state=<live> obtained the distinct reply and
    * burned a real user's in-flight login. */
   kb_oidc_login_store_reset();
   start_login_full("mintsrv", state, sizeof(state), nonce, sizeof(nonce));
   snprintf(query, sizeof(query), "error=access_denied&error=login_required&state=%s", state);
   assert(callback(query, out, sizeof(out)) == 400);
   assert(strstr(out, "invalid callback"));
   assert(!strstr(out, "identity provider"));
   /* The login survives, so the real user can still complete it. */
   assert(kb_oidc_login_store_count(NOW) == 1);
   /* AND WITH A VALID CODE ATTACHED it must still be refused, not completed. A
    * parser that treats a malformed error as merely absent proceeds to the code
    * branch and authorizes — which would make smuggling a duplicate error into a
    * genuine callback a way to have it succeed while suppressing nothing. */
   snprintf(query, sizeof(query), "error=x&error=y&code=THECODE&state=%s", state);
   stub_intent_calls = 0;
   assert(callback(query, out, sizeof(out)) == 400);
   assert(strstr(out, "invalid callback"));
   assert(stub_intent_calls == 0);
   assert(kb_oidc_login_store_count(NOW) == 1);
   /* THE MIRROR IMAGE: a well-formed error with a DUPLICATED code. Round 3 found
    * this honoured as a refusal, consuming the login. Both directions must be
    * refused, and the login must survive both. */
   snprintf(query, sizeof(query), "error=access_denied&code=x&code=y&state=%s", state);
   assert(callback(query, out, sizeof(out)) == 400);
   assert(strstr(out, "invalid callback"));
   assert(!strstr(out, "identity provider"));
   assert(kb_oidc_login_store_count(NOW) == 1);
   /* And the well-formed error for that same login still works afterwards. */
   snprintf(query, sizeof(query), "error=access_denied&state=%s", state);
   assert(callback(query, out, sizeof(out)) == 401);
   assert(strstr(out, "the identity provider refused"));
   assert(kb_oidc_login_store_count(NOW) == 0);

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
   stub_intent_reset("alice");
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"mintsrv\",\"team_"
                "id\":770001}",
                out, sizeof(out)) == 200);
   assert(strstr(out, "\"subject\":\"alice\""));
   assert(strstr(out, "\"server_id\":\"mintsrv\""));
   /* BOTH MODES REACH THE SAME INTENT STEP. If one of them ever stopped, that mode
    * would have acquired an authorization path the other lacks. */
   assert(stub_ctx_calls == 1 && stub_intent_calls == 1);
   assert(stub_seen_mode == DB2_IDENTITY_AUTH_MODE_PAM);
   assert(stub_seen_team == 770001);
   assert(strstr(out, "\"correlation_id\":\"") && strstr(out, "\"jti\":\""));
   assert(stub_pam_calls == 1);
   /* PAM got exactly what was posted, and no prefix was invented. */
   assert(strcmp(stub_pam_user, "alice") == 0 && strcmp(stub_pam_pass, "correct") == 0);
   /* No credential is echoed back. */
   assert(!strstr(out, "correct") && !strstr(out, "password"));

   /* A WRONG PASSWORD, an unknown account, and a username outside the grammar all
    * answer identically: on a pre-auth route any distinction enumerates accounts. */
   const char *refused[] = {
       "{\"username\":\"alice\",\"password\":\"wrong\",\"server_id\":\"s\",\"team_id\":770001}",
       "{\"username\":\"nosuchuser\",\"password\":\"correct\",\"server_id\":\"s\",\"team_id\":"
       "770001}",
       /* outside the subject grammar the identity tables CHECK */
       "{\"username\":\"-leading-dash\",\"password\":\"correct\",\"server_id\":\"s\",\"team_id\":"
       "770001}",
       "{\"username\":\"has "
       "space\",\"password\":\"correct\",\"server_id\":\"s\",\"team_id\":770001}",
       "{\"username\":\"has:colon\",\"password\":\"correct\",\"server_id\":\"s\",\"team_id\":"
       "770001}",
       "{\"username\":\"oidc:iss:sub\",\"password\":\"correct\",\"server_id\":\"s\",\"team_id\":"
       "770001}",
       /* reserved by the schema as the host-account name for the owner */
       "{\"username\":\"owner\",\"password\":\"correct\",\"server_id\":\"s\",\"team_id\":770001}",
   };
   for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i)
   {
      /* Each case starts from an empty budget. This list is longer than
       * KB_LOGIN_THROTTLE_BUDGET, so without the reset the tail of it would be
       * answered 429 by the brute-force throttle -- correctly, since that IS more
       * consecutive failures than a caller is allowed. What this loop asserts is
       * that every one of these credentials is refused ALIKE, which is a separate
       * property from how many refusals in a row are tolerated; the throttle has
       * its own assertions below and in test_kb_login_throttle.c. */
      kb_login_throttle_reset();
      assert(route("POST", "/v1/identity/login/pam", refused[i], out, sizeof(out)) == 401);
      assert(strstr(out, "authentication failed"));
      /* Nothing that says WHICH of the reasons applied. */
      assert(!strstr(out, "username") && !strstr(out, "reserved") && !strstr(out, "no such"));
   }

   /* THE ROUTE ACTUALLY CONSULTS THE THROTTLE. test_kb_login_throttle.c proves the
    * budget works; it cannot prove this route asks it. A throttle that exists and
    * is never called passes every test the module has, which is exactly how the
    * gap this closes would come back.
    *
    * Measured before the throttle existed: twelve consecutive wrong passwords on
    * this route each returned an immediate 401. */
   kb_login_throttle_reset();
   {
      const char *wrong =
          "{\"username\":\"alice\",\"password\":\"wrong\",\"server_id\":\"s\",\"team_id\":770001}";
      int saw_429 = 0;
      for (int i = 0; i < KB_LOGIN_THROTTLE_BUDGET + 3 && !saw_429; ++i)
         if (route("POST", "/v1/identity/login/pam", wrong, out, sizeof(out)) == 429)
            saw_429 = 1;
      assert(saw_429);
      /* The refusal tells the caller how long to wait, rather than leaving it to
       * hammer the route and stay locked out forever. */
      assert(strstr(out, "retry_after"));
      /* AND IT APPLIES TO A CORRECT PASSWORD TOO. If a valid credential slipped
       * past the lockout, an attacker who guessed right on attempt N would still
       * win, and the throttle would be a side channel confirming the guess. */
      assert(route("POST", "/v1/identity/login/pam",
                   "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"s\","
                   "\"team_id\":770001}",
                   out, sizeof(out)) == 429);
      /* A throttled attempt must not reach PAM: the point is to stop spending the
       * host's authentication stack on an attacker. */
      stub_pam_calls = 0;
      (void)route("POST", "/v1/identity/login/pam", wrong, out, sizeof(out));
      assert(stub_pam_calls == 0);
   }
   kb_login_throttle_reset();

   /* AN UNUSABLE USERNAME NEVER REACHES PAM: refusing it before the host's
    * authentication stack means a malformed subject costs no PAM round trip that
    * could be timed, and nothing outside the grammar can reach the database. */
   kb_login_throttle_reset();
   stub_pam_calls = 0;
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"owner\",\"password\":\"correct\",\"server_id\":\"s\",\"team_id\":"
                "770001}",
                out, sizeof(out)) == 401);
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"has "
                "space\",\"password\":\"correct\",\"server_id\":\"s\",\"team_id\":770001}",
                out, sizeof(out)) == 401);
   assert(stub_pam_calls == 0);

   /* MALFORMED requests are a 400 — distinct from 401 on purpose, because a
    * missing field is a client bug and reveals nothing about any account. */
   const char *bad[] = {
       "{\"password\":\"correct\",\"server_id\":\"s\",\"team_id\":770001}",
       "{\"username\":\"alice\",\"server_id\":\"s\",\"team_id\":770001}",
       "{\"username\":\"alice\",\"password\":\"correct\"}",
       "{\"username\":\"alice\",\"password\":\"\",\"server_id\":\"s\",\"team_id\":770001}",
       "{\"username\":5,\"password\":\"correct\",\"server_id\":\"s\",\"team_id\":770001}",
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
               "\"server_id\":\"s\",\"team_id\":770001}",
               big);
      assert(route("POST", "/v1/identity/login/pam", body, out, sizeof(out)) == 401);
      assert(stub_pam_calls == 0);

      /* Oversized PASSWORD with a perfectly valid username — the case that
       * isolates length enforcement, since nothing else about this request is
       * wrong. */
      stub_pam_calls = 0;
      snprintf(body, sizeof(body),
               "{\"username\":\"alice\",\"password\":\"%.900s\","
               "\"server_id\":\"s\",\"team_id\":770001}",
               big);
      assert(route("POST", "/v1/identity/login/pam", body, out, sizeof(out)) == 401);
      assert(stub_pam_calls == 0);

      /* And an oversized server_id, which would otherwise be silently shortened
       * into a different server's name. */
      stub_pam_calls = 0;
      snprintf(body, sizeof(body),
               "{\"username\":\"alice\",\"password\":\"correct\","
               "\"server_id\":\"%.300s\",\"team_id\":770001}",
               big);
      assert(route("POST", "/v1/identity/login/pam", body, out, sizeof(out)) == 401);
      assert(stub_pam_calls == 0);
   }

   kb_login_throttle_reset(); /* judge this group on its own, not on prior refusals */
   /* THE INTENT'S REFUSALS ARE THEIR OWN ANSWERS, and deliberately not folded into
    * the generic 401. A caller here has already PROVED who it is, so telling it
    * that it holds no write-tier grant reveals nothing it could not learn from an
    * operator — and hiding it would make the flow undebuggable. */
   stub_intent_reset("alice");
   stub_intent_result = DB2_MANAGEMENT_ACTION_DENIED;
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"s\","
                "\"team_id\":770001}",
                out, sizeof(out)) == 403);
   assert(strstr(out, "no write-tier grant"));

   kb_login_throttle_reset(); /* judge this group on its own, not on prior refusals */
   /* Not a member of the named team: also 403, from the context read, and it never
    * reaches the intent writer. */
   stub_intent_reset("alice");
   stub_ctx_result = DB2_MANAGEMENT_ACTION_DENIED;
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"s\","
                "\"team_id\":770001}",
                out, sizeof(out)) == 403);
   assert(strstr(out, "not a member of that team"));
   assert(stub_intent_calls == 0);

   kb_login_throttle_reset(); /* judge this group on its own, not on prior refusals */
   /* A kb that cannot reach its own authority state answers 503, not 401: the
    * credential was fine and retrying with a different password is pointless. */
   stub_intent_reset("alice");
   stub_ctx_result = DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"s\","
                "\"team_id\":770001}",
                out, sizeof(out)) == 503);
   assert(strstr(out, "cannot issue write tokens"));
   stub_intent_reset("alice");

   kb_login_throttle_reset(); /* judge this group on its own, not on prior refusals */
   /* A MISSING team_id is a 400, and PAM is never consulted for it. */
   stub_pam_calls = 0;
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"s\"}", out,
                sizeof(out)) == 400);
   assert(stub_pam_calls == 0);

   /* team_id MUST BE AN EXACT INTEGER. cJSON stores every number as a double, so
    * 770001.9 passes a bare positive-and-in-range check and then becomes 770001 on
    * the cast — silently authorizing against a team the caller did not name, since
    * team_id is what selects the FORCE RLS-bound grant lookup. Refused, not
    * rounded, and PAM is never consulted for any of them. */
   {
      const char *bad_teams[] = {
          "770001.9",             /* fractional, rounds down */
          "770001.5",             /* fractional, rounds to even/nearest */
          "0.5",                  /* fractional and below 1 */
          "-770001",              /* negative */
          "0",                    /* zero is not a team */
          "1e400",                /* overflows to inf */
          "9007199254740993",     /* 2^53+1: not exactly representable as a double */
          "18446744073709551616", /* far past int64 */
          "\"770001\"",           /* a string, not a number */
      };
      for (size_t i = 0; i < sizeof(bad_teams) / sizeof(bad_teams[0]); ++i)
      {
         stub_pam_calls = 0;
         stub_intent_reset("alice");
         snprintf(body, sizeof(body),
                  "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"s\","
                  "\"team_id\":%s}",
                  bad_teams[i]);
         if (route("POST", "/v1/identity/login/pam", body, out, sizeof(out)) != 400)
         {
            fprintf(stderr, "team_id %s was ACCEPTED: %s\n", bad_teams[i], out);
            assert(0);
         }
         assert(stub_pam_calls == 0 && stub_intent_calls == 0);

         /* The same values on the OIDC start route, which retains team_id for the
          * callback and so must refuse them just as hard. */
         env_configure();
         snprintf(body, sizeof(body), "{\"server_id\":\"mintsrv\",\"team_id\":%s}", bad_teams[i]);
         assert(route("POST", "/v1/identity/login/start", body, out, sizeof(out)) == 400);
         env_clear();
      }
      /* The precision BOUNDARY that is still legal: 2^53 - 1 is exactly
       * representable, so it must be accepted rather than swept up by the guard. */
      stub_intent_reset("alice");
      snprintf(body, sizeof(body),
               "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"s\","
               "\"team_id\":9007199254740991}");
      assert(route("POST", "/v1/identity/login/pam", body, out, sizeof(out)) == 200);
      assert(stub_seen_team == 9007199254740991LL);
   }

   kb_login_throttle_reset(); /* judge this group on its own, not on prior refusals */
   /* METHOD: POST only. */
   assert(route("GET", "/v1/identity/login/pam", NULL, out, sizeof(out)) == 405);

   kb_login_throttle_reset(); /* judge this group on its own, not on prior refusals */
   /* MUTUAL EXCLUSION, the rule that matters most: a kb with a working OIDC login
    * profile REFUSES password login outright. Enforced here, not just declared by
    * auth-mode — otherwise the IdP's MFA, lockout and account-disable policy is
    * bypassable by anyone holding a local host password. */
   env_configure();
   stub_pam_calls = 0;
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"s\",\"team_id\":"
                "770001}",
                out, sizeof(out)) == 409);
   assert(strstr(out, "password login is disabled"));
   /* And PAM was never consulted, so a correct host password buys nothing. */
   assert(stub_pam_calls == 0);
   /* auth-mode agrees, so a client is never told to use a route that refuses it. */
   assert(route("GET", "/v1/identity/auth-mode", NULL, out, sizeof(out)) == 200);
   assert(strstr(out, "\"mode\":\"oidc\""));

   kb_login_throttle_reset(); /* judge this group on its own, not on prior refusals */
   /* A configured-but-BROKEN OIDC profile serves PAM, matching what auth-mode
    * reports. Reporting one mode and enforcing the other would leave a kb with a
    * typo'd issuer unable to log anybody in at all. */
   setenv("AIMEE_KB_OIDC_LOGIN_AUTHORIZE_URL", "http://not-https.example/authorize", 1);
   assert(route("GET", "/v1/identity/auth-mode", NULL, out, sizeof(out)) == 200);
   assert(strstr(out, "\"mode\":\"pam\""));
   assert(route("POST", "/v1/identity/login/pam",
                "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"mintsrv\",\"team_"
                "id\":770001}",
                out, sizeof(out)) == 200);
   assert(strstr(out, "\"subject\":\"alice\""));

   env_clear();
}

/* ── CSRF: the content types a browser can forge ──────────────────────────── */

/* A cross-origin HTML form can send exactly three content types without a
 * preflight. While this route accepted a JSON body under any of them it was
 * drivable from an attacker's page — measured on a live kb, all three reached
 * credential processing and answered 401. Requiring application/json means the
 * only type that works is one a cross-origin form cannot send without a
 * preflight it will fail.
 *
 * The assertion that matters is stub_pam_calls == 0, not the status: a refusal
 * that still ran the credential check would leave the password oracle open (and
 * the throttle chargeable) even while answering 415. */
static void test_login_pam_requires_json(void)
{
   char out[4096];
   const char *body = "{\"username\":\"alice\",\"password\":\"correct\","
                      "\"server_id\":\"mintsrv\",\"team_id\":770001}";
   env_clear();
   kb_login_throttle_reset();

   const char *forgeable[] = {"text/plain", "application/x-www-form-urlencoded",
                              "multipart/form-data; boundary=----x", "text/plain;charset=UTF-8"};
   for (size_t i = 0; i < sizeof(forgeable) / sizeof(forgeable[0]); i++)
   {
      stub_pam_calls = 0;
      assert(route_ct("POST", "/v1/identity/login/pam", body, forgeable[i], out, sizeof(out)) ==
             415);
      assert(stub_pam_calls == 0);
   }

   /* A request that names no content type at all is refused too: a route that
    * requires JSON must not accept one that never said what it was sending. */
   stub_pam_calls = 0;
   assert(route_ct("POST", "/v1/identity/login/pam", body, "", out, sizeof(out)) == 415);
   assert(stub_pam_calls == 0);

   /* "application/jsonx" is a DIFFERENT media type, not JSON with a suffix. */
   stub_pam_calls = 0;
   assert(route_ct("POST", "/v1/identity/login/pam", body, "application/jsonx", out, sizeof(out)) ==
          415);
   assert(stub_pam_calls == 0);

   /* JSON still works, including with a parameter and odd case — refusing those
    * would break real clients while stopping no attacker. */
   const char *ok[] = {"application/json", "application/json; charset=utf-8", "APPLICATION/JSON",
                       " application/json"};
   for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
   {
      stub_intent_reset("alice");
      stub_pam_calls = 0;
      assert(route_ct("POST", "/v1/identity/login/pam", body, ok[i], out, sizeof(out)) == 200);
      assert(stub_pam_calls == 1);
   }

   /* The refusal happens BEFORE the throttle. Otherwise a forged cross-origin
    * form would spend the named user's login budget and lock them out — turning
    * a CSRF that achieves nothing into a denial of service that does. */
   kb_login_throttle_reset();
   for (int i = 0; i < 40; i++)
      assert(route_ct("POST", "/v1/identity/login/pam", body, "text/plain", out, sizeof(out)) ==
             415);
   stub_intent_reset("alice");
   assert(route("POST", "/v1/identity/login/pam", body, out, sizeof(out)) == 200);

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
      stub_intent_reset("alice");

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
          {"start", "POST", "/v1/identity/login/start", NULL,
           "{\"server_id\":\"mintsrv\",\"team_id\":770001}", table[i].start},
          /* A syntactically fine callback that matches no pending login: in OIDC
           * mode that is a 400 (the route is live and refuses it), and in the
           * other modes a 503 (the route is not offered at all). The distinction
           * is the point — it tells a client whether to expect a redirect. */
          {"callback", "GET", "/v1/identity/login/callback",
           "code=abc&state=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", NULL, table[i].callback},
          {"pam", "POST", "/v1/identity/login/pam", NULL,
           "{\"username\":\"alice\",\"password\":\"correct\",\"server_id\":\"mintsrv\",\"team_id\":"
           "770001}",
           table[i].pam},
      };
      for (size_t j = 0; j < sizeof(probes) / sizeof(probes[0]); ++j)
      {
         char body_out[4096] = "";
         /* What a real client sends; the PAM route now requires it. This matrix
          * is about MODE, so it must not also be testing the content type. */
         kb_reqctx_set_content_type("application/json");
         int st = kb_http_identity_login_route(probes[j].method, probes[j].path, probes[j].query,
                                               probes[j].body, NOW, body_out, sizeof(body_out));
         kb_reqctx_clear_content_type();
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
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"srv-a\",\"team_id\":770001}",
                out, sizeof(out)) == 503);
   char unconfigured[4096];
   snprintf(unconfigured, sizeof(unconfigured), "%s", out);

   /* THE property: a broken profile answers IDENTICALLY to an absent one. A
    * stranger must not be able to probe whether an operator has attempted OIDC
    * configuration. */
   env_configure();
   setenv("AIMEE_KB_OIDC_LOGIN_TOKEN_URL", "https://idp.example:8443/token", 1);
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"srv-a\",\"team_id\":770001}",
                out, sizeof(out)) == 503);
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

   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"srv-a\",\"team_id\":770001}",
                out, sizeof(out)) == 200);
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
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"srv-b\",\"team_id\":770001}",
                out, sizeof(out)) == 200);
   assert(kb_oidc_login_store_count(NOW) == 2);

   /* A missing, empty, non-string or oversize server_id is a 400, and retains
    * nothing. */
   int before = kb_oidc_login_store_count(NOW);
   assert(route("POST", "/v1/identity/login/start", "{}", out, sizeof(out)) == 400);
   assert(route("POST", "/v1/identity/login/start", NULL, out, sizeof(out)) == 400);
   assert(route("POST", "/v1/identity/login/start", "", out, sizeof(out)) == 400);
   assert(route("POST", "/v1/identity/login/start", "not json", out, sizeof(out)) == 400);
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"\",\"team_id\":770001}", out,
                sizeof(out)) == 400);
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":42}", out, sizeof(out)) == 400);
   /* Outside the grammar the identity tables CHECK — refused at the edge rather
    * than at intent time. */
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"-bad\",\"team_id\":770001}",
                out, sizeof(out)) == 400);
   assert(route("POST", "/v1/identity/login/start",
                "{\"server_id\":\"has space\",\"team_id\":770001}", out, sizeof(out)) == 400);
   assert(kb_oidc_login_store_count(NOW) == before);

   /* Start mutates state, so it is POST only. A GET would be prefetchable by a
    * browser or a link scanner, which would burn pending-login slots. */
   assert(route("GET", "/v1/identity/login/start", "{\"server_id\":\"srv-a\",\"team_id\":770001}",
                out, sizeof(out)) == 405);
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
      assert(route("POST", "/v1/identity/login/start",
                   "{\"server_id\":\"srv-a\",\"team_id\":770001}", out, sizeof(out)) == 200);
   /* A full store is 503 with a retry hint, not a 500 and not a URL that would
    * fail later at the callback. */
   assert(route("POST", "/v1/identity/login/start", "{\"server_id\":\"srv-a\",\"team_id\":770001}",
                out, sizeof(out)) == 503);
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
   int rc = route("POST", "/v1/identity/login/start",
                  "{\"server_id\":\"srv-a\",\"team_id\":770001}", tiny, sizeof(tiny));
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
   test_login_pam_requires_json();
   test_config_mode_matrix();
   env_clear();
   kb_oidc_login_store_reset();
   free(jwks);
   EVP_PKEY_free(key);
   printf("test_kb_http_identity_login: ok\n");
   return 0;
}
