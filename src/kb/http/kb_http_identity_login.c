/* kb_http_identity_login.c — see kb_http_identity_login.h. */

#include "kb_http_identity_login.h"

#include "cJSON.h"
#include "kb_oidc_login.h"
#include "kb_oidc_login_store.h"
#include "log.h"

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

int kb_http_identity_login_route(const char *method, const char *path, const char *body,
                                 int64_t now, char *out_buf, int out_cap)
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
   return -1;
}
