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
 */
#include "kb_http_identity_login.h"

#include "kb_oidc_login.h"
#include "kb_oidc_login_store.h"

#include <assert.h>
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
   return kb_http_identity_login_route(method, path, body, NOW, out, cap);
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
   test_not_our_routes();
   test_auth_mode();
   test_login_start_unavailable();
   test_login_start();
   test_login_start_full_store();
   test_small_buffer();
   env_clear();
   kb_oidc_login_store_reset();
   printf("test_kb_http_identity_login: ok\n");
   return 0;
}
