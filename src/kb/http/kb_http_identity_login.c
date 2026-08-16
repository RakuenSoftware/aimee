/* kb_http_identity_login.c — see kb_http_identity_login.h. */

#include "kb_http_identity_login.h"

#include "cJSON.h"
#include "pam_auth.h" /* pam_check_credentials — the one PAM policy, shared with the dashboard */
#include "modules/db2/c/management_identity_journal.h"
#include "modules/db2/c/management_intent_fields.h" /* db2_intent_bare_username (header-only) */
#include "kb_auth_oidc.h"
#include "kb/kb_login_throttle.h" /* the pre-auth brute-force budget */
#include "kb_oidc_login.h"
#include "kb_oidc_login_store.h"
#include "kb_oidc_token_exchange.h"
#include "kb_reqctx.h" /* the request's Content-Type — CSRF surface on the PAM login */
#include "log.h"
#include "vault_service.h"

#include <openssl/crypto.h> /* OPENSSL_cleanse */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* A refusal that tells the caller how long to wait.
 *
 * The wait is a BODY FIELD, not a Retry-After header, and that is a scope
 * boundary rather than an oversight: every kb route returns (status, json body)
 * and nothing in this layer can set a response header. Adding one would mean
 * changing the signature every kb route is written against. The field carries the
 * same information to any client that reads it; a browser's automatic
 * Retry-After handling is not relevant to a JSON API. Called out so a reviewer
 * can overrule it cheaply. */
static int json_error_retry_after(char *out_buf, int out_cap, int status, const char *message,
                                  int retry_after)
{
   cJSON *o = cJSON_CreateObject();
   if (o)
   {
      cJSON_AddStringToObject(o, "error", message);
      cJSON_AddNumberToObject(o, "retry_after", retry_after);
   }
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

/* Read a JSON team_id, refusing anything that is not an exactly representable
 * positive integer.
 *
 * cJSON stores every number as a double, so `770001.9` passes a bare
 * "positive and in range" check and then becomes 770001 on the cast. That is not a
 * cosmetic sloppiness: team_id is the authorization scope — it selects the
 * FORCE RLS-bound grant lookup — so a silent truncation authorizes against a team
 * the caller did not name. Anything not exactly an integer is refused rather than
 * rounded, and the round-trip comparison also catches a value too large to survive
 * the cast. Returns 0 on success. */
static int json_team_id(const cJSON *v, int64_t *out)
{
   *out = 0;
   if (!cJSON_IsNumber(v))
      return -1;
   double d = v->valuedouble;
   /* Below 2^53 every integer is exactly representable, so the round trip below is
    * decisive; at or above it the comparison can succeed for a value that is not
    * the one written, so refuse outright. No real team id is anywhere near this. */
   if (!(d >= 1.0) || d >= 9007199254740992.0)
      return -1;
   int64_t n = (int64_t)d;
   if ((double)n != d)
      return -1; /* fractional */
   *out = n;
   return 0;
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
   const cJSON *team = request ? cJSON_GetObjectItemCaseSensitive(request, "team_id") : NULL;
   char server_id[KB_OIDC_LOGIN_SERVER_MAX + 1] = "";
   /* team_id is required at START, not at the callback: it has to be retained
    * alongside the other per-login state so the callback cannot name its own. */
   int64_t team_id = 0;
   if (!cJSON_IsString(server) || !server->valuestring || !server->valuestring[0] ||
       strlen(server->valuestring) > KB_OIDC_LOGIN_SERVER_MAX || json_team_id(team, &team_id) != 0)
   {
      cJSON_Delete(request);
      return json_error(out_buf, out_cap, 400, "server_id and an integer team_id are required");
   }
   snprintf(server_id, sizeof(server_id), "%s", server->valuestring);
   cJSON_Delete(request);

   kb_oidc_login_pending_t pending;
   char url[KB_OIDC_LOGIN_URL_MAX];
   kb_oidc_login_result_t rc =
       kb_oidc_login_start(&cfg, server_id, team_id, &pending, url, sizeof(url));
   if (rc == KB_OIDC_LOGIN_INVALID)
      /* The profile was already checked, so this is the server_id failing the
       * grammar the identity tables CHECK, or a non-positive team. */
      return json_error(out_buf, out_cap, 400, "server_id or team_id is not valid");
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

/* The issuer recorded on every intent this kb files. A constant, not configuration:
 * it names the AUTHORITY that will sign the token (this kb), which is not something
 * a deployment or a caller gets to vary — the mint's own checks bind the token root
 * and publication to it. Distinct from the OIDC issuer, which names who
 * authenticated the user. */
#define IDENTITY_INTENT_TOKEN_ISSUER "kb"

/* How long a minted write token lives. Short by policy: a token is consumed on
 * first use anyway, so this only bounds how long an unused one stays mintable.
 * DB2_IDENTITY_TTL_MAX_SECONDS is the ceiling the schema enforces. */
#define IDENTITY_INTENT_TTL_SECONDS 300

/* File the identity intent for an authenticated principal and render the result.
 *
 * Shared by both login modes deliberately. The two routes differ entirely in how
 * they authenticate and not at all in what they do afterwards, and the moment that
 * stops being true one mode acquires an authorization step the other lacks.
 *
 * kid and installation_id are READ here, never taken from the caller — see
 * db2_identity_login_context. Failures are mapped to the same generic answer the
 * calling route uses for an authentication failure, because "you are who you say
 * but have no grant on that server" is not something to spell out to a caller that
 * has just proved only its own identity. */
static int file_intent(const kb_principal_t *principal, const char *subject, int64_t team_id,
                       const char *server_id, db2_identity_auth_mode_t mode, const char *log_domain,
                       char *out_buf, int out_cap)
{
   char installation_id[33] = "", kid[DB2_IDENTITY_KID_MAX + 1] = "";
   db2_management_action_result_t rc =
       db2_identity_login_context(principal, team_id, installation_id, kid);
   if (rc != DB2_MANAGEMENT_ACTION_OK)
   {
      LOG_WARN(log_domain, "no identity login context for team %lld (rc=%d)", (long long)team_id,
               (int)rc);
      return json_error(out_buf, out_cap, rc == DB2_MANAGEMENT_ACTION_DENIED ? 403 : 503,
                        rc == DB2_MANAGEMENT_ACTION_DENIED
                            ? "not a member of that team"
                            : "this kb cannot issue write tokens right now");
   }

   db2_identity_intent_operation_t op;
   rc = db2_identity_intent_operation_init(team_id, server_id, mode, IDENTITY_INTENT_TOKEN_ISSUER,
                                           kid, IDENTITY_INTENT_TTL_SECONDS, installation_id, &op);
   if (rc != DB2_MANAGEMENT_ACTION_OK)
   {
      LOG_ERROR(log_domain, "could not prepare an identity intent (rc=%d)", (int)rc);
      return json_error(out_buf, out_cap, 503, "this kb cannot issue write tokens right now");
   }

   db2_identity_intent_t intent;
   rc = db2_identity_intent_start(principal, &op, &intent);
   if (rc != DB2_MANAGEMENT_ACTION_OK)
   {
      /* DENIED is the common and expected case: authenticated, but with no live
       * write-tier grant on that server. It is reported as its own 403 rather than
       * folded into the generic auth failure — the caller has already proved who it
       * is, so telling it that it lacks a grant reveals nothing it could not learn
       * by asking an operator, and hiding it would make the flow undebuggable. */
      LOG_WARN(log_domain, "identity intent refused for %s on %s (rc=%d)", subject, server_id,
               (int)rc);
      return json_error(out_buf, out_cap, rc == DB2_MANAGEMENT_ACTION_DENIED ? 403 : 503,
                        rc == DB2_MANAGEMENT_ACTION_DENIED
                            ? "no write-tier grant for that subject on that server"
                            : "this kb cannot issue write tokens right now");
   }

   LOG_INFO(log_domain, "identity intent filed for %s on %s (team %lld, replayed=%d)",
            intent.subject, intent.target_server_id, (long long)intent.team_id, intent.replayed);
   cJSON *o = cJSON_CreateObject();
   /* The SUBJECT the DATABASE resolved, not the one this route computed. They must
    * agree — the SQL takes it from aimee.principal — and returning the recorded one
    * means the caller sees exactly what the mint will act on. */
   cJSON_AddStringToObject(o, "subject", intent.subject);
   cJSON_AddStringToObject(o, "server_id", intent.target_server_id);
   cJSON_AddNumberToObject(o, "team_id", (double)intent.team_id);
   /* The pair the token authority mints from. Not secret — they authorize nothing
    * on their own, and the mint re-reads every precondition — but they are what the
    * caller presents to collect its token. */
   cJSON_AddStringToObject(o, "correlation_id", intent.correlation_id);
   cJSON_AddStringToObject(o, "jti", intent.jti);
   cJSON_AddNumberToObject(o, "expires_at", (double)intent.expires_at);
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
   if (rc != KB_OIDC_LOGIN_OK && rc != KB_OIDC_LOGIN_IDP_ERROR)
   {
      kb_oidc_login_callback_clear(&cb);
      return json_error(out_buf, out_cap, 400, "invalid callback");
   }

   /* SINGLE USE, AND ON THE ERROR PATH TOO. The state lookup happens before the
    * branch on rc, so a genuine IdP refusal consumes the pending login it belongs
    * to instead of leaving it live with a valid state until its TTL — and an
    * UNSOLICITED ?error= cannot obtain the distinct "the identity provider
    * refused" answer without first proving it belongs to a login this kb started.
    * RFC 6749 §4.1.2.1 makes state REQUIRED in the error response, so there is no
    * legitimate error callback this excludes. */
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

   if (rc == KB_OIDC_LOGIN_IDP_ERROR)
   {
      /* Now that the login is accounted for, the IdP's own refusal is reported as
       * such. Its error code is safe to log: it is one of RFC 6749's fixed
       * keywords, and the parser refused anything carrying a control byte. An
       * operator seeing this should look at their IdP, not at kb. */
      LOG_INFO("kb.oidc.login", "the identity provider refused a login: %s", cb.idp_error);
      kb_oidc_login_pending_clear(&pending);
      kb_oidc_login_callback_clear(&cb);
      return json_error(out_buf, out_cap, 401, "the identity provider refused the login");
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
   int64_t team_id = pending.team_id;
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
   /* The intent writer needs the PRINCIPAL, not the derived key: db2_tenant_scope
    * sets aimee.principal from it, and the SQL reads the subject from there. Kept
    * as its own copy so the ordering below stays explicit about when it dies. */
   kb_principal_t principal_copy = principal;
   OPENSSL_cleanse(&principal, sizeof(principal));

   LOG_INFO("kb.oidc.login", "authenticated %s for a write token on %s", subject, server_id);
   /* The server_id and team_id come from the PENDING login, never the callback's
    * query: taking either from the query would let a forged callback point a
    * completed login at a different server or team. */
   int status = file_intent(&principal_copy, subject, team_id, server_id,
                            DB2_IDENTITY_AUTH_MODE_OIDC, "kb.oidc.login", out_buf, out_cap);
   OPENSSL_cleanse(&principal_copy, sizeof(principal_copy));
   return status;
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
 * RATE LIMITING IS HERE, via kb_login_throttle. It has to be: this route is a
 * password oracle by nature, it is reachable with no bearer (that is what a login
 * surface is), and kb's other rate limiting lives on the bearer-gated path that
 * this route by necessity sits in front of. Measured before it existed, twelve
 * wrong-password attempts in a row each returned an immediate 401.
 *
 * THE ORDER MATTERS. The throttle is consulted BEFORE the grammar check and
 * before PAM, and a refusal is charged for EVERY credential rejection —
 * wrong password, unknown account, the reserved name, an ungrammatical username
 * alike. Charging only the "real" failures would make the throttle itself an
 * account-enumeration oracle: a caller could tell a real account from an unknown
 * one by which attempts started getting 429s. A successful credential check
 * clears the budget, and a caller that authenticates but is then refused for want
 * of a grant is NOT charged — it proved who it is. */
static int post_login_pam(const char *body, char *out_buf, int out_cap)
{
   /* CSRF: a browser can send exactly text/plain, application/x-www-form-urlencoded
    * and multipart/form-data cross-origin without a preflight. Accepting a JSON
    * body under any of them made this route drivable from a form on an attacker's
    * page — measured, not assumed: all three reached credential processing and
    * returned 401. Requiring application/json means a cross-origin form cannot
    * reach the credential check at all, because the one content type this route
    * accepts is the one that forces a preflight the attacker's origin will fail.
    *
    * BEFORE the throttle deliberately: a request that never reaches the
    * credential check must not consume another caller's login budget, or a
    * forged form becomes a denial-of-service against the real user it names.
    * Nothing in the tree posts here with another content type. */
   if (!kb_reqctx_content_type_is_json())
      return json_error(out_buf, out_cap, 415, "content-type must be application/json");

   kb_oidc_login_config_t oidc;
   kb_oidc_login_result_t orc = kb_oidc_login_config_from_env(&oidc);
   if (orc == KB_OIDC_LOGIN_OK)
      return json_error(out_buf, out_cap, 409, "this kb uses oidc; password login is disabled");
   /* A configured-but-BROKEN OIDC profile falls through to PAM, matching what
    * auth-mode reports. Reporting one mode and enforcing another would leave a kb
    * with a typo'd issuer unable to log anybody in at all. */
   if (orc == KB_OIDC_LOGIN_INVALID)
      /* Its own log domain so an operator can alert on precisely this: a kb that
       * was meant to be on OIDC is serving passwords. "kb.pam.login" alone would
       * be indistinguishable from a kb intentionally in PAM mode.
       *
       * It stays a FALLBACK rather than requiring an explicit opt-in flag, which
       * was considered. Opt-in would mean one typo'd URL locks every user out of a
       * working kb, and it buys nothing against the threat it looks like it
       * addresses: an attacker who can invalidate the OIDC profile can equally
       * delete it, which reaches PAM either way. The defence against downgrade is
       * that this is loud, not that it is impossible. */
      LOG_WARN("kb.pam.login.fallback",
               "an OIDC login profile is configured but UNUSABLE; this kb is serving "
               "password login instead of oidc — fix the profile or remove it");

   cJSON *request = body && body[0] ? cJSON_Parse(body) : NULL;
   const cJSON *juser = request ? cJSON_GetObjectItemCaseSensitive(request, "username") : NULL;
   const cJSON *jpass = request ? cJSON_GetObjectItemCaseSensitive(request, "password") : NULL;
   const cJSON *jsrv = request ? cJSON_GetObjectItemCaseSensitive(request, "server_id") : NULL;
   const cJSON *jteam = request ? cJSON_GetObjectItemCaseSensitive(request, "team_id") : NULL;
   int64_t team_id = 0;
   if (!cJSON_IsString(juser) || !juser->valuestring || !cJSON_IsString(jpass) ||
       !jpass->valuestring || !jpass->valuestring[0] || !cJSON_IsString(jsrv) ||
       !jsrv->valuestring || !jsrv->valuestring[0] || json_team_id(jteam, &team_id) != 0)
   {
      /* The password is cleansed on THIS path too — a malformed request still
       * carried one, and cJSON_Delete frees the buffer without clearing it. */
      if (cJSON_IsString(jpass) && jpass->valuestring)
         OPENSSL_cleanse(jpass->valuestring, strlen(jpass->valuestring));
      cJSON_Delete(request);
      return json_error(out_buf, out_cap, 400,
                        "username, password, server_id and an integer team_id are required");
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

   /* Throttle BEFORE any credential work: a refused attempt must cost the caller
    * a round trip and nothing else — no PAM call, no /etc/shadow read. The answer
    * is 429 with Retry-After, and it is the same answer whatever the username is,
    * so the throttle cannot be probed to tell real accounts from invented ones. */
   int64_t now = (int64_t)time(NULL);
   int retry_after = kb_login_throttle_check(username, now);
   if (retry_after > 0)
   {
      OPENSSL_cleanse(password, sizeof(password));
      LOG_WARN("kb.pam.login", "a password login was throttled (retry_after=%d)", retry_after);
      return json_error_retry_after(out_buf, out_cap, 429, "too many login attempts", retry_after);
   }

   /* The subject grammar and the reserved name are checked BEFORE PAM, so an
    * unusable username never reaches the host's authentication stack — and so a
    * refusal here costs no PAM round trip that could be timed. */
   int usable = fits && db2_intent_bare_username(username) && strcmp(username, "owner") != 0;
   int ok = usable && pam_check_credentials(username, password);
   OPENSSL_cleanse(password, sizeof(password));

   if (ok)
      kb_login_throttle_record_success(username);
   else
      kb_login_throttle_record_failure(username, now);

   if (!ok)
   {
      /* ONE ANSWER for a bad password, an unknown account, a locked account, a
       * reserved name and a username outside the grammar. Any distinction here is
       * an account-enumeration oracle on a pre-auth route. */
      LOG_WARN("kb.pam.login", "a password login was refused (usable_subject=%d)", usable);
      return json_error(out_buf, out_cap, 401, "authentication failed");
   }

   LOG_INFO("kb.pam.login", "authenticated %s for a write token on %s", username, server_id);
   /* A host-account principal: the bare username IS the subject, so the identity
    * key and the subject are the same string. kb_principal_from_host_account is
    * what marks it authenticated — a zero-initialised principal is refused by every
    * tenant-scoped entry, which is exactly the protection wanted here. */
   kb_principal_t principal;
   if (kb_principal_from_host_account(username, &principal) != 0)
   {
      LOG_ERROR("kb.pam.login", "authenticated %s but could not build a principal", username);
      return json_error(out_buf, out_cap, 401, "authentication failed");
   }
   int status = file_intent(&principal, username, team_id, server_id, DB2_IDENTITY_AUTH_MODE_PAM,
                            "kb.pam.login", out_buf, out_cap);
   OPENSSL_cleanse(&principal, sizeof(principal));
   return status;
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
