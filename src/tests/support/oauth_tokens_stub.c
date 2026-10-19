/* oauth_tokens_stub.c: stub the server-hosted OAuth token accessors for unit
 * tests that link agent_config.o (agent_resolve_auth consults them for
 * server-hosted-OAuth agents) but don't exercise OAuth. The real implementation
 * (server/oauth_tokens.c) pulls in secret_store + oauth_pkce + http_retry, which
 * a unit test shouldn't need. "no token available" is the correct test default. */
#include "oauth_flow.h"
#include "vault_service.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static struct
{
   char client[128], token[4096];
} tokens[16];
int test_oauth_stores, test_oauth_removes, test_oauth_get_fail;
void test_oauth_tokens_reset(void)
{
   memset(tokens, 0, sizeof tokens);
   test_oauth_stores = test_oauth_removes = test_oauth_get_fail = 0;
}
int oauth_token_get(const char *client_name, const char *client_id, const char *token_endpoint,
                    int skew_secs, char *buf, size_t capacity)
{
   (void)client_id;
   (void)token_endpoint;
   (void)skew_secs;
   if (!buf || !capacity)
      return -1;
   buf[0] = 0;
   for (size_t i = 0; !test_oauth_get_fail && i < 16; i++)
      if (!strcmp(tokens[i].client, client_name) && tokens[i].token[0])
      {
         if (strlen(tokens[i].token) >= capacity)
            return -1;
         snprintf(buf, capacity, "%s", tokens[i].token);
         return 0;
      }
   return -1;
}
int oauth_token_store(const char *client_name, const oauth_token_response_t *response)
{
   if (!response || !response->access_token[0])
      return -1;
   size_t selected = 16;
   for (size_t i = 0; i < 16; i++)
      if (!strcmp(tokens[i].client, client_name))
      {
         selected = i;
         break;
      }
      else if (selected == 16 && !tokens[i].client[0])
         selected = i;
   if (selected == 16)
      return -1;
   snprintf(tokens[selected].client, sizeof tokens[selected].client, "%s", client_name);
   snprintf(tokens[selected].token, sizeof tokens[selected].token, "%s", response->access_token);
   test_oauth_stores++;
   return 0;
}
int oauth_token_remove(const char *client_name)
{
   for (size_t i = 0; i < 16; i++)
      if (!strcmp(tokens[i].client, client_name))
         memset(&tokens[i], 0, sizeof tokens[i]);
   test_oauth_removes++;
   return 0;
}

int oauth_token_reauth_required(const char *client_name)
{
   (void)client_name;
   return 0; /* tests don't exercise the REAUTH_REQUIRED marker */
}

char test_oauth_profile_client[256], test_oauth_profile_scope[4096],
    test_oauth_profile_endpoint[256];
int test_oauth_profile_format;
int oauth_token_get_profile(const char *client_name, const oauth_refresh_profile_t *profile,
                            int skew_secs, char *buf, size_t capacity)
{
   snprintf(test_oauth_profile_client, sizeof test_oauth_profile_client, "%s", profile->client_id);
   snprintf(test_oauth_profile_scope, sizeof test_oauth_profile_scope, "%s", profile->scope);
   snprintf(test_oauth_profile_endpoint, sizeof test_oauth_profile_endpoint, "%s",
            profile->token_endpoint);
   test_oauth_profile_format = profile->format;
   return oauth_token_get(client_name, profile->client_id, profile->token_endpoint, skew_secs, buf,
                          capacity);
}

int oauth_token_store_source(const char *client_name, const oauth_token_response_t *response,
                             const char *source_digest)
{
   if (oauth_token_store(client_name, response))
      return -1;
   return vault_service_set_server(client_name, OAUTH_SOURCE_CREDENTIAL, source_digest) == VAULT_OK
              ? 0
              : -1;
}
