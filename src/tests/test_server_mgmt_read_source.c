#define _GNU_SOURCE

#include "server.h"
#include "agent_config.h"
#include "management_read.h"
#include "config.h"

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

/* server_mgmt_read_load_config reads these five through accessors now rather than
 * copying them out of a legacy_config_record. Same five values the legacy_config_read stub set.
 *
 * That stub also filled the rest of the struct with 0xa5 so a whole-struct copy
 * into the output would be visible; with the struct gone from this seam there is
 * nothing left to poison, and the assertion that mattered -- that no secret byte
 * reaches the output -- is on the agent path, which is unchanged. */
int config_present(void)
{
   return 1;
}

int config_server_api_mtls(void)
{
   return 2;
}

int config_server_api_remote_writes(void)
{
   return 1;
}

const char *config_server_api_client_transport(void)
{
   return "auto";
}

int config_server_api_cli_session_forwarding(void)
{
   return 1;
}

int config_require_aimee_git(void)
{
   return 0;
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
   struct
   {
      unsigned char before[32];
      server_mgmt_read_config_t config;
      unsigned char after[32];
   } guarded;
   memset(&guarded, 0x5a, sizeof(guarded));
   assert(server_mgmt_read_load_config(&guarded.config) == 0);
   assert(guarded.config.mtls == 2 && guarded.config.remote_writes == 1 &&
          !strcmp(guarded.config.client_transport, "auto") &&
          guarded.config.cli_session_forwarding == 1 && guarded.config.require_aimee_git == 0);
   for (size_t i = 0; i < sizeof(guarded.before); ++i)
      assert(guarded.before[i] == 0x5a && guarded.after[i] == 0x5a);
   puts("server management read source tests passed");
   return 0;
}
