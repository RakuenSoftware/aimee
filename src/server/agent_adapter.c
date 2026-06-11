/* agent_adapter.c: direct agent adapter registry.
 *
 * An adapter is the provider-facing contract that can be used both as a
 * primary conversational agent and as a bounded delegate turn. CLI backends are
 * intentionally not direct adapters.
 */
#include "agent_adapter.h"

#include <string.h>

static const agent_adapter_t g_adapters[] = {
    {
        .name = "codex",
        .provider = "chatgpt",
        .display_name = "Codex",
        .default_endpoint = "https://chatgpt.com/backend-api/codex",
        .default_model = "gpt-5.5",
        .auth_type = "codex-oauth",
        .capabilities = AGENT_ADAPTER_CAP_DELEGATE_TURN | AGENT_ADAPTER_CAP_PRIMARY_CONVERSATION |
                        AGENT_ADAPTER_CAP_DIRECT_HTTP | AGENT_ADAPTER_CAP_PRIMARY_SESSION,
    },
    {
        .name = "mistral",
        .provider = "mistral",
        .display_name = "Mistral AI",
        .default_endpoint = "https://api.mistral.ai/v1",
        .default_model = "mistral-large-latest",
        .auth_type = "oauth",
        .capabilities = AGENT_ADAPTER_CAP_DELEGATE_TURN | AGENT_ADAPTER_CAP_PRIMARY_CONVERSATION |
                        AGENT_ADAPTER_CAP_DIRECT_HTTP | AGENT_ADAPTER_CAP_PRIMARY_SESSION,
    },
    {
        .name = "gemini",
        .provider = "gemini",
        .display_name = "Gemini",
        .default_endpoint = "https://generativelanguage.googleapis.com/v1beta",
        .default_model = "gemini-2.5-flash",
        .auth_type = "none",
        .capabilities = AGENT_ADAPTER_CAP_DELEGATE_TURN | AGENT_ADAPTER_CAP_PRIMARY_CONVERSATION |
                        AGENT_ADAPTER_CAP_DIRECT_HTTP | AGENT_ADAPTER_CAP_PRIMARY_SESSION,
    },
    {
        .name = "minimax",
        .provider = "minimax",
        .display_name = "MiniMax",
        .default_endpoint = "https://api.minimax.io/v1",
        .default_model = "MiniMax-M2.7",
        .auth_type = "bearer",
        .capabilities = AGENT_ADAPTER_CAP_DELEGATE_TURN | AGENT_ADAPTER_CAP_PRIMARY_CONVERSATION |
                        AGENT_ADAPTER_CAP_DIRECT_HTTP | AGENT_ADAPTER_CAP_PRIMARY_SESSION,
    },
    {0},
};

static int streq(const char *a, const char *b)
{
   return a && b && strcmp(a, b) == 0;
}

const agent_adapter_t *agent_adapter_for_name(const char *name)
{
   if (!name || !name[0])
      return NULL;
   for (int i = 0; g_adapters[i].name; i++)
      if (streq(g_adapters[i].name, name))
         return &g_adapters[i];
   return NULL;
}

const agent_adapter_t *agent_adapter_for_provider(const char *provider)
{
   if (!provider || !provider[0])
      return NULL;
   for (int i = 0; g_adapters[i].name; i++)
      if (streq(g_adapters[i].provider, provider) || streq(g_adapters[i].name, provider))
         return &g_adapters[i];
   return NULL;
}

const agent_adapter_t *agent_adapter_for_agent(const agent_t *agent)
{
   const agent_adapter_t *adapter = NULL;
   if (!agent)
      return NULL;
   adapter = agent_adapter_for_provider(agent->provider);
   if (adapter)
      return adapter;
   return agent_adapter_for_name(agent->name);
}

int agent_adapter_supports(const agent_adapter_t *adapter, int capability)
{
   return adapter && ((adapter->capabilities & capability) == capability);
}

int agent_adapter_agent_is_direct(const agent_t *agent)
{
   const agent_adapter_t *adapter = agent_adapter_for_agent(agent);
   if (!agent_adapter_supports(adapter, AGENT_ADAPTER_CAP_DIRECT_HTTP))
      return 0;
   return strcmp(agent->backend, AGENT_BACKEND_PROVIDER_CLI) != 0 &&
          strcmp(agent->backend, AGENT_BACKEND_CLI_STDIO) != 0 &&
          strcmp(agent->backend, AGENT_BACKEND_TMUX_CLI) != 0;
}
