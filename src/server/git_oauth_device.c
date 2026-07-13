/* git_oauth_device.c — provider-generic OAuth 2.0 Device Authorization Grant for
 * GitLab + Gitea/Forgejo. See header. Mirrors git_oauth_github.c's flow, but the
 * endpoints/scope/credential-host + client_id key are resolved per (provider,
 * host) so a self-hosted instance works too. */

#include "git_oauth_device.h"

#include "cJSON.h"
#include "git_host_cred.h"  /* git_host_cred_set */
#include "oauth_defaults.h" /* AIMEE_DEFAULT_GITLAB_OAUTH_CLIENT_ID */
#include "vault_service.h"  /* per-(provider,host) client_id storage */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Declared in agent_exec.h; forward-declared to keep this module free of the heavy
 * agent header chain (which pulls generated headers). Signature must stay in
 * lockstep with agent_exec.h. */
int agent_http_post_content_type(const char *url, const char *auth_header, const char *content_type,
                                 const char *body, char **response_buf, int timeout_ms,
                                 const char *extra_headers);

#define DEV_GRANT     "urn:ietf:params:oauth:grant-type:device_code"
#define DEV_FORM_CT   "application/x-www-form-urlencoded"
#define DEV_ACCEPT    "Accept: application/json"
#define DEV_CID_AGENT "git"

/* Single-user server: one pending device flow at a time, mutex-guarded. Holds
 * everything poll() needs so it targets the right token endpoint + host. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_device_code[256];
static char g_principal[256];
static char g_token_url[512];
static char g_cred_host[256];
static char g_client_id[256];

int oauth_dev_provider_from_name(const char *name, oauth_dev_provider_t *out)
{
   if (!name || !out)
      return -1;
   if (strcmp(name, "gitlab") == 0)
   {
      *out = OAUTH_DEV_GITLAB;
      return 0;
   }
   if (strcmp(name, "gitea") == 0 || strcmp(name, "forgejo") == 0)
   {
      *out = OAUTH_DEV_GITEA;
      return 0;
   }
   return -1;
}

const char *oauth_dev_provider_name(oauth_dev_provider_t p)
{
   return p == OAUTH_DEV_GITLAB ? "gitlab" : "gitea";
}

/* Validate a host as [A-Za-z0-9.:-], non-empty, bounded. */
static int host_ok(const char *s)
{
   if (!s || !s[0] || strlen(s) > 200)
      return 0;
   for (const char *p = s; *p; p++)
      if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == ':'))
         return 0;
   return 1;
}

int oauth_dev_endpoints(oauth_dev_provider_t p, const char *host, char *device_url,
                        size_t device_cap, char *token_url, size_t token_cap, char *cred_host,
                        size_t cred_cap)
{
   /* GitLab defaults to gitlab.com; Gitea is always self-hosted (host required). */
   const char *h = (host && host[0]) ? host : (p == OAUTH_DEV_GITLAB ? "gitlab.com" : NULL);
   if (!h || !host_ok(h))
      return -1;
   if (p == OAUTH_DEV_GITLAB)
   {
      snprintf(device_url, device_cap, "https://%s/oauth/authorize_device", h);
      snprintf(token_url, token_cap, "https://%s/oauth/token", h);
   }
   else /* Gitea / Forgejo */
   {
      snprintf(device_url, device_cap, "https://%s/login/oauth/authorize_device", h);
      snprintf(token_url, token_cap, "https://%s/login/oauth/access_token", h);
   }
   snprintf(cred_host, cred_cap, "%s", h);
   return 0;
}

/* OAuth scope requested per provider (empty ⇒ rely on the app's default scopes). */
static const char *provider_scope(oauth_dev_provider_t p)
{
   return p == OAUTH_DEV_GITLAB ? "read_api read_repository" : "";
}

/* Vault cred key for a (provider, host) client_id: "oauth_cid_<provider>_<host>"
 * with the host reduced to a safe token. */
static void client_id_key(oauth_dev_provider_t p, const char *host, char *out, size_t cap)
{
   const char *h = (host && host[0]) ? host : "default";
   char safe[220];
   size_t o = 0;
   for (const char *s = h; *s && o < sizeof(safe) - 1; s++)
      safe[o++] = isalnum((unsigned char)*s) ? *s : '_';
   safe[o] = '\0';
   snprintf(out, cap, "oauth_cid_%s_%s", oauth_dev_provider_name(p), safe);
}

/* Env + built-in default client_id for a canonical public host. A single shared
 * OAuth App only makes sense for GitLab's SaaS host (gitlab.com); self-hosted
 * GitLab and every Gitea/Forgejo instance runs its own app and must be configured
 * per-host from the UI. Returns 1 + fills buf, or 0. */
static int builtin_client_id(oauth_dev_provider_t p, const char *host, char *buf, size_t cap)
{
   if (p != OAUTH_DEV_GITLAB)
      return 0;
   /* Only the default GitLab host (blank ⇒ gitlab.com) shares the built-in app. */
   if (host && host[0] && strcmp(host, "gitlab.com") != 0)
      return 0;
   const char *env = getenv("AIMEE_GITLAB_OAUTH_CLIENT_ID");
   if (env && env[0])
   {
      snprintf(buf, cap, "%s", env);
      return 1;
   }
   const char *builtin = AIMEE_DEFAULT_GITLAB_OAUTH_CLIENT_ID;
   if (builtin[0])
   {
      snprintf(buf, cap, "%s", builtin);
      return 1;
   }
   return 0;
}

int oauth_dev_get_client_id(oauth_dev_provider_t p, const char *host, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!out || cap == 0)
      return 0;
   char key[256];
   client_id_key(p, host, key, sizeof(key));
   if (vault_service_get_server_principal(DEV_CID_AGENT, key, out, cap) == VAULT_OK && out[0])
      return 1;
   if (builtin_client_id(p, host, out, cap) && out[0])
      return 1;
   out[0] = '\0';
   return 0;
}

int oauth_dev_set_client_id(oauth_dev_provider_t p, const char *host, const char *client_id)
{
   if (!client_id || !client_id[0])
      return -1;
   char key[256];
   client_id_key(p, host, key, sizeof(key));
   return vault_service_set_server(DEV_CID_AGENT, key, client_id) == VAULT_OK ? 0 : -1;
}

int oauth_dev_available(oauth_dev_provider_t p, const char *host)
{
   char id[256];
   return oauth_dev_get_client_id(p, host, id, sizeof(id));
}

int oauth_dev_start(oauth_dev_provider_t p, const char *host, const char *principal,
                    char *user_code, size_t uc_len, char *verify_uri, size_t vu_len, int *interval,
                    char *err, size_t errlen)
{
   if (user_code && uc_len)
      user_code[0] = '\0';
   if (verify_uri && vu_len)
      verify_uri[0] = '\0';

   char device_url[512], token_url[512], cred_host[256];
   if (oauth_dev_endpoints(p, host, device_url, sizeof(device_url), token_url, sizeof(token_url),
                           cred_host, sizeof(cred_host)) != 0)
   {
      snprintf(err, errlen, "a host is required for this provider");
      return -1;
   }
   char cid[256];
   if (!oauth_dev_get_client_id(p, host, cid, sizeof(cid)))
   {
      snprintf(err, errlen, "sign-in is not configured (set a client ID)");
      return -1;
   }

   char body[640];
   const char *scope = provider_scope(p);
   if (scope[0])
      snprintf(body, sizeof(body), "client_id=%s&scope=%s", cid, scope);
   else
      snprintf(body, sizeof(body), "client_id=%s", cid);

   char *resp = NULL;
   int st =
       agent_http_post_content_type(device_url, NULL, DEV_FORM_CT, body, &resp, 15000, DEV_ACCEPT);
   if (st != 200 || !resp)
   {
      free(resp);
      snprintf(err, errlen, "device request failed (status %d)", st);
      return -1;
   }
   cJSON *j = cJSON_Parse(resp);
   free(resp);
   if (!j)
   {
      snprintf(err, errlen, "unexpected response from the host");
      return -1;
   }
   const cJSON *dc = cJSON_GetObjectItem(j, "device_code");
   const cJSON *uc = cJSON_GetObjectItem(j, "user_code");
   const cJSON *vu = cJSON_GetObjectItem(j, "verification_uri");
   if (!cJSON_IsString(vu))
      vu = cJSON_GetObjectItem(j, "verification_uri_complete");
   const cJSON *iv = cJSON_GetObjectItem(j, "interval");
   int ok = cJSON_IsString(dc) && cJSON_IsString(uc) && cJSON_IsString(vu);
   if (ok)
   {
      pthread_mutex_lock(&g_lock);
      snprintf(g_device_code, sizeof(g_device_code), "%s", dc->valuestring);
      snprintf(g_principal, sizeof(g_principal), "%s", principal ? principal : "");
      snprintf(g_token_url, sizeof(g_token_url), "%s", token_url);
      snprintf(g_cred_host, sizeof(g_cred_host), "%s", cred_host);
      snprintf(g_client_id, sizeof(g_client_id), "%s", cid);
      pthread_mutex_unlock(&g_lock);
      snprintf(user_code, uc_len, "%s", uc->valuestring);
      snprintf(verify_uri, vu_len, "%s", vu->valuestring);
      if (interval)
         *interval = cJSON_IsNumber(iv) ? (int)iv->valuedouble : 5;
   }
   else
   {
      snprintf(err, errlen, "incomplete device response");
   }
   cJSON_Delete(j);
   return ok ? 0 : -1;
}

int oauth_dev_poll(oauth_dev_provider_t p, const char *host, const char *principal, char *err,
                   size_t errlen)
{
   (void)p;
   (void)host;
   char dc[256], token_url[512], cred_host[256], cid[256];
   pthread_mutex_lock(&g_lock);
   int has = (g_device_code[0] && (!principal || strcmp(g_principal, principal) == 0));
   snprintf(dc, sizeof(dc), "%s", g_device_code);
   snprintf(token_url, sizeof(token_url), "%s", g_token_url);
   snprintf(cred_host, sizeof(cred_host), "%s", g_cred_host);
   snprintf(cid, sizeof(cid), "%s", g_client_id);
   pthread_mutex_unlock(&g_lock);
   if (!has)
   {
      snprintf(err, errlen, "no pending sign-in");
      return -1;
   }

   char body[768];
   snprintf(body, sizeof(body), "client_id=%s&device_code=%s&grant_type=%s", cid, dc, DEV_GRANT);
   char *resp = NULL;
   int st =
       agent_http_post_content_type(token_url, NULL, DEV_FORM_CT, body, &resp, 15000, DEV_ACCEPT);
   if (st < 200 || !resp)
   {
      free(resp);
      snprintf(err, errlen, "token request failed (status %d)", st);
      return -1;
   }
   cJSON *j = cJSON_Parse(resp);
   free(resp);
   if (!j)
   {
      snprintf(err, errlen, "unexpected response from the host");
      return -1;
   }
   cJSON *at = cJSON_GetObjectItem(j, "access_token");
   const cJSON *e = cJSON_GetObjectItem(j, "error");
   int rc;
   if (cJSON_IsString(at) && at->valuestring[0])
   {
      git_host_cred_set(cred_host, at->valuestring);
      memset(at->valuestring, 0, strlen(at->valuestring)); /* wipe before free */
      pthread_mutex_lock(&g_lock);
      g_device_code[0] = '\0';
      g_client_id[0] = '\0';
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
