/* git_oauth_github.c — GitHub OAuth device flow → host:github.com token. */
#include "git_oauth_github.h"

#include "aimee.h"      /* MAX_PATH_LEN (needed by agent_types.h via agent_exec.h) */
#include "agent_exec.h" /* agent_http_post_content_type */
#include "cJSON.h"
#include "git_host_cred.h"  /* git_host_cred_set */
#include "oauth_defaults.h" /* AIMEE_DEFAULT_GITHUB_OAUTH_CLIENT_ID */
#include "vault_service.h"  /* store the client_id server-side (set from the UI) */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GH_DEVICE_URL "https://github.com/login/device/code"
#define GH_TOKEN_URL  "https://github.com/login/oauth/access_token"
#define GH_GRANT      "urn:ietf:params:oauth:grant-type:device_code"
#define GH_FORM_CT    "application/x-www-form-urlencoded"
#define GH_ACCEPT     "Accept: application/json"

/* Single-user server: one pending device_code at a time, mutex-guarded. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_device_code[256];
static char g_principal[256];

#define GH_CLIENT_ID_AGENT "git"
#define GH_CLIENT_ID_CRED  "github_oauth_client_id"

/* Resolve the GitHub OAuth App client_id: a value set from the UI (stored in the
 * server vault) takes precedence, else the AIMEE_GITHUB_OAUTH_CLIENT_ID env, else
 * the built-in default baked into the build (oauth_defaults.h). The client_id is
 * public (device flow needs no secret), so a distribution can ship a default and
 * every deployment signs in out of the box. Returns 1 + fills buf, or 0. */
static int get_client_id(char *buf, size_t cap)
{
   if (buf && cap)
      buf[0] = '\0';
   if (!buf || cap == 0)
      return 0;
   /* Stored under the SERVER principal (vault_service_set_server → wrapped_dek);
    * read it the matching way via get_server_principal, not get_server_wrap. */
   if (vault_service_get_server_principal(GH_CLIENT_ID_AGENT, GH_CLIENT_ID_CRED, buf, cap) ==
           VAULT_OK &&
       buf[0])
      return 1;
   const char *env = getenv("AIMEE_GITHUB_OAUTH_CLIENT_ID");
   if (env && env[0])
   {
      snprintf(buf, cap, "%s", env);
      return 1;
   }
   const char *builtin = AIMEE_DEFAULT_GITHUB_OAUTH_CLIENT_ID;
   if (builtin[0])
   {
      snprintf(buf, cap, "%s", builtin);
      return 1;
   }
   buf[0] = '\0';
   return 0;
}

int git_oauth_github_available(void)
{
   char id[256];
   return get_client_id(id, sizeof(id));
}

int git_oauth_github_set_client_id(const char *client_id)
{
   if (!client_id || !client_id[0])
      return -1;
   return vault_service_set_server(GH_CLIENT_ID_AGENT, GH_CLIENT_ID_CRED, client_id) == VAULT_OK
              ? 0
              : -1;
}

int git_oauth_github_get_client_id(char *out, size_t out_len)
{
   return get_client_id(out, out_len);
}

int git_oauth_github_start(const char *principal, char *user_code, size_t uc_len, char *verify_uri,
                           size_t vu_len, int *interval, char *err, size_t errlen)
{
   if (user_code && uc_len)
      user_code[0] = '\0';
   if (verify_uri && vu_len)
      verify_uri[0] = '\0';
   char cid[256];
   if (!get_client_id(cid, sizeof(cid)))
   {
      snprintf(err, errlen, "GitHub sign-in is not configured (set a client ID)");
      return -1;
   }
   char body[512];
   snprintf(body, sizeof(body), "client_id=%s&scope=repo", cid);
   char *resp = NULL;
   int st =
       agent_http_post_content_type(GH_DEVICE_URL, NULL, GH_FORM_CT, body, &resp, 15000, GH_ACCEPT);
   if (st != 200 || !resp)
   {
      free(resp);
      snprintf(err, errlen, "GitHub device request failed");
      return -1;
   }
   cJSON *j = cJSON_Parse(resp);
   free(resp);
   if (!j)
   {
      snprintf(err, errlen, "unexpected GitHub response");
      return -1;
   }
   const cJSON *dc = cJSON_GetObjectItem(j, "device_code");
   const cJSON *uc = cJSON_GetObjectItem(j, "user_code");
   const cJSON *vu = cJSON_GetObjectItem(j, "verification_uri");
   const cJSON *iv = cJSON_GetObjectItem(j, "interval");
   int ok = cJSON_IsString(dc) && cJSON_IsString(uc) && cJSON_IsString(vu);
   if (ok)
   {
      pthread_mutex_lock(&g_lock);
      snprintf(g_device_code, sizeof(g_device_code), "%s", dc->valuestring);
      snprintf(g_principal, sizeof(g_principal), "%s", principal ? principal : "");
      pthread_mutex_unlock(&g_lock);
      snprintf(user_code, uc_len, "%s", uc->valuestring);
      snprintf(verify_uri, vu_len, "%s", vu->valuestring);
      if (interval)
         *interval = cJSON_IsNumber(iv) ? (int)iv->valuedouble : 5;
   }
   else
   {
      snprintf(err, errlen, "incomplete GitHub response");
   }
   cJSON_Delete(j);
   return ok ? 0 : -1;
}

int git_oauth_github_poll(const char *principal, char *err, size_t errlen)
{
   char cid[256];
   if (!get_client_id(cid, sizeof(cid)))
   {
      snprintf(err, errlen, "GitHub sign-in is not configured");
      return -1;
   }
   char dc[256];
   pthread_mutex_lock(&g_lock);
   int has = (g_device_code[0] && (!principal || strcmp(g_principal, principal) == 0));
   snprintf(dc, sizeof(dc), "%s", g_device_code);
   pthread_mutex_unlock(&g_lock);
   if (!has)
   {
      snprintf(err, errlen, "no pending GitHub sign-in");
      return -1;
   }
   char body[640];
   snprintf(body, sizeof(body), "client_id=%s&device_code=%s&grant_type=%s", cid, dc, GH_GRANT);
   char *resp = NULL;
   int st =
       agent_http_post_content_type(GH_TOKEN_URL, NULL, GH_FORM_CT, body, &resp, 15000, GH_ACCEPT);
   if (st < 200 || !resp)
   {
      free(resp);
      snprintf(err, errlen, "GitHub token request failed");
      return -1;
   }
   cJSON *j = cJSON_Parse(resp);
   free(resp);
   if (!j)
   {
      snprintf(err, errlen, "unexpected GitHub response");
      return -1;
   }
   cJSON *at = cJSON_GetObjectItem(j, "access_token");
   const cJSON *e = cJSON_GetObjectItem(j, "error");
   int rc;
   if (cJSON_IsString(at) && at->valuestring[0])
   {
      git_host_cred_set("github.com", at->valuestring);
      /* Wipe the token from the parsed JSON before freeing it. */
      memset(at->valuestring, 0, strlen(at->valuestring));
      pthread_mutex_lock(&g_lock);
      g_device_code[0] = '\0';
      pthread_mutex_unlock(&g_lock);
      rc = 1;
   }
   else if (cJSON_IsString(e) && (strcmp(e->valuestring, "authorization_pending") == 0 ||
                                  strcmp(e->valuestring, "slow_down") == 0))
   {
      rc = 0; /* keep polling */
   }
   else
   {
      snprintf(err, errlen, "%s", cJSON_IsString(e) ? e->valuestring : "sign-in failed");
      pthread_mutex_lock(&g_lock);
      g_device_code[0] = '\0';
      pthread_mutex_unlock(&g_lock);
      rc = -1;
   }
   cJSON_Delete(j);
   return rc;
}
