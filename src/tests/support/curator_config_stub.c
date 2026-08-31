/* Accessor stubs over curator_config_stub_t -- see the header for why these exist
 * rather than linking the generated accessors. */
#include "curator_config_stub.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

curator_config_stub_t cfg;

const char *config_kb_curator_provider_base_url(void)
{
   return cfg.kb_curator_provider_base_url;
}

const char *config_kb_curator_provider_model(void)
{
   return cfg.kb_curator_provider_model;
}

const char *config_kb_curator_provider_api_key(void)
{
   return cfg.kb_curator_provider_api_key;
}

const char *config_synthesis_endpoint(void)
{
   return cfg.synthesis_endpoint;
}

int config_synth_chat_endpoint_current(char *out, size_t n)
{
   const char *endpoint = getenv("SYNTHESIS_ENDPOINT");
   if (!endpoint || !endpoint[0])
      endpoint = cfg.synthesis_endpoint;
   if (!out || n == 0 || !endpoint[0])
      return 0;
   size_t len = strlen(endpoint);
   while (len && endpoint[len - 1] == '/')
      len--;
   if (len == 0)
   {
      out[0] = 0;
      return 0;
   }
   int has_v1 = len >= 3 && !strncmp(endpoint + len - 3, "/v1", 3);
   int wrote = snprintf(out, n, "%.*s%s", (int)len, endpoint, has_v1 ? "" : "/v1");
   return wrote >= 0 && (size_t)wrote < n;
}

int config_synthesis_thinking(void)
{
   return cfg.synthesis_thinking;
}

int config_kb_curator_extract_max_tokens(void)
{
   return cfg.kb_curator_extract_max_tokens;
}
