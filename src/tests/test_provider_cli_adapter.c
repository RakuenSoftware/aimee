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

static agent_t g_seen_agent;
static char g_seen_system[256];
static char g_seen_user[256];
static int g_seen_max_tokens;
static double g_seen_temperature;

int agent_execute_with_tools(const agent_t *agent, const agent_network_t *network,
                             const char *system_prompt, const char *user_prompt, int max_tokens,
                             double temperature, agent_result_t *out)
{
   (void)network;
   assert(agent != NULL);
   assert(out != NULL);
   memcpy(&g_seen_agent, agent, sizeof(g_seen_agent));
   snprintf(g_seen_system, sizeof(g_seen_system), "%s", system_prompt ? system_prompt : "");
   snprintf(g_seen_user, sizeof(g_seen_user), "%s", user_prompt ? user_prompt : "");
   g_seen_max_tokens = max_tokens;
   g_seen_temperature = temperature;

   memset(out, 0, sizeof(*out));
   snprintf(out->agent_name, sizeof(out->agent_name), "%s", agent->name);
   assert(!agent_result_from_host_text(out, "native ok"));
   out->success = 1;
   out->turns = 1;
   return 0;
}

static void reset_seen_agent(void)
{
   memset(&g_seen_agent, 0, sizeof(g_seen_agent));
   g_seen_system[0] = '\0';
   g_seen_user[0] = '\0';
   g_seen_max_tokens = 0;
   g_seen_temperature = 0.0;
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

   assert(strcmp(codex->cli_kind, "codex") == 0);
   assert(codex->execute != NULL);
   /* Codex must advertise its real context window + tool use so capability
    * routing keeps it as a delegate for roles with a min-context floor (e.g.
    * review). A 0 here silently drops codex from every such fleet. */
   assert(codex->caps.supports_tool_use == 1);
   /* Exact value (not a floor) so an accidental drift back toward 0 is caught. */
   assert(codex->caps.max_context_tokens == 272000);
   assert(claude->spawn != NULL);
   assert(claude->caps.proto_stability == PROVIDER_CLI_PROTO_STABLE);
   /* The gemini CLI adapter is gone: it pinned the NATIVE Google endpoint
    * (generativelanguage.googleapis.com/v1beta, GEMINI_API_KEY), which is exactly
    * the bespoke path being retired. Pinned as unregistered so it cannot come
    * back by accident. */
   assert(provider_cli_adapter_get("gemini") == NULL);
   assert(mistral->caps.supports_tool_use == 1);
   assert(mistral->caps.proto_stability == PROVIDER_CLI_PROTO_NATIVE);
   assert(mistral->spawn == NULL);
   assert(mistral->execute != NULL);
   assert(strcmp(mistral->native_provider, "mistral") == 0);
   assert(strcmp(mistral->native_default_endpoint, "https://api.mistral.ai/v1") == 0);
   assert(strcmp(mistral->native_default_model, "mistral-vibe-cli-latest") == 0);
   assert(strcmp(mistral->native_api_key_env, "MISTRAL_API_KEY") == 0);
}

static void test_claude_stream_json_does_not_duplicate_final_result(void)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/aimee-fake-claude-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   FILE *f = fdopen(fd, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\n"
         "cat >/dev/null\n"
         "printf '%s\\n' "
         "'{\"type\":\"assistant\",\"message\":{\"id\":\"fixture\",\"role\":\"assistant\","
         "\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}}'\n"
         "printf '%s\\n' "
         "'{\"type\":\"result\",\"is_error\":false,\"result\":\"ok\",\"duration_ms\":5}'\n",
         f);
   fclose(f);
   chmod(path, 0700);

   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.name, sizeof(agent.name), "fake-claude");
   snprintf(agent.backend, sizeof(agent.backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
   snprintf(agent.cli_kind, sizeof(agent.cli_kind), "claude");
   snprintf(agent.cli_cmd, sizeof(agent.cli_cmd), "%s", path);
   agent.timeout_ms = 5000;

   agent_result_t out = {0};
   assert(provider_cli_adapter_execute(provider_cli_adapter_get("claude"), &agent, ".", "sys",
                                       "user", &out) == 0);
   assert(out.success == 1);
   assert(out.response != NULL);
   assert(strcmp(out.response, "ok") == 0);
   assert(out.turns == 1);
   agent_result_clear_response(&out);
   unlink(path);
}

static int reject_live_capture(const aimee_session_event_t *event, void *context)
{
   int *count = context;
   assert(event->kind == AIMEE_SESSION_MESSAGE && event->message &&
          !strcmp(event->message->role, "assistant"));
   ++*count;
   return -1;
}
static void test_provider_cli_captures_before_child_completion(void)
{
   char path[512], marker[544];
   snprintf(path, sizeof path, "%s/aimee-live-claude-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   snprintf(marker, sizeof marker, "%s.marker", path);
   FILE *script = fdopen(fd, "w");
   assert(script);
   fputs("#!/bin/sh\ncat >/dev/null\n"
         "printf '%s\\n' "
         "'{\"type\":\"assistant\",\"message\":{\"id\":\"live\",\"role\":\"assistant\",\"content\":"
         "[{\"type\":\"text\",\"text\":\"pending\"}]}}'\n"
         "sleep 2\n",
         script);
   fprintf(script, "touch '%s'\n", marker);
   fclose(script);
   assert(chmod(path, 0700) == 0);
   agent_t agent = {0};
   snprintf(agent.cli_cmd, sizeof agent.cli_cmd, "%s", path);
   agent.timeout_ms = 5000;
   int observed = 0;
   aimee_ir_session_set_sink(reject_live_capture, &observed);
   agent_result_t out = {0};
   assert(provider_cli_adapter_execute(provider_cli_adapter_get("claude"), &agent, ".", "", "user",
                                       &out) == AGENT_RC_CAPTURE_FAILED);
   assert(observed == 1 && !out.success && access(marker, F_OK) != 0);
   aimee_ir_session_set_sink(NULL, NULL);
   agent_result_clear_response(&out);
   unlink(path);
}

static void test_provider_cli_honors_workflow_tool_loop_cap(void)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/aimee-slow-claude-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   FILE *f = fdopen(fd, "w");
   assert(f != NULL);
   fputs("#!/bin/sh\ncat >/dev/null\nsleep 2\n", f);
   fclose(f);
   chmod(path, 0700);

   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.name, sizeof(agent.name), "slow-claude");
   snprintf(agent.backend, sizeof(agent.backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
   snprintf(agent.cli_kind, sizeof(agent.cli_kind), "claude");
   snprintf(agent.cli_cmd, sizeof(agent.cli_cmd), "%s", path);
   agent.timeout_ms = 5000;
   agent.tool_loop_timeout_ms_cap = 50;

   agent_result_t out = {0};
   assert(provider_cli_adapter_execute(provider_cli_adapter_get("claude"), &agent, ".", "sys",
                                       "user", &out) != 0);
   assert(strstr(out.error, "timed out after 50 ms") != NULL);
   unlink(path);
}

static void test_mistral_native_adapter_execution(void)
{
   const provider_cli_adapter_t *mistral = provider_cli_adapter_get("mistral-plan");
   assert(mistral != NULL);
   assert(mistral->spawn == NULL);

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

   reset_seen_agent();
   assert(runtime_secret_store("MISTRAL_API_KEY", "unit-test-mistral-key") == 0);
   agent_result_t out = {0};
   assert(provider_cli_adapter_execute(mistral, &agent, ".", "sys", "user", &out) == 0);
   assert(out.success == 1);
   assert(out.response != NULL);
   assert(strcmp(out.response, "native ok") == 0);
   assert(strcmp(g_seen_agent.name, "mistral-test") == 0);
   assert(strcmp(g_seen_agent.provider, "mistral") == 0);
   assert(g_seen_agent.backend[0] == '\0');
   assert(g_seen_agent.cli_kind[0] == '\0');
   assert(g_seen_agent.cli_cmd[0] == '\0');
   assert(strcmp(g_seen_agent.endpoint, "https://api.mistral.ai/v1") == 0);
   assert(strcmp(g_seen_agent.model, "mistral-vibe-cli-latest") == 0);
   assert(strcmp(g_seen_agent.auth_type, "bearer") == 0);
   assert(strcmp(g_seen_agent.api_key, "unit-test-mistral-key") == 0);
   assert(g_seen_agent.tools_enabled == 1);
   assert(g_seen_agent.max_tokens == 1234);
   assert(g_seen_agent.timeout_ms == 4321);
   agent_result_clear_response(&out);
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

static int forbidden_legacy_spawn(const provider_cli_cfg_t *cfg, const char *prompt, int *input,
                                  int *output, pid_t *pid)
{
   (void)cfg;
   (void)prompt;
   (void)input;
   (void)output;
   (void)pid;
   assert(!"an adapter without a canonical executor must never spawn");
   return -1;
}
static int flat_executor(const provider_cli_cfg_t *cfg, agent_result_t *result)
{
   (void)cfg;
   result->response = strdup("unqualified native text");
   assert(result->response);
   result->success = 1;
   return 0;
}
static int passive_capture(const aimee_session_event_t *event, void *context)
{
   (void)event;
   (void)context;
   return 0;
}
static void test_no_flat_child_fallback(void)
{
   provider_cli_adapter_t legacy = {.display_name = "legacy", .spawn = forbidden_legacy_spawn};
   provider_cli_cfg_t cfg = {.user_prompt = "task"};
   agent_result_t result = {0};
   assert(provider_cli_adapter_execute_cfg(&legacy, &cfg, &result) == AGENT_RC_CONTEXT_REFUSED);
   assert(!result.response && !result.canonical_response.content && !result.success);
   legacy.execute = flat_executor;
   assert(provider_cli_adapter_execute_cfg(&legacy, &cfg, &result) == AGENT_RC_CAPTURE_FAILED);
   assert(!result.success);
   agent_result_clear_response(&result);
   aimee_ir_session_set_sink(passive_capture, NULL);
   assert(provider_cli_adapter_execute_cfg(&legacy, &cfg, &result) == AGENT_RC_CAPTURE_FAILED);
   assert(!result.success && aimee_ir_session_capture_failed());
   agent_result_clear_response(&result);
   aimee_ir_session_set_sink(NULL, NULL); /* Independent negative fixture ends here. */

   for (const char **kind = (const char *[]){"codex", "claude", "mistral", "acp", NULL}; *kind;
        kind++)
      assert(provider_cli_adapter_get(*kind)->execute);
}

int main(void)
{
   test_no_flat_child_fallback();
   test_registry_and_caps();
   test_claude_stream_json_does_not_duplicate_final_result();
   test_provider_cli_captures_before_child_completion();
   test_provider_cli_honors_workflow_tool_loop_cap();
   test_mistral_native_adapter_execution();
   test_native_auth_cmd_uses_bearer_token_command();
   test_missing_and_invalid_cli_fail_cleanly();
   printf("provider CLI adapter tests passed\n");
   return 0;
}
