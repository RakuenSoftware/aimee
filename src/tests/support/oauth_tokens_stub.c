/* oauth_tokens_stub.c: stub the server-hosted OAuth token accessors for unit
 * tests that link agent_config.o (agent_resolve_auth consults them for
 * server-hosted-OAuth agents) but don't exercise OAuth. The real implementation
 * (server/oauth_tokens.c) pulls in secret_store + oauth_pkce + http_retry, which
 * a unit test shouldn't need. "no token available" is the correct test default. */
#include "oauth_flow.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static char g_test_oauth_token[4096];

void test_oauth_tokens_reset(void)
{
   memset(g_test_oauth_token, 0, sizeof(g_test_oauth_token));
}

int oauth_token_get(const char *client_name, const char *client_id, const char *token_endpoint,
                    int skew_secs, char *buf, size_t len)
{
   (void)client_name;
   (void)client_id;
   (void)token_endpoint;
   (void)skew_secs;
   if (!buf || len == 0 || !g_test_oauth_token[0])
   {
      if (buf && len)
         buf[0] = '\0';
      return -1;
   }
   snprintf(buf, len, "%s", g_test_oauth_token);
   return 0;
}

int oauth_token_store(const char *client_name, const oauth_token_response_t *resp)
{
   (void)client_name;
   if (!resp || !resp->access_token[0])
      return -1;
   snprintf(g_test_oauth_token, sizeof(g_test_oauth_token), "%s", resp->access_token);
   return 0;
}

int oauth_token_reauth_required(const char *client_name)
{
   (void)client_name;
   return 0; /* tests don't exercise the REAUTH_REQUIRED marker */
}
