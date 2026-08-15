/* test_git_oauth_device.c — pure parts of the GitLab/Gitea device-flow module:
 * provider-name parsing + endpoint construction. No network (the HTTP/vault
 * externals are stubbed for linking only; these tests never invoke them). */

#include "modules/git/git_oauth_device.h"
#include "vault_service.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── Link-only stubs for the module's externals (never called by these tests) ── */
/* Canned POST reply, so the device flow can be walked start -> poll without a
 * network. NULL leaves the original link-only behaviour (no response body). */
static const char *g_http_reply = NULL;
int agent_http_post_content_type(const char *url, const char *auth_header, const char *content_type,
                                 const char *body, char **response_buf, int timeout_ms,
                                 const char *extra_headers)
{
   if (g_http_reply && response_buf)
   {
      *response_buf = strdup(g_http_reply);
      return 200;
   }
   (void)url;
   (void)auth_header;
   (void)content_type;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   if (response_buf)
      *response_buf = NULL;
   return 0;
}
/* Scripted so the sign-in completion can be driven both ways: the store is a
 * separate failure from the provider returning a token, and the whole point of
 * checking it is what happens when it fails. */
static int g_cred_set_rc = 0;
static int g_cred_set_calls = 0;
int git_host_cred_set(const char *host, const char *token)
{
   (void)host;
   (void)token;
   g_cred_set_calls++;
   return g_cred_set_rc;
}
vault_status_t vault_service_set_server(const char *agent, const char *cred, const char *secret)
{
   (void)agent;
   (void)cred;
   (void)secret;
   return VAULT_OK;
}
/* A configured client ID, when a test needs the flow to get past "sign-in is
 * not configured". Gitea has no built-in app, so this is the only way in. */
static const char *g_vault_client_id = NULL;
vault_status_t vault_service_get_server_principal(const char *agent, const char *cred, char *out,
                                                  size_t out_len)
{
   (void)agent;
   (void)cred;
   if (out && out_len)
      out[0] = '\0';
   if (g_vault_client_id && out && out_len)
   {
      snprintf(out, out_len, "%s", g_vault_client_id);
      return VAULT_OK;
   }
   return VAULT_NO_ENTRY;
}

/* ── Tests ──────────────────────────────────────────────────────────────────── */

static void test_provider_name(void)
{
   oauth_dev_provider_t p;
   assert(oauth_dev_provider_from_name("gitlab", &p) == 0 && p == OAUTH_DEV_GITLAB);
   assert(oauth_dev_provider_from_name("gitea", &p) == 0 && p == OAUTH_DEV_GITEA);
   assert(oauth_dev_provider_from_name("forgejo", &p) == 0 && p == OAUTH_DEV_GITEA);
   assert(oauth_dev_provider_from_name("github", &p) == -1); /* GitHub uses its own module */
   assert(oauth_dev_provider_from_name("bogus", &p) == -1);
   assert(oauth_dev_provider_from_name(NULL, &p) == -1);
   assert(strcmp(oauth_dev_provider_name(OAUTH_DEV_GITLAB), "gitlab") == 0);
   assert(strcmp(oauth_dev_provider_name(OAUTH_DEV_GITEA), "gitea") == 0);
   printf("  provider name: OK\n");
}

static void test_endpoints(void)
{
   char dev[512], tok[512], cred[256];

   /* GitLab defaults to gitlab.com when no host is given. */
   assert(oauth_dev_endpoints(OAUTH_DEV_GITLAB, NULL, dev, sizeof(dev), tok, sizeof(tok), cred,
                              sizeof(cred)) == 0);
   assert(strcmp(dev, "https://gitlab.com/oauth/authorize_device") == 0);
   assert(strcmp(tok, "https://gitlab.com/oauth/token") == 0);
   assert(strcmp(cred, "gitlab.com") == 0);

   /* Self-hosted GitLab. */
   assert(oauth_dev_endpoints(OAUTH_DEV_GITLAB, "gitlab.example.com", dev, sizeof(dev), tok,
                              sizeof(tok), cred, sizeof(cred)) == 0);
   assert(strcmp(dev, "https://gitlab.example.com/oauth/authorize_device") == 0);
   assert(strcmp(cred, "gitlab.example.com") == 0);

   /* Gitea requires a host and uses the /login/oauth/* paths. */
   assert(oauth_dev_endpoints(OAUTH_DEV_GITEA, "gitea.example.com", dev, sizeof(dev), tok,
                              sizeof(tok), cred, sizeof(cred)) == 0);
   assert(strcmp(dev, "https://gitea.example.com/login/oauth/authorize_device") == 0);
   assert(strcmp(tok, "https://gitea.example.com/login/oauth/access_token") == 0);
   assert(strcmp(cred, "gitea.example.com") == 0);

   /* Gitea with no host is rejected (no sensible default). */
   assert(oauth_dev_endpoints(OAUTH_DEV_GITEA, NULL, dev, sizeof(dev), tok, sizeof(tok), cred,
                              sizeof(cred)) == -1);
   /* A malformed host is rejected. */
   assert(oauth_dev_endpoints(OAUTH_DEV_GITLAB, "bad host!", dev, sizeof(dev), tok, sizeof(tok),
                              cred, sizeof(cred)) == -1);
   printf("  endpoints: OK\n");
}

/* Completing a sign-in means the token is STORED, not merely received. The
 * provider returning one and the vault keeping it fail independently, and
 * reporting completion on the first alone leaves the host with no credential
 * while the user is told they are connected — after which every forge call
 * reports "no credential" and points back at a sign-in that looked fine. */
static void test_poll_requires_the_token_to_be_stored(void)
{
   char user_code[64], verify[256], err[256];
   int interval = 0;

   /* Walk a real start -> poll against canned replies. */
   g_vault_client_id = "CID-TEST";
   g_http_reply = "{\"device_code\":\"DEV-CODE\",\"user_code\":\"UC-1\","
                  "\"verification_uri\":\"https://gitea.example.com/login/device\"}";
   assert(oauth_dev_start(OAUTH_DEV_GITEA, "gitea.example.com", "alice", user_code,
                          sizeof(user_code), verify, sizeof(verify), &interval, err,
                          sizeof(err)) == 0);

   /* The store succeeds → the sign-in completes. */
   g_http_reply = "{\"access_token\":\"TOK-1\"}";
   g_cred_set_rc = 0;
   g_cred_set_calls = 0;
   err[0] = '\0';
   assert(oauth_dev_poll(OAUTH_DEV_GITEA, "gitea.example.com", "alice", err, sizeof(err)) == 1);
   assert(g_cred_set_calls == 1);
   assert(err[0] == '\0');

   /* The store fails → the sign-in must FAIL and say so, not report complete.
    * Before this was checked, the call returned 1 here and the credential was
    * silently gone. */
   g_http_reply = "{\"device_code\":\"DEV-CODE\",\"user_code\":\"UC-2\","
                  "\"verification_uri\":\"https://gitea.example.com/login/device\"}";
   assert(oauth_dev_start(OAUTH_DEV_GITEA, "gitea.example.com", "alice", user_code,
                          sizeof(user_code), verify, sizeof(verify), &interval, err,
                          sizeof(err)) == 0);
   g_http_reply = "{\"access_token\":\"TOK-2\"}";
   g_cred_set_rc = -1;
   g_cred_set_calls = 0;
   err[0] = '\0';
   assert(oauth_dev_poll(OAUTH_DEV_GITEA, "gitea.example.com", "alice", err, sizeof(err)) == -1);
   assert(g_cred_set_calls == 1);
   assert(strstr(err, "could not store") != NULL);

   g_http_reply = NULL;
   g_cred_set_rc = 0;
   g_vault_client_id = NULL;
   printf("  poll requires the token to be stored: OK\n");
}

int main(void)
{
   printf("test_git_oauth_device:\n");
   test_provider_name();
   test_endpoints();
   test_poll_requires_the_token_to_be_stored();
   printf("ALL PASS\n");
   return 0;
}
