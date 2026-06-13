/* oauth_tokens.c: OAuth token storage, exchange, and refresh.
 *
 * Implements the token-layer of the PKCE flow:
 *   - JSON parsing of RFC 6749 §5.1 token responses
 *   - secret_store persistence keyed by MCP client name
 *   - Token exchange (authorization_code grant)
 *   - Token refresh (refresh_token grant)
 *   - High-level oauth_token_get() with auto-refresh
 *
 * Network: uses platform_exec_pipe with curl (same pattern as HyDE rewrite
 * and cross-encoder commands) so the build stays dependency-free.
 */
#include "aimee.h"
#include "db1.h"
#include "oauth_flow.h"
#include "oauth_pkce.h"
#include "vault_service.h" /* server-sealed vault: encrypted-at-rest token storage */
#include "cJSON.h"
#include "log.h"
#include "platform_process.h"
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* OAuth tokens are encrypted at rest in the server-owned vault (WP-C.4), keyed by
 * (VAULT_SERVER_PRINCIPAL, client_name, cred). The server decrypts them
 * autonomously to refresh/use them. Previously they were stored via db1/secrets,
 * which in a container falls back to a PLAINTEXT 0600 file — the leak this closes.
 * The legacy plaintext entries are migrated lazily on first read and scrubbed. */
#define OAUTH_VCRED_ACCESS  "oauth_access_token"
#define OAUTH_VCRED_REFRESH "oauth_refresh_token"
#define OAUTH_VCRED_EXPIRES "oauth_expires_at"

static int oauth_secret_store(const char *client_name, const char *vcred, const char *value)
{
   if (vault_service_set_server(client_name, vcred, value) == VAULT_OK)
      return 0;
   aimee_log(LOG_WARN, "oauth_flow", "vault store failed for %s/%s (token not persisted)",
             client_name, vcred);
   return -1;
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
   snprintf(key, sizeof(key), db1_fmt, client_name);
   if (db1_secret_load(key, buf, len) == 0 && buf[0])
   {
      /* Scrub the plaintext ONLY after a durable encrypted copy exists. If the
       * vault write fails (e.g. no master key), keep the plaintext and retry the
       * migration on the next read — never delete the only copy of the token. The
       * value is valid either way, so we still return it to the caller. */
      if (vault_service_set_server(client_name, vcred, buf) == VAULT_OK)
         (void)db1_secret_remove(key);
      return 0;
   }
   buf[0] = '\0';
   return -1;
}

static void oauth_secret_remove(const char *client_name, const char *vcred, const char *db1_fmt)
{
   (void)vault_service_delete(VAULT_SERVER_PRINCIPAL, client_name, vcred);
   char key[256];
   snprintf(key, sizeof(key), db1_fmt, client_name);
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

   if (resp->expires_at > 0)
   {
      char ea_str[32];
      snprintf(ea_str, sizeof(ea_str), "%ld", resp->expires_at);
      if (oauth_secret_store(client_name, OAUTH_VCRED_EXPIRES, ea_str) != 0)
         rc = -1;
   }

   return rc;
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
 * |post_data| is the application/x-www-form-urlencoded body.
 * On success, |*resp_out| is a malloc'd JSON string (caller frees).
 */
static int curl_post_token(const char *token_endpoint, const char *post_data, char **resp_out,
                           size_t *resp_len_out)
{
   if (!token_endpoint || !post_data || !resp_out)
      return -1;

   /* Build command: curl -s -X POST -H 'Content-Type: application/x-www-form-urlencoded'
    *   --data-urlencode not needed here because we pre-encode.
    *   We use -d with the raw body (caller must percent-encode values). */
   char cmd[4096];
   snprintf(cmd, sizeof(cmd),
            "curl -sf -X POST"
            " -H 'Content-Type: application/x-www-form-urlencoded'"
            " -H 'Accept: application/json'"
            " --data-binary @-"
            " '%s'",
            token_endpoint);

   return platform_exec_pipe(cmd, post_data, strlen(post_data), resp_out, resp_len_out);
}

/* Minimal percent-encoding for OAuth POST body values.
 * Encodes characters that are not unreserved per RFC 3986 §2.3. */
static void pct_encode(const char *in, char *out, size_t outlen)
{
   static const char *safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
   size_t pos = 0;
   for (; *in && pos + 4 < outlen; in++)
   {
      if (strchr(safe, (unsigned char)*in))
         out[pos++] = *in;
      else
      {
         snprintf(out + pos, outlen - pos, "%%%02X", (unsigned char)*in);
         pos += 3;
      }
   }
   out[pos] = '\0';
}

int oauth_token_refresh(const char *client_name, const char *client_id, const char *token_endpoint)
{
   if (!client_name || !client_id || !token_endpoint)
      return -1;

   /* Load the stored refresh token */
   char refresh_token[512] = "";
   if (oauth_secret_load(client_name, OAUTH_VCRED_REFRESH, OAUTH_KEY_REFRESH_TOKEN, refresh_token,
                         sizeof(refresh_token)) != 0 ||
       !refresh_token[0])
   {
      aimee_log(LOG_WARN, "oauth_flow", "no refresh token stored for %s", client_name);
      return -1;
   }

   char enc_refresh[512], enc_client[256];
   pct_encode(refresh_token, enc_refresh, sizeof(enc_refresh));
   pct_encode(client_id, enc_client, sizeof(enc_client));

   char body[2048];
   snprintf(body, sizeof(body),
            "grant_type=refresh_token"
            "&refresh_token=%s"
            "&client_id=%s",
            enc_refresh, enc_client);

   char *resp = NULL;
   size_t resp_len = 0;
   if (curl_post_token(token_endpoint, body, &resp, &resp_len) != 0 || !resp)
   {
      free(resp);
      aimee_log(LOG_WARN, "oauth_flow", "token refresh failed for %s", client_name);
      return -1;
   }

   oauth_token_response_t result;
   int rc = oauth_token_parse_response(resp, &result);
   free(resp);
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "oauth_flow", "token refresh returned invalid JSON for %s", client_name);
      return -1;
   }

   return oauth_token_store(client_name, &result);
}

int oauth_token_get(const char *client_name, const char *client_id, const char *token_endpoint,
                    int skew_secs, char *buf, size_t len)
{
   if (!client_name || !buf || len == 0)
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

   if (need_refresh && client_id && token_endpoint)
      oauth_token_refresh(client_name, client_id, token_endpoint);

   /* Try to load the (possibly refreshed) access token */
   if (oauth_secret_load(client_name, OAUTH_VCRED_ACCESS, OAUTH_KEY_ACCESS_TOKEN, buf, len) != 0 ||
       buf[0] == '\0')
      return -1;

   return 0;
}
