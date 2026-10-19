/* oauth_tokens.c: OAuth token storage, exchange, and refresh.
 *
 * Implements the token-layer of the PKCE flow:
 *   - JSON parsing of RFC 6749 §5.1 token responses
 *   - Vault persistence keyed by MCP client name
 *   - Token exchange (authorization_code grant)
 *   - Token refresh (refresh_token grant)
 *   - High-level oauth_token_get() with auto-refresh
 *
 * Network: uses platform_exec_pipe with curl (same pattern as HyDE rewrite
 * and cross-encoder commands) so the build stays dependency-free.
 */
#include "aimee.h"
#include "db1_client/db1.h"
#include "oauth_flow.h"
#include "oauth_pkce.h"
#include "vault_service.h" /* server-sealed vault: encrypted-at-rest token storage */
#include "cJSON.h"
#include "log.h"
#include "platform_process.h"
#include "util.h"
#include <stdint.h>
#include <openssl/crypto.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* OAuth tokens are encrypted at rest in the server-owned vault (WP-C.4), keyed by
 * (VAULT_SERVER_PRINCIPAL, client_name, cred). The server decrypts them
 * autonomously to refresh/use them. Previously they were stored via db1/secrets,
 * which in a container falls back to a PLAINTEXT 0600 file — the leak this closes.
 * The legacy plaintext entries are migrated lazily on first read and scrubbed.
 * A migration that cannot durably seal and remove its source fails closed: the
 * server never uses a credential from the legacy backend. */
#define OAUTH_VCRED_ACCESS  "oauth_access_token"
#define OAUTH_VCRED_REFRESH "oauth_refresh_token"
#define OAUTH_VCRED_EXPIRES "oauth_expires_at"
/* Set when a refresh is rejected by the IdP (refresh token revoked/expired): the
 * server cannot recover autonomously, so the operator must re-auth (D6). Cleared
 * on the next successful token store (a fresh device-flow re-auth). */
#define OAUTH_VCRED_REAUTH  "oauth_reauth_required"
#define OAUTH_DB1FMT_REAUTH "oauth.%s.reauth_required"

static int oauth_secret_store(const char *client_name, const char *vcred, const char *value)
{
   if (vault_service_set_server(client_name, vcred, value) == VAULT_OK)
      return 0;
   aimee_log(LOG_WARN, "oauth_flow", "vault store failed for %s/%s (token not persisted)",
             client_name, vcred);
   return -1;
}

static int oauth_legacy_key(char *key, size_t key_len, const char *format, const char *client_name)
{
   int written = -1;
   if (strcmp(format, OAUTH_KEY_ACCESS_TOKEN) == 0)
      written = snprintf(key, key_len, "oauth.%s.access_token", client_name);
   else if (strcmp(format, OAUTH_KEY_REFRESH_TOKEN) == 0)
      written = snprintf(key, key_len, "oauth.%s.refresh_token", client_name);
   else if (strcmp(format, OAUTH_KEY_EXPIRES_AT) == 0)
      written = snprintf(key, key_len, "oauth.%s.expires_at", client_name);
   else if (strcmp(format, OAUTH_DB1FMT_REAUTH) == 0)
      written = snprintf(key, key_len, "oauth.%s.reauth_required", client_name);
   return written >= 0 && (size_t)written < key_len ? 0 : -1;
}

/* Load from the vault; if absent, lazily migrate a legacy db1/secrets plaintext
 * value (db1_fmt is the old "oauth.%s.*" key format) into the vault and scrub it.
 * Returns 0 with `buf` filled, or -1 on a miss (buf emptied). */
static int oauth_secret_load(const char *client_name, const char *vcred, const char *db1_fmt,
                             char *buf, size_t len)
{
   if (!buf || len == 0)
      return -1;
   if (vault_service_get_server_principal(client_name, vcred, buf, len) == VAULT_OK && buf[0])
      return 0;
   char key[256];
   if (oauth_legacy_key(key, sizeof key, db1_fmt, client_name) != 0)
      return -1;
   if (db1_secret_load(key, buf, len) == 0 && buf[0])
   {
      if (vault_service_set_server(client_name, vcred, buf) != VAULT_OK ||
          db1_secret_remove(key) != 0)
      {
         OPENSSL_cleanse(buf, len);
         return -1;
      }
      /* Re-read through the only supported authority. This also proves the
       * durable Vault record is usable before the caller receives plaintext. */
      OPENSSL_cleanse(buf, len);
      return vault_service_get_server_principal(client_name, vcred, buf, len) == VAULT_OK && buf[0]
                 ? 0
                 : -1;
   }
   buf[0] = '\0';
   return -1;
}

static void oauth_secret_remove(const char *client_name, const char *vcred, const char *db1_fmt)
{
   (void)vault_service_delete(VAULT_SERVER_PRINCIPAL, client_name, vcred);
   char key[256];
   if (oauth_legacy_key(key, sizeof key, db1_fmt, client_name) == 0)
      (void)db1_secret_remove(key); /* also clear any legacy plaintext */
}

/* ---- JSON parsing ---- */

int oauth_token_parse_response(const char *json, oauth_token_response_t *out)
{
   if (!json || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   cJSON *j = cJSON_Parse(json);
   if (!j)
      return -1;

   cJSON *at = cJSON_GetObjectItemCaseSensitive(j, "access_token");
   if (!cJSON_IsString(at) || !at->valuestring[0])
   {
      cJSON_Delete(j);
      return -1;
   }
   snprintf(out->access_token, sizeof(out->access_token), "%s", at->valuestring);

   cJSON *rt = cJSON_GetObjectItemCaseSensitive(j, "refresh_token");
   if (cJSON_IsString(rt) && rt->valuestring[0])
      snprintf(out->refresh_token, sizeof(out->refresh_token), "%s", rt->valuestring);

   cJSON *tt = cJSON_GetObjectItemCaseSensitive(j, "token_type");
   if (cJSON_IsString(tt) && tt->valuestring[0])
      snprintf(out->token_type, sizeof(out->token_type), "%s", tt->valuestring);

   cJSON *ei = cJSON_GetObjectItemCaseSensitive(j, "expires_in");
   if (cJSON_IsNumber(ei) && ei->valuedouble > 0)
   {
      out->expires_in = (long)ei->valuedouble;
      out->expires_at = (long)time(NULL) + out->expires_in;
   }

   /* Also accept pre-computed expires_at (from our own stored JSON) */
   cJSON *ea = cJSON_GetObjectItemCaseSensitive(j, "expires_at");
   if (cJSON_IsNumber(ea) && ea->valuedouble > 0)
      out->expires_at = (long)ea->valuedouble;

   cJSON_Delete(j);
   return 0;
}

/* ---- Secret-store helpers ---- */

int oauth_token_store(const char *client_name, const oauth_token_response_t *resp)
{
   if (!client_name || !resp || !resp->access_token[0])
      return -1;

   int rc = 0;

   if (oauth_secret_store(client_name, OAUTH_VCRED_ACCESS, resp->access_token) != 0)
      rc = -1;

   if (resp->refresh_token[0])
   {
      if (oauth_secret_store(client_name, OAUTH_VCRED_REFRESH, resp->refresh_token) != 0)
         rc = -1;
   }

   /* An absent expiry belongs to the new token too. Retaining the previous
    * account/token's expiry can incorrectly expire a newly imported login. */
   char ea_str[32];
   snprintf(ea_str, sizeof(ea_str), "%ld", resp->expires_at > 0 ? resp->expires_at : 0L);
   if (oauth_secret_store(client_name, OAUTH_VCRED_EXPIRES, ea_str) != 0)
      rc = -1;

   /* A usable token is now stored (fresh exchange or an operator re-auth) — clear
    * any standing "re-auth required" marker (D6). */
   if (rc == 0)
      oauth_secret_remove(client_name, OAUTH_VCRED_REAUTH, OAUTH_DB1FMT_REAUTH);

   return rc;
}

/* True if `client_name`'s OAuth credential is marked REAUTH_REQUIRED — a prior
 * refresh was rejected by the IdP and the operator must re-authenticate (D6). */
int oauth_token_reauth_required(const char *client_name)
{
   if (!client_name)
      return 0;
   char v[8] = "";
   if (oauth_secret_load(client_name, OAUTH_VCRED_REAUTH, OAUTH_DB1FMT_REAUTH, v, sizeof(v)) == 0 &&
       v[0] == '1')
      return 1;
   return 0;
}

int oauth_token_load(const char *client_name, char *buf, size_t len)
{
   if (!client_name || !buf || len == 0)
      return -1;

   if (oauth_secret_load(client_name, OAUTH_VCRED_ACCESS, OAUTH_KEY_ACCESS_TOKEN, buf, len) != 0)
      return -1;

   /* Check expiry */
   char ea_str[32] = "";
   if (oauth_secret_load(client_name, OAUTH_VCRED_EXPIRES, OAUTH_KEY_EXPIRES_AT, ea_str,
                         sizeof(ea_str)) == 0 &&
       ea_str[0])
   {
      long expires_at = atol(ea_str);
      if (expires_at > 0 && time(NULL) >= expires_at)
      {
         buf[0] = '\0';
         return -1; /* expired */
      }
   }

   return 0;
}

int oauth_token_remove(const char *client_name)
{
   if (!client_name)
      return -1;
   oauth_secret_remove(client_name, OAUTH_VCRED_ACCESS, OAUTH_KEY_ACCESS_TOKEN);
   oauth_secret_remove(client_name, OAUTH_VCRED_REFRESH, OAUTH_KEY_REFRESH_TOKEN);
   oauth_secret_remove(client_name, OAUTH_VCRED_EXPIRES, OAUTH_KEY_EXPIRES_AT);
   return 0;
}

/* ---- Token exchange via curl ---- */

/*
 * Build and run a curl command for a token endpoint POST.
 * |post_data| is the pre-encoded form or JSON body.
 * On success, |*resp_out| is a malloc'd JSON string (caller frees).
 */
static int curl_post_token(const char *token_endpoint, const char *post_data, int json_body,
                           char **resp_out, size_t *resp_len_out, int *http_status_out)
{
   if (!token_endpoint || !post_data || !resp_out)
      return -1;
   if (http_status_out)
      *http_status_out = 0;

   /* NOTE: NOT `-f`. We deliberately capture the response BODY on HTTP errors so
    * the caller can tell a rejected refresh (4xx `invalid_grant` → re-auth, D6)
    * from a transient transport/5xx error. `-w '\n%{http_code}'` appends the
    * status after the body; we split it off below. We pre-encode the body. */
   char *endpoint = shell_quote(token_endpoint);
   if (!endpoint)
      return -1;
   char cmd[4096];
   int written =
       snprintf(cmd, sizeof(cmd),
                "curl -s -X POST --connect-timeout 10 --max-time 30"
                " -H 'Content-Type: %s'"
                " -H 'Accept: application/json'"
                " --data-binary @-"
                " -w '\\n%%{http_code}'"
                " --url %s",
                json_body ? "application/json" : "application/x-www-form-urlencoded", endpoint);
   free(endpoint);
   if (written < 0 || (size_t)written >= sizeof cmd)
      return -1;

   char *raw = NULL;
   size_t raw_len = 0;
   int rc = platform_exec_pipe(cmd, post_data, strlen(post_data), &raw, &raw_len);
   if (rc != 0 || !raw)
   {
      free(raw);
      return -1; /* transport failure: no body, no status */
   }

   /* Split the trailing "\n<http_code>" appended by -w off the body. */
   char *nl = strrchr(raw, '\n');
   if (nl)
   {
      if (http_status_out)
         *http_status_out = atoi(nl + 1);
      *nl = '\0';
      raw_len = (size_t)(nl - raw);
   }
   *resp_out = raw;
   if (resp_len_out)
      *resp_len_out = raw_len;
   return 0;
}

/* Minimal percent-encoding for OAuth POST body values.
 * Encodes characters that are not unreserved per RFC 3986 §2.3. */
static char *pct_encode(const char *in)
{
   static const char *safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
   size_t length = strlen(in);
   if (length > (SIZE_MAX - 1) / 3)
      return NULL;
   char *out = malloc(length * 3 + 1);
   if (!out)
      return NULL;
   size_t pos = 0;
   for (; *in; in++)
   {
      if (strchr(safe, (unsigned char)*in))
         out[pos++] = *in;
      else
      {
         snprintf(out + pos, 4, "%%%02X", (unsigned char)*in);
         pos += 3;
      }
   }
   out[pos] = '\0';
   return out;
}

static void oauth_cleanse_free(char *value)
{
   if (value)
      OPENSSL_cleanse(value, strlen(value));
   free(value);
}

static char *oauth_refresh_body(const oauth_refresh_profile_t *profile, const char *refresh)
{
   char *body = NULL;
   if (profile->format == OAUTH_REFRESH_JSON)
   {
      cJSON *json = cJSON_CreateObject();
      if (json && cJSON_AddStringToObject(json, "grant_type", "refresh_token") &&
          cJSON_AddStringToObject(json, "refresh_token", refresh) &&
          cJSON_AddStringToObject(json, "client_id", profile->client_id) &&
          (!profile->scope || cJSON_AddStringToObject(json, "scope", profile->scope)))
         body = cJSON_PrintUnformatted(json);
      /* The temporary tree contains the refresh secret too. */
      cJSON *secret = cJSON_GetObjectItemCaseSensitive(json, "refresh_token");
      if (cJSON_IsString(secret))
         OPENSSL_cleanse(secret->valuestring, strlen(secret->valuestring));
      cJSON_Delete(json);
      return body;
   }
   char *encoded_refresh = pct_encode(refresh), *encoded_client = pct_encode(profile->client_id);
   if (encoded_refresh && encoded_client)
   {
      size_t capacity = strlen(encoded_refresh) + strlen(encoded_client) + 64;
      body = malloc(capacity);
      if (body)
         snprintf(body, capacity, "grant_type=refresh_token&refresh_token=%s&client_id=%s",
                  encoded_refresh, encoded_client);
   }
   oauth_cleanse_free(encoded_refresh);
   free(encoded_client);
   return body;
}

static int oauth_refresh_profile(const char *client_name, const oauth_refresh_profile_t *profile)
{
   if (!client_name || !profile || !profile->client_id || !profile->token_endpoint ||
       (profile->format != OAUTH_REFRESH_FORM && profile->format != OAUTH_REFRESH_JSON))
      return OAUTH_REFRESH_TRANSIENT;
   oauth_token_response_t result = {0};
   char refresh_token[sizeof result.refresh_token] = "";
   char *body = NULL, *resp = NULL;
   int rc = OAUTH_REFRESH_TRANSIENT, http_status = 0;
   if (oauth_secret_load(client_name, OAUTH_VCRED_REFRESH, OAUTH_KEY_REFRESH_TOKEN, refresh_token,
                         sizeof refresh_token) ||
       !refresh_token[0])
      goto done;
   body = oauth_refresh_body(profile, refresh_token);
   if (!body ||
       curl_post_token(profile->token_endpoint, body, profile->format == OAUTH_REFRESH_JSON, &resp,
                       NULL, &http_status) ||
       !resp)
      goto done;

   /* Only the actual OAuth error field can reject a refresh credential. Proxy
    * messages mentioning invalid_grant are not an operator reauthentication. */
   if (http_status >= 400 && http_status < 500)
   {
      cJSON *error_body = cJSON_ParseWithOpts(resp, NULL, 1);
      const cJSON *error = cJSON_GetObjectItemCaseSensitive(error_body, "error");
      int rejected = cJSON_IsString(error) && !strcmp(error->valuestring, "invalid_grant");
      cJSON_Delete(error_body);
      if (rejected)
      {
         aimee_log(LOG_ERROR, "oauth_flow", "refresh rejected for %s: reauthentication required",
                   client_name);
         (void)oauth_secret_store(client_name, OAUTH_VCRED_REAUTH, "1");
         rc = OAUTH_REFRESH_REAUTH;
      }
      goto done;
   }
   /* Never persist an error response even if it carries token-shaped fields. */
   if (http_status < 200 || http_status >= 300 || oauth_token_parse_response(resp, &result))
      goto done;
   rc = oauth_token_store(client_name, &result) ? OAUTH_REFRESH_TRANSIENT : 0;
done:
   OPENSSL_cleanse(refresh_token, sizeof refresh_token);
   OPENSSL_cleanse(&result, sizeof result);
   oauth_cleanse_free(body);
   oauth_cleanse_free(resp);
   return rc;
}

int oauth_token_refresh(const char *client_name, const char *client_id, const char *token_endpoint)
{
   const oauth_refresh_profile_t profile = {.client_id = client_id,
                                            .token_endpoint = token_endpoint};
   return oauth_refresh_profile(client_name, &profile);
}

int oauth_token_get_profile(const char *client_name, const oauth_refresh_profile_t *profile,
                            int skew_secs, char *buf, size_t len)
{
   if (!buf || len == 0)
      return -1;
   buf[0] = '\0';
   if (!client_name || oauth_token_reauth_required(client_name))
      return -1;

   /* Check whether the stored token is near expiry */
   char ea_str[32] = "";
   int need_refresh = 0;
   if (oauth_secret_load(client_name, OAUTH_VCRED_EXPIRES, OAUTH_KEY_EXPIRES_AT, ea_str,
                         sizeof(ea_str)) == 0 &&
       ea_str[0])
   {
      long expires_at = atol(ea_str);
      if (expires_at > 0 && time(NULL) + skew_secs >= expires_at)
         need_refresh = 1;
   }

   if (need_refresh && profile && profile->client_id && profile->token_endpoint)
   {
      if (oauth_refresh_profile(client_name, profile) == OAUTH_REFRESH_REAUTH)
         return -1;
   }

   /* A transient refresh failure may leave a still-valid access token usable,
    * but must never revive an expired token or a rejected credential. */
   if (oauth_token_reauth_required(client_name) || oauth_token_load(client_name, buf, len) != 0 ||
       !buf[0])
   {
      OPENSSL_cleanse(buf, len);
      return -1;
   }
   return 0;
}

int oauth_token_get(const char *client_name, const char *client_id, const char *token_endpoint,
                    int skew_secs, char *buf, size_t len)
{
   const oauth_refresh_profile_t profile = {.client_id = client_id,
                                            .token_endpoint = token_endpoint};
   return oauth_token_get_profile(client_name, &profile, skew_secs, buf, len);
}
