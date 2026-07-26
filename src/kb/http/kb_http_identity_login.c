/* kb_http_identity_login.c — see kb_http_identity_login.h. */

#include "kb_http_identity_login.h"

#include "cJSON.h"
#include "pam_auth.h" /* pam_check_credentials — the one PAM policy, shared with the dashboard */
#include "db2/management_intent_fields.h" /* db2_intent_bare_username (header-only) */
#include "kb_auth_oidc.h"
#include "kb_oidc_login.h"
#include "kb_oidc_login_store.h"
#include "kb_oidc_token_exchange.h"
#include "log.h"
#include "vault_service.h"

#include <openssl/crypto.h> /* OPENSSL_cleanse */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int json_error(char *out_buf, int out_cap, int status, const char *message)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "error", message);
   char *text = o ? cJSON_PrintUnformatted(o) : NULL;
   snprintf(out_buf, (size_t)out_cap, "%s", text ? text : "{\"error\":\"internal error\"}");
   if (text)
      free(text);
   cJSON_Delete(o);
   return status;
}

static int json_body(char *out_buf, int out_cap, int status, cJSON *o)
{
   char *text = o ? cJSON_PrintUnformatted(o) : NULL;
   if (!text || (int)strlen(text) >= out_cap)
   {
      if (text)
         free(text);
      cJSON_Delete(o);
      return json_error(out_buf, out_cap, 500, "response too large");
   }
   snprintf(out_buf, (size_t)out_cap, "%s", text);
   free(text);
   cJSON_Delete(o);
   return status;
}

/* GET /v1/identity/auth-mode — which login mode this kb offers. Deliberately
 * says nothing else: not the issuer, not the client id, not whether a particular
 * user exists. An unauthenticated caller learns only which flow to start, which
 * is exactly what it needs and nothing that helps it attack either flow. */
static int get_auth_mode(char *out_buf, int out_cap)
{
   /* OIDC when it is configured, PAM otherwise. That is the whole rule (§3's
    * "two mutually-exclusive modes per kb"), and PAM needs no probe of its own:
    * the PAM login is smoothgui/auth, already in production and shared with
    * SmoothNAS, so it is simply what a kb does when no issuer is set.
    *
    * A configured-but-broken OIDC profile still falls through to PAM — a working
    * fallback beats reporting a mode nobody can use — but it gets a log line,
    * because an operator who typo'd the issuer would otherwise see "pam" and have
    * nowhere to look. */
   kb_oidc_login_config_t cfg;
   kb_oidc_login_result_t rc = kb_oidc_login_config_from_env(&cfg);
   if (rc == KB_OIDC_LOGIN_INVALID)
      LOG_WARN("kb.oidc.login",
               "an OIDC login profile is configured but unusable; falling back to pam");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "mode", rc == KB_OIDC_LOGIN_OK ? "oidc" : "pam");
   return json_body(out_buf, out_cap, 200, o);
}

/* POST /v1/identity/login/start — begin an authorization-code login for a write
 * token on the named aimee-server.
 *
 * Returns {authorize_url, redirect_uri}, mirroring the server's existing
 * git_oauth_github_web_start pair; the caller navigates. The state, nonce and
 * PKCE verifier are retained server-side and NONE of them is returned: the state
 * and nonce travel via the IdP, and the verifier must never leave this process.
 */
static int post_login_start(const char *body, int64_t now, char *out_buf, int out_cap)
{
   kb_oidc_login_config_t cfg;
   if (kb_oidc_login_config_from_env(&cfg) != KB_OIDC_LOGIN_OK)
      /* Both "not configured" and "configured but broken" answer the same way.
       * An unauthenticated caller must not be able to tell a kb that has no OIDC
       * login from one whose profile is misconfigured. */
      return json_error(out_buf, out_cap, 503, "oidc login is not available");

   cJSON *request = body && body[0] ? cJSON_Parse(body) : NULL;
   const cJSON *server = request ? cJSON_GetObjectItemCaseSensitive(request, "server_id") : NULL;
   char server_id[KB_OIDC_LOGIN_SERVER_MAX + 1] = "";
   if (!cJSON_IsString(server) || !server->valuestring || !server->valuestring[0] ||
       strlen(server->valuestring) > KB_OIDC_LOGIN_SERVER_MAX)
   {
      cJSON_Delete(request);
      return json_error(out_buf, out_cap, 400, "server_id is required");
   }
   snprintf(server_id, sizeof(server_id), "%s", server->valuestring);
   cJSON_Delete(request);

   kb_oidc_login_pending_t pending;
   char url[KB_OIDC_LOGIN_URL_MAX];
   kb_oidc_login_result_t rc = kb_oidc_login_start(&cfg, server_id, &pending, url, sizeof(url));
   if (rc == KB_OIDC_LOGIN_INVALID)
      /* The profile was already checked, so this is the server_id failing the
       * grammar the identity tables CHECK. */
      return json_error(out_buf, out_cap, 400, "server_id is not a valid identifier");
   if (rc != KB_OIDC_LOGIN_OK)
   {
      LOG_WARN("kb.oidc.login", "could not start a login (rc=%d)", (int)rc);
      return json_error(out_buf, out_cap, 503, "oidc login is not available");
   }

   kb_oidc_login_store_result_t stored = kb_oidc_login_store_put(&pending, now, 0);
   if (stored != KB_OIDC_LOGIN_STORE_OK)
   {
      /* Nothing is retained, so the URL must not be handed out: a login whose
       * state cannot be looked up would fail at the callback with a far more
       * confusing error than this one. */
      kb_oidc_login_pending_clear(&pending);
      if (stored == KB_OIDC_LOGIN_STORE_FULL)
         return json_error(out_buf, out_cap, 503, "too many logins in progress; retry shortly");
      return json_error(out_buf, out_cap, 500, "could not start a login");
   }

   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "authorize_url", url);
   /* Echoed so the caller can see where the browser will land, which is the
    * value it must have registered with the IdP. */
   cJSON_AddStringToObject(o, "redirect_uri", cfg.redirect_uri);
   /* The pending record is now the store's; this copy has served its purpose. */
   kb_oidc_login_pending_clear(&pending);
   return json_body(out_buf, out_cap, 200, o);
}

/* Where the OIDC client secret lives. Not in kb's environment: the login profile
 * (kb_oidc_login_config_from_env) deliberately excludes it, so a crash dump or a
 * `ps` cannot reach it. Read at the moment of the exchange and cleansed after. */
#define OIDC_LOGIN_VAULT_AGENT "oidc"
#define OIDC_LOGIN_SECRET_CRED "oidc_login_client_secret"

/* GET /v1/identity/login/callback?code=&state= — finish the authorization-code
 * login the IdP is redirecting back from.
 *
 * THE ORDER OF THE CHECKS BELOW IS THE SECURITY OF THIS ROUTE, so each step says
 * what it would cost to move it:
 *
 *   1. parse            — a malformed or duplicated parameter never reaches the
 *                         store's lookup.
 *   2. take by state    — SINGLE USE. Removing the pending login here means a
 *                         replayed callback (from browser history, a proxy log,
 *                         a referrer header) finds nothing, even with the right
 *                         state. Taking it later would leave a replay window.
 *   3. check_state      — constant-time, over the fixed length. The take already
 *                         matched, so this is belt-and-braces against the store
 *                         ever gaining a faster lookup that leaked timing.
 *   4. exchange         — the code goes to the IdP with the RETAINED verifier and
 *                         the RETAINED redirect_uri. A stolen code is useless
 *                         without the verifier, which never left this process.
 *   5. verify signature — against the registered JWKS, audience pinned to the
 *                         login client_id. Nothing in the token is believed
 *                         before this line.
 *   6. check_nonce      — AFTER verification, because on an unverified token the
 *                         nonce claim is attacker-chosen. This is what stops an
 *                         id_token minted for another login being replayed here.
 *   7. principal        — the identity key is issuer-scoped from the CONFIGURED
 *                         issuer, so a token cannot nominate its own namespace.
 *
 * Every failure answers with the SAME generic message and status. The distinctions
 * are real and are logged, but reporting them would tell an unauthenticated
 * caller whether a state existed, whether a code was accepted by the IdP, and
 * whether a signature or a nonce was the thing that failed — an oracle for
 * exactly the attacks steps 2-6 exist to stop. */
static int get_login_callback(const char *query_string, int64_t now, char *out_buf, int out_cap)
{
   kb_oidc_login_config_t cfg;
   if (kb_oidc_login_config_from_env(&cfg) != KB_OIDC_LOGIN_OK)
      return json_error(out_buf, out_cap, 503, "oidc login is not available");

   kb_oidc_login_callback_t cb;
   kb_oidc_login_result_t rc = kb_oidc_login_callback_parse(query_string, &cb);
   if (rc == KB_OIDC_LOGIN_IDP_ERROR)
   {
      /* The IdP's own refusal is reported as such, and its error code is safe to
       * log: it is one of RFC 6749's fixed keywords, and the parser already
       * refused anything with a control byte in it. An operator seeing this
       * should look at their IdP, not at kb. */
      LOG_INFO("kb.oidc.login", "the identity provider refused a login: %s", cb.idp_error);
      kb_oidc_login_callback_clear(&cb);
      return json_error(out_buf, out_cap, 401, "the identity provider refused the login");
   }
   if (rc != KB_OIDC_LOGIN_OK)
   {
      kb_oidc_login_callback_clear(&cb);
      return json_error(out_buf, out_cap, 400, "invalid callback");
   }

   /* SINGLE USE from here on: the pending login no longer exists, so every path
    * below — including each failure — has already consumed it. */
   kb_oidc_login_pending_t pending;
   kb_oidc_login_store_result_t taken = kb_oidc_login_store_take(cb.state, now, &pending);
   if (taken != KB_OIDC_LOGIN_STORE_OK ||
       kb_oidc_login_check_state(&pending, cb.state) != KB_OIDC_LOGIN_OK)
   {
      LOG_WARN("kb.oidc.login", "a callback matched no pending login (store rc=%d)", (int)taken);
      kb_oidc_login_pending_clear(&pending);
      kb_oidc_login_callback_clear(&cb);
      return json_error(out_buf, out_cap, 400, "invalid callback");
   }

   char secret[512] = "";
   if (vault_service_get_server_principal(OIDC_LOGIN_VAULT_AGENT, OIDC_LOGIN_SECRET_CRED, secret,
                                          sizeof(secret)) != VAULT_OK ||
       !secret[0])
   {
      /* Distinct from the generic failure on purpose: this one is a deployment
       * mistake, not an attack, and it is not an oracle — anyone can already see
       * from /v1/identity/auth-mode that this kb offers OIDC. */
      OPENSSL_cleanse(secret, sizeof(secret));
      kb_oidc_login_pending_clear(&pending);
      kb_oidc_login_callback_clear(&cb);
      LOG_ERROR("kb.oidc.login", "no OIDC client secret in the vault (%s/%s)",
                OIDC_LOGIN_VAULT_AGENT, OIDC_LOGIN_SECRET_CRED);
      return json_error(out_buf, out_cap, 503, "oidc login is not fully configured");
   }

   char unverified_id_token[KB_OIDC_TOKEN_EXCHANGE_JWT_MAX] = "";
   kb_oidc_token_exchange_result_t xrc = kb_oidc_token_exchange_post(
       &cfg, &pending, cb.code, secret, unverified_id_token, sizeof(unverified_id_token));
   OPENSSL_cleanse(secret, sizeof(secret));
   kb_oidc_login_callback_clear(&cb); /* the code has been spent */

   kb_principal_t principal;
   memset(&principal, 0, sizeof(principal));
   int ok = 0;
   if (xrc != KB_OIDC_TOKEN_EXCHANGE_OK)
   {
      LOG_WARN("kb.oidc.login", "the token exchange failed (rc=%d)", (int)xrc);
   }
   else
   {
      /* NOTHING in the token is believed until this returns 1. The audience is
       * the login client_id, which is what an id_token's "aud" must be. */
      kb_verify_result_t verified;
      memset(&verified, 0, sizeof(verified));
      if (!kb_oidc_verify_id_token(unverified_id_token, cfg.client_id, (long)now, &verified))
         LOG_WARN("kb.oidc.login", "an id_token failed verification");
      else if (kb_oidc_login_check_nonce(&pending, unverified_id_token) != KB_OIDC_LOGIN_OK)
         /* A verified token that belongs to a DIFFERENT login. The signature was
          * genuine, which is exactly why the nonce check cannot be skipped. */
         LOG_WARN("kb.oidc.login", "an id_token's nonce did not match this login");
      else if (kb_oidc_login_principal(&cfg, &verified, &principal) != KB_OIDC_LOGIN_OK)
         LOG_WARN("kb.oidc.login", "a verified id_token yielded no usable principal");
      else
         ok = 1;
      OPENSSL_cleanse(&verified, sizeof(verified));
   }
   OPENSSL_cleanse(unverified_id_token, sizeof(unverified_id_token));

   char server_id[KB_OIDC_LOGIN_SERVER_MAX + 1] = "";
   snprintf(server_id, sizeof(server_id), "%s", pending.target_server_id);
   kb_oidc_login_pending_clear(&pending);

   if (!ok)
      return json_error(out_buf, out_cap, 401, "the login could not be completed");

   /* The canonical identity key, derived rather than read off a field: it is what
    * the grant and the intent are keyed by, and kb_identity_key refuses an
    * unauthenticated principal, so this is also the last check that step 7
    * actually produced one. */
   /* 576 is the width of the identity tables' subject column
    * (DB2_IDENTITY_SUBJECT_MAX), matching kb_vault_key_use.c. Not the db2 header,
    * because this unit has no other reason to depend on it. */
   char subject[576] = "";
   if (kb_identity_key(&principal, subject, sizeof(subject)) != 0)
   {
      OPENSSL_cleanse(&principal, sizeof(principal));
      return json_error(out_buf, out_cap, 401, "the login could not be completed");
   }
   OPENSSL_cleanse(&principal, sizeof(principal));

   LOG_INFO("kb.oidc.login", "authenticated %s for a write token on %s", subject, server_id);
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "subject", subject);
   /* Echoed from the PENDING login, never from the callback's query: taking it
    * from the query would let a forged callback point a completed login at a
    * different server. */
   cJSON_AddStringToObject(o, "server_id", server_id);
   return json_body(out_buf, out_cap, 200, o);
}

/* POST /v1/identity/login/pam — the other login mode.
 *
 * PAM IS DISABLED WHENEVER OIDC IS CONFIGURED, and that is enforced here rather
 * than merely declared by /v1/identity/auth-mode. The two modes are mutually
 * exclusive per kb: a kb with an IdP must not also accept host passwords, or the
 * IdP's policy (MFA, lockout, disabled accounts) is bypassable by anyone with a
 * local account. A caller that ignores auth-mode and posts here anyway is refused.
 *
 * The credential check is pam_check_credentials — the SAME function the dashboard
 * and SmoothNAS use, not a second PAM stack. There is no kb-specific PAM policy:
 * the "aimee" PAM service decides, and this route only asks.
 *
 * The subject is the BARE USERNAME. There is no `pam:` prefix, because a prefix
 * would only be meaningful if a host account and some other kind of account could
 * both be called alice on the same kb — and with the modes mutually exclusive
 * they cannot. It is validated against the same grammar the identity tables'
 * subject CHECK enforces, so an unusable subject is refused before it could reach
 * the database, and `owner` is refused because the schema reserves it.
 *
 * Rate limiting is NOT here. This route is a password oracle by nature and wants
 * throttling, but kb's rate limiter is applied by the bearer-gated path and this
 * route is pre-auth by necessity. Left as its own change rather than a partial
 * one, and called out so it is not mistaken for handled. */
static int post_login_pam(const char *body, char *out_buf, int out_cap)
{
   kb_oidc_login_config_t oidc;
   kb_oidc_login_result_t orc = kb_oidc_login_config_from_env(&oidc);
   if (orc == KB_OIDC_LOGIN_OK)
      return json_error(out_buf, out_cap, 409, "this kb uses oidc; password login is disabled");
   /* A configured-but-BROKEN OIDC profile falls through to PAM, matching what
    * auth-mode reports. Reporting one mode and enforcing another would leave a kb
    * with a typo'd issuer unable to log anybody in at all. */
   if (orc == KB_OIDC_LOGIN_INVALID)
      LOG_WARN("kb.pam.login",
               "an OIDC login profile is configured but unusable; serving pam login");

   cJSON *request = body && body[0] ? cJSON_Parse(body) : NULL;
   const cJSON *juser = request ? cJSON_GetObjectItemCaseSensitive(request, "username") : NULL;
   const cJSON *jpass = request ? cJSON_GetObjectItemCaseSensitive(request, "password") : NULL;
   const cJSON *jsrv = request ? cJSON_GetObjectItemCaseSensitive(request, "server_id") : NULL;
   if (!cJSON_IsString(juser) || !juser->valuestring || !cJSON_IsString(jpass) ||
       !jpass->valuestring || !jpass->valuestring[0] || !cJSON_IsString(jsrv) ||
       !jsrv->valuestring || !jsrv->valuestring[0])
   {
      cJSON_Delete(request);
      return json_error(out_buf, out_cap, 400, "username, password and server_id are required");
   }

   char username[64] = "", server_id[KB_OIDC_LOGIN_SERVER_MAX + 1] = "";
   char password[512] = "";
   int fits = strlen(juser->valuestring) < sizeof(username) &&
              strlen(jpass->valuestring) < sizeof(password) &&
              strlen(jsrv->valuestring) <= KB_OIDC_LOGIN_SERVER_MAX;
   if (fits)
   {
      snprintf(username, sizeof(username), "%s", juser->valuestring);
      snprintf(password, sizeof(password), "%s", jpass->valuestring);
      snprintf(server_id, sizeof(server_id), "%s", jsrv->valuestring);
   }
   /* Cleanse the parsed request before the check, not after: cJSON holds the
    * password in a heap buffer that cJSON_Delete frees without clearing. */
   if (cJSON_IsString(jpass) && jpass->valuestring)
      OPENSSL_cleanse(jpass->valuestring, strlen(jpass->valuestring));
   cJSON_Delete(request);

   /* The subject grammar and the reserved name are checked BEFORE PAM, so an
    * unusable username never reaches the host's authentication stack — and so a
    * refusal here costs no PAM round trip that could be timed. */
   int usable = fits && db2_intent_bare_username(username) && strcmp(username, "owner") != 0;
   int ok = usable && pam_check_credentials(username, password);
   OPENSSL_cleanse(password, sizeof(password));

   if (!ok)
   {
      /* ONE ANSWER for a bad password, an unknown account, a locked account, a
       * reserved name and a username outside the grammar. Any distinction here is
       * an account-enumeration oracle on a pre-auth route. */
      LOG_WARN("kb.pam.login", "a password login was refused (usable_subject=%d)", usable);
      return json_error(out_buf, out_cap, 401, "authentication failed");
   }

   LOG_INFO("kb.pam.login", "authenticated %s for a write token on %s", username, server_id);
   cJSON *o = cJSON_CreateObject();
   /* The bare username IS the subject the intent writer records. */
   cJSON_AddStringToObject(o, "subject", username);
   cJSON_AddStringToObject(o, "server_id", server_id);
   return json_body(out_buf, out_cap, 200, o);
}

int kb_http_identity_login_route(const char *method, const char *path, const char *query_string,
                                 const char *body, int64_t now, char *out_buf, int out_cap)
{
   if (!method || !path || !out_buf || out_cap <= 0)
      return -1;

   if (strcmp(path, "/v1/identity/auth-mode") == 0)
   {
      if (strcmp(method, "GET") != 0)
         return json_error(out_buf, out_cap, 405, "method not allowed");
      return get_auth_mode(out_buf, out_cap);
   }
   if (strcmp(path, "/v1/identity/login/start") == 0)
   {
      /* POST rather than GET even though it reads like one: starting a login
       * MUTATES server state (it consumes a slot in the pending store), and a
       * GET would be prefetchable by a browser or a link scanner. */
      if (strcmp(method, "POST") != 0)
         return json_error(out_buf, out_cap, 405, "method not allowed");
      return post_login_start(body, now, out_buf, out_cap);
   }
   if (strcmp(path, "/v1/identity/login/pam") == 0)
   {
      if (strcmp(method, "POST") != 0)
         return json_error(out_buf, out_cap, 405, "method not allowed");
      return post_login_pam(body, out_buf, out_cap);
   }
   if (strcmp(path, "/v1/identity/login/callback") == 0)
   {
      /* GET, because this is where a BROWSER lands: the IdP issues a redirect and
       * a browser follows it with GET. The route mutates state (it consumes the
       * pending login) which would normally argue for POST, but the method is not
       * ours to choose. */
      if (strcmp(method, "GET") != 0)
         return json_error(out_buf, out_cap, 405, "method not allowed");
      return get_login_callback(query_string, now, out_buf, out_cap);
   }
   return -1;
}
