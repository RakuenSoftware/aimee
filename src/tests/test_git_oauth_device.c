/* test_git_oauth_device.c — pure parts of the GitLab/Gitea device-flow module:
 * provider-name parsing + endpoint construction. No network (the HTTP/vault
 * externals are stubbed for linking only; these tests never invoke them). */

#include "git_oauth_device.h"
#include "vault_service.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── Link-only stubs for the module's externals (never called by these tests) ── */
int agent_http_post_content_type(const char *url, const char *auth_header, const char *content_type,
                                 const char *body, char **response_buf, int timeout_ms,
                                 const char *extra_headers)
{
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
int git_host_cred_set(const char *host, const char *token)
{
   (void)host;
   (void)token;
   return 0;
}
vault_status_t vault_service_set_server(const char *agent, const char *cred, const char *secret)
{
   (void)agent;
   (void)cred;
   (void)secret;
   return VAULT_OK;
}
vault_status_t vault_service_get_server_principal(const char *agent, const char *cred, char *out,
                                                  size_t out_len)
{
   (void)agent;
   (void)cred;
   if (out && out_len)
      out[0] = '\0';
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

int main(void)
{
   printf("test_git_oauth_device:\n");
   test_provider_name();
   test_endpoints();
   printf("ALL PASS\n");
   return 0;
}
