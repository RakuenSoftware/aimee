/* cli_mistral.c: Mistral native adapter for legacy provider-CLI routes. */
#include "provider_cli_adapter.h"

#include "agent_exec.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static int mistral_native_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   if (!cfg || !cfg->agent)
   {
      snprintf(out->error, sizeof(out->error), "mistral adapter: missing agent");
      return -1;
   }

   agent_t native_agent;
   char err[256];
   if (provider_cli_adapter_prepare_native_agent(&mistral_provider_cli_adapter, cfg->agent,
                                                 &native_agent, err, sizeof(err)) != 0)
   {
      snprintf(out->error, sizeof(out->error), "%s", err[0] ? err : "mistral adapter failed");
      return -1;
   }

   int max_tokens =
       native_agent.max_tokens > 0 ? native_agent.max_tokens : AGENT_DEFAULT_MAX_TOKENS;
   return agent_execute_with_tools(&native_agent, NULL, cfg->system_prompt, cfg->user_prompt,
                                   max_tokens, -1.0, out);
}

const provider_cli_adapter_t mistral_provider_cli_adapter = {
    .cli_kind = "mistral",
    .display_name = "Mistral native adapter",
    .caps = {.max_context_tokens = 0,
             .supports_tool_use = 1,
             .proto_stability = PROVIDER_CLI_PROTO_NATIVE,
             .write_confidence = 0.9f},
    .spawn = NULL,
    .native_provider = "mistral",
    .native_default_endpoint = "https://api.mistral.ai/v1",
    .native_default_model = "mistral-vibe-cli-latest",
    .native_api_key_env = "MISTRAL_API_KEY",
    .execute = mistral_native_execute,
};
