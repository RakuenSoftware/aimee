#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aimee.h"
#include "agent.h"
#include "agent_exec.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "provider_cli_adapter.h"
#include "runtime_secret.h"
#include <aimee/ir/session_event.h>

/* provider_cli_adapter now resolves an agent's key through the vault module
 * rather than reading agent_t.api_key directly, because config stores the
 * on-disk form verbatim. This minimal-link test does not pull the vault module,
 * so mirror the contract exactly: a literal is the key; a "$VAR" reference is
 * resolved from the runtime secret cache. */
int agent_api_key_secret(const agent_t *agent, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';
   if (!agent || !agent->api_key[0])
      return 0;
   if (agent->api_key[0] == '$')
      return (runtime_secret_get(agent->api_key + 1, out, out_len) && out[0]) ? 1 : 0;
   snprintf(out, out_len, "%s", agent->api_key);
   return 1;
}

/* Minimal harnesses have no bound server turn to cancel. */
int agent_request_cancelled(void)
{
   return 0;
}

static void test_registry_and_caps(void)
{
   const provider_cli_adapter_t *codex = provider_cli_adapter_get("codex");
   const provider_cli_adapter_t *claude = provider_cli_adapter_get("claude");
   const provider_cli_adapter_t *mistral = provider_cli_adapter_get("mistral");

   assert(codex != NULL);
   assert(claude != NULL);
   assert(mistral != NULL);
   assert(provider_cli_adapter_get("mistral-plan") == mistral);
   assert(provider_cli_adapter_get("vibe") == mistral);
   assert(provider_cli_adapter_get("vibe-plan") == mistral);
   assert(provider_cli_adapter_get("missing") == NULL);
   const provider_cli_adapter_t *acp = provider_cli_adapter_get("acp");
   assert(acp && acp->execute == NULL); /* Existing config metadata only. */

   assert(strcmp(codex->cli_kind, "codex") == 0);
   assert(codex->execute != NULL);
   /* Codex must advertise its real context window + tool use so capability
    * routing keeps it as a delegate for roles with a min-context floor (e.g.
    * review). A 0 here silently drops codex from every such fleet. */
   assert(codex->caps.supports_tool_use == 1);
   /* Exact value (not a floor) so an accidental drift back toward 0 is caught. */
   assert(codex->caps.max_context_tokens == 272000);
   assert(claude->execute == NULL); /* Metadata only; the owned factory executes Claude. */
   assert(claude->caps.proto_stability == PROVIDER_CLI_PROTO_STABLE);
   /* The gemini CLI adapter is gone: it pinned the NATIVE Google endpoint
    * (generativelanguage.googleapis.com/v1beta, GEMINI_API_KEY), which is exactly
    * the bespoke path being retired. Pinned as unregistered so it cannot come
    * back by accident. */
   assert(provider_cli_adapter_get("gemini") == NULL);
   assert(mistral->caps.supports_tool_use == 1);
   assert(mistral->caps.proto_stability == PROVIDER_CLI_PROTO_NATIVE);
   assert(mistral->execute == NULL); /* Native HTTP bridge metadata, not a second executor. */
   assert(strcmp(mistral->native_provider, "mistral") == 0);
   assert(strcmp(mistral->native_default_endpoint, "https://api.mistral.ai/v1") == 0);
   assert(strcmp(mistral->native_default_model, "mistral-vibe-cli-latest") == 0);
   assert(strcmp(mistral->native_api_key_env, "MISTRAL_API_KEY") == 0);
}

static void test_mistral_native_http_configuration(void)
{
   const provider_cli_adapter_t *mistral = provider_cli_adapter_get("mistral-plan");
   assert(mistral != NULL);

   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.name, sizeof(agent.name), "mistral-test");
   snprintf(agent.provider, sizeof(agent.provider), "mistral");
   snprintf(agent.backend, sizeof(agent.backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
   snprintf(agent.cli_kind, sizeof(agent.cli_kind), "mistral-plan");
   snprintf(agent.cli_cmd, sizeof(agent.cli_cmd), "/no/such/vibe");
   agent.tools_enabled = 1;
   agent.max_tokens = 1234;
   agent.timeout_ms = 4321;

   assert(runtime_secret_store("MISTRAL_API_KEY", "unit-test-mistral-key") == 0);
   agent_t native_agent;
   char error[256];
   assert(!provider_cli_adapter_prepare_native_agent(mistral, &agent, &native_agent, error,
                                                     sizeof error));
   assert(strcmp(native_agent.name, "mistral-test") == 0);
   assert(strcmp(native_agent.provider, "mistral") == 0);
   assert(native_agent.backend[0] == '\0');
   assert(native_agent.cli_kind[0] == '\0');
   assert(native_agent.cli_cmd[0] == '\0');
   assert(strcmp(native_agent.endpoint, "https://api.mistral.ai/v1") == 0);
   assert(strcmp(native_agent.model, "mistral-vibe-cli-latest") == 0);
   assert(strcmp(native_agent.auth_type, "bearer") == 0);
   assert(strcmp(native_agent.api_key, "unit-test-mistral-key") == 0);
   assert(native_agent.tools_enabled == 1 && native_agent.max_tokens == 1234 &&
          native_agent.timeout_ms == 4321);
   runtime_secret_remove("MISTRAL_API_KEY");
}

static void test_native_auth_cmd_uses_bearer_token_command(void)
{
   const provider_cli_adapter_t *mistral = provider_cli_adapter_get("mistral");
   assert(mistral != NULL);

   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.name, sizeof(agent.name), "mistral-auth-cmd-test");
   snprintf(agent.backend, sizeof(agent.backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
   snprintf(agent.cli_kind, sizeof(agent.cli_kind), "mistral");
   snprintf(agent.auth_cmd, sizeof(agent.auth_cmd), "/bin/echo token");

   agent_t native_agent;
   char err[256];
   assert(provider_cli_adapter_prepare_native_agent(mistral, &agent, &native_agent, err,
                                                    sizeof(err)) == 0);
   assert(strcmp(native_agent.auth_type, "oauth") == 0);
   assert(strcmp(native_agent.auth_cmd, "/bin/echo token") == 0);
   assert(native_agent.api_key[0] == '\0');
}

static void test_missing_and_invalid_cli_fail_cleanly(void)
{
   assert(provider_cli_command_has_shell_operators("/bin/cat ; /bin/true") == 1);
   assert(provider_cli_check_available("/bin/cat ; /bin/true", "codex") == 0);
   assert(provider_cli_check_available("/bin/cat", "codex") == 1);
   assert(provider_cli_check_available("/no/such/provider-cli", "codex") == 0);
}

int main(void)
{
   test_registry_and_caps();
   test_mistral_native_http_configuration();
   test_native_auth_cmd_uses_bearer_token_command();
   test_missing_and_invalid_cli_fail_cleanly();
   printf("provider CLI adapter tests passed\n");
   return 0;
}
