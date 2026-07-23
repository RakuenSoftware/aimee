#include "management_read.h"

#include "server.h"
#include "agent_config.h"
#include "config.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

int server_mgmt_read_load_agents(server_mgmt_read_agent_t *out, size_t cap, size_t *count)
{
   if (count)
      *count = 0;
   if (!out || !count || cap != SERVER_MGMT_READ_AGENT_MAX)
      return -1;
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || cfg.agent_count < 0 ||
       cfg.agent_count > SERVER_MGMT_READ_AGENT_MAX)
      return -1;
   memset(out, 0, SERVER_MGMT_READ_AGENT_MAX * sizeof(*out));
   for (int i = 0; i < cfg.agent_count; ++i)
   {
      const agent_t *src = &cfg.agents[i];
      server_mgmt_read_agent_t *dst = &out[i];
      if (strnlen(src->name, sizeof(src->name)) >= sizeof(dst->name) ||
          strnlen(src->provider, sizeof(src->provider)) >= sizeof(dst->provider) ||
          strnlen(src->model, sizeof(src->model)) >= sizeof(dst->model))
      {
         memset(out, 0, SERVER_MGMT_READ_AGENT_MAX * sizeof(*out));
         return -1;
      }
      snprintf(dst->name, sizeof(dst->name), "%s", src->name);
      snprintf(dst->provider, sizeof(dst->provider), "%s", src->provider);
      snprintf(dst->model, sizeof(dst->model), "%s", src->model);
      dst->enabled = src->enabled;
      dst->delegate_available = agent_is_available_for_routing(src);
      dst->primary_only = src->primary_only;
      dst->max_parallel = src->max_parallel;
   }
   *count = (size_t)cfg.agent_count;
   return 0;
}

int server_mgmt_read_load_config(server_mgmt_read_config_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out)
      return -1;
   config_t active;
   memset(&active, 0, sizeof(active));
   if (config_load(&active) != 0)
   {
      OPENSSL_cleanse(&active, sizeof(active));
      return -1;
   }
   out->mtls = active.server_api_mtls;
   out->remote_writes = active.server_api_remote_writes;
   memcpy(out->client_transport, active.server_api_client_transport, sizeof(out->client_transport));
   out->cli_session_forwarding = active.server_api_cli_session_forwarding;
   out->require_aimee_git = active.require_aimee_git;
   OPENSSL_cleanse(&active, sizeof(active));
   return 0;
}
