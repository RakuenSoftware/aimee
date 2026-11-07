/* provider_cli_adapter.c: shared provider-CLI delegate adapter registry. */
#include "provider_cli_adapter.h"

#include "aimee.h"
#include "cJSON.h"
#include "runtime_secret.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Non-server adapters/fixtures have no host capture or Vault authority. */
__attribute__((weak)) int provider_cli_adapter_host_execute(const provider_cli_cfg_t *cfg,
                                                            agent_result_t *out)
{
   (void)cfg;
   (void)out;
   return 1;
}

static int provider_cli_resolve_native_env_key(const char *name, char *dst, size_t dst_len)
{
   if (!name || !name[0] || !dst || dst_len == 0)
      return 0;
   /* The name remains environment-shaped for provider compatibility, but the
    * value was sealed during first boot and is available only through the
    * process-local Vault cache. A ~/.vibe/.env fallback would reintroduce a
    * plaintext credential file on aimee-server and is intentionally forbidden. */
   return runtime_secret_get(name, dst, dst_len);
}

int provider_cli_adapter_prepare_native_agent(const provider_cli_adapter_t *adapter,
                                              const agent_t *agent, agent_t *native_agent,
                                              char *errbuf, size_t errbuf_len)
{
   if (errbuf && errbuf_len > 0)
      errbuf[0] = '\0';
   if (!adapter || !agent || !native_agent || !adapter->native_provider ||
       !adapter->native_provider[0])
   {
      if (errbuf && errbuf_len > 0)
         snprintf(errbuf, errbuf_len, "provider adapter has no native provider");
      return -1;
   }

   memcpy(native_agent, agent, sizeof(*native_agent));
   snprintf(native_agent->provider, sizeof(native_agent->provider), "%s", adapter->native_provider);
   native_agent->backend[0] = '\0';
   native_agent->cli_kind[0] = '\0';
   native_agent->cli_cmd[0] = '\0';
   native_agent->cli_idle_timeout_ms = 0;
   native_agent->session_reuse = 0;

   if (!native_agent->endpoint[0] && adapter->native_default_endpoint)
      snprintf(native_agent->endpoint, sizeof(native_agent->endpoint), "%s",
               adapter->native_default_endpoint);
   if (!native_agent->model[0] && adapter->native_default_model)
      snprintf(native_agent->model, sizeof(native_agent->model), "%s",
               adapter->native_default_model);
   if (!native_agent->auth_type[0])
      snprintf(native_agent->auth_type, sizeof(native_agent->auth_type), "%s", "bearer");
   if (native_agent->auth_cmd[0] && strcmp(native_agent->auth_type, "bearer") == 0)
      snprintf(native_agent->auth_type, sizeof(native_agent->auth_type), "%s", "oauth");

   if (!native_agent->api_key[0] && !native_agent->auth_cmd[0] && adapter->native_api_key_env)
   {
      /* Nothing on disk: fall back to the provider's vault slot. Writing the
       * resolved secret into api_key here is deliberate and local -- native_agent
       * is this call's mutable copy, not the cached registry. */
      (void)provider_cli_resolve_native_env_key(adapter->native_api_key_env, native_agent->api_key,
                                                sizeof(native_agent->api_key));
      if (!native_agent->api_key[0])
      {
         if (errbuf && errbuf_len > 0)
            snprintf(errbuf, errbuf_len, "%s is not present in Vault for native %s adapter",
                     adapter->native_api_key_env, adapter->cli_kind);
         return -1;
      }
   }

   if (!native_agent->endpoint[0] || !native_agent->model[0])
   {
      if (errbuf && errbuf_len > 0)
         snprintf(errbuf, errbuf_len, "native %s adapter missing endpoint or model",
                  adapter->cli_kind);
      return -1;
   }

   return 0;
}

/* Retain configured ACP names/capabilities without an outgoing executor.
 * Incoming ACP editor sessions use the protocols module's server edge. */
static const provider_cli_adapter_t acp_provider_cli_adapter = {
    .cli_kind = "acp",
    .display_name = "ACP agent transport",
    .caps = {.max_context_tokens = 0,
             .supports_tool_use = 1,
             .proto_stability = PROVIDER_CLI_PROTO_STABLE,
             .write_confidence = 0.85f},
};

const provider_cli_adapter_t *provider_cli_adapter_get(const char *cli_kind)
{
   static const provider_cli_adapter_t *adapters[] = {
       &codex_provider_cli_adapter,
       &claude_provider_cli_adapter,
       &mistral_provider_cli_adapter,
       &acp_provider_cli_adapter,
       NULL,
   };

   if (!cli_kind || !cli_kind[0])
      return NULL;
   if (strcmp(cli_kind, "mistral-plan") == 0 || strcmp(cli_kind, "vibe") == 0 ||
       strcmp(cli_kind, "vibe-plan") == 0)
      return &mistral_provider_cli_adapter;
   for (int i = 0; adapters[i]; i++)
   {
      if (strcmp(adapters[i]->cli_kind, cli_kind) == 0)
         return adapters[i];
   }
   return NULL;
}
