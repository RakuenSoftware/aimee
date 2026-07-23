#include "server.h"
#include "agent_config.h"
#include "management_read.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->agent_count = 1;
   agent_t *a = &cfg->agents[0];
   snprintf(a->name, sizeof(a->name), "%s", "safe-agent");
   snprintf(a->provider, sizeof(a->provider), "%s", "openai");
   snprintf(a->model, sizeof(a->model), "%s", "gpt-5.2");
   snprintf(a->endpoint, sizeof(a->endpoint), "%s", "https://secret.invalid/v1");
   snprintf(a->api_key, sizeof(a->api_key), "%s", "secret-canary");
   snprintf(a->auth_cmd, sizeof(a->auth_cmd), "%s", "/secret/command");
   snprintf(a->extra_headers, sizeof(a->extra_headers), "%s", "X-Secret: canary");
   a->enabled = 1;
   a->primary_only = 0;
   a->max_parallel = 7;
   return 0;
}

int agent_is_available_for_routing(const agent_t *agent)
{
   return agent && agent->enabled && !agent->primary_only;
}

int main(void)
{
   server_mgmt_read_agent_t out[SERVER_MGMT_READ_AGENT_MAX];
   size_t count = 99;
   assert(server_mgmt_read_load_agents(out, SERVER_MGMT_READ_AGENT_MAX, &count) == 0);
   assert(count == 1);
   assert(!strcmp(out[0].name, "safe-agent"));
   assert(!strcmp(out[0].provider, "openai"));
   assert(!strcmp(out[0].model, "gpt-5.2"));
   assert(out[0].enabled == 1 && out[0].delegate_available == 1 && out[0].primary_only == 0 &&
          out[0].max_parallel == 7);
   const unsigned char *bytes = (const unsigned char *)&out[0];
   assert(!memmem(bytes, sizeof(out[0]), "secret", 6));
   count = 99;
   assert(server_mgmt_read_load_agents(out, SERVER_MGMT_READ_AGENT_MAX - 1, &count) < 0);
   assert(count == 0);
   puts("server management read source tests passed");
   return 0;
}
