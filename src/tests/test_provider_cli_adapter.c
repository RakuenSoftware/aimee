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
#include "workspace_provider.h"

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
   out->response = strdup("native ok");
   assert(out->response != NULL);
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
   const provider_cli_adapter_t *gemini = provider_cli_adapter_get("gemini");
   const provider_cli_adapter_t *mistral = provider_cli_adapter_get("mistral");

   assert(codex != NULL);
   assert(claude != NULL);
   assert(gemini != NULL);
   assert(mistral != NULL);
   assert(provider_cli_adapter_get("mistral-plan") == mistral);
   assert(provider_cli_adapter_get("vibe") == mistral);
   assert(provider_cli_adapter_get("vibe-plan") == mistral);
   assert(provider_cli_adapter_get("missing") == NULL);

   assert(strcmp(codex->cli_kind, "codex") == 0);
   assert(codex->execute != NULL);
   assert(claude->spawn != NULL);
   assert(claude->parse_line != NULL);
   assert(claude->caps.proto_stability == PROVIDER_CLI_PROTO_STABLE);
   assert(gemini->caps.supports_tool_use == 1);
   assert(gemini->caps.proto_stability == PROVIDER_CLI_PROTO_NATIVE);
   assert(gemini->spawn == NULL);
   assert(gemini->execute != NULL);
   assert(strcmp(gemini->native_provider, "gemini") == 0);
   assert(strcmp(gemini->native_default_endpoint,
                 "https://generativelanguage.googleapis.com/v1beta") == 0);
   assert(strcmp(gemini->native_default_model, "gemini-2.5-flash") == 0);
   assert(strcmp(gemini->native_api_key_env, "GEMINI_API_KEY") == 0);
   assert(mistral->caps.supports_tool_use == 1);
   assert(mistral->caps.proto_stability == PROVIDER_CLI_PROTO_NATIVE);
   assert(mistral->spawn == NULL);
   assert(mistral->execute != NULL);
   assert(strcmp(mistral->native_provider, "mistral") == 0);
   assert(strcmp(mistral->native_default_endpoint, "https://api.mistral.ai/v1") == 0);
   assert(strcmp(mistral->native_default_model, "mistral-vibe-cli-latest") == 0);
   assert(strcmp(mistral->native_api_key_env, "MISTRAL_API_KEY") == 0);
}

static void test_common_json_parse_text_tool_and_error(void)
{
   cli_event_t ev;

   assert(provider_cli_parse_json_line_common(
              "{\"type\":\"message\",\"role\":\"assistant\",\"content\":\"hello\"}", &ev) == 1);
   assert(ev.type == CLI_EVENT_TEXT_DELTA);
   assert(strcmp(ev.text, "hello") == 0);

   assert(provider_cli_parse_json_line_common("{\"type\":\"tool_call\",\"name\":\"Write\"}", &ev) ==
          1);
   assert(ev.type == CLI_EVENT_TOOL_START);
   assert(strcmp(ev.tool_name, "Write") == 0);
   assert(provider_cli_event_is_write(&ev) == 1);

   assert(provider_cli_parse_json_line_common("{\"type\":\"error\",\"message\":\"boom\"}", &ev) ==
          1);
   assert(ev.type == CLI_EVENT_ERROR);
   assert(strcmp(ev.text, "boom") == 0);
}

static void test_claude_parse_stream_json(void)
{
   const provider_cli_adapter_t *claude = provider_cli_adapter_get("claude");
   assert(claude != NULL);

   cli_event_t ev;
   const char *delta = "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
                       "\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}}";
   assert(claude->parse_line(delta, &ev) == 1);
   assert(ev.type == CLI_EVENT_TEXT_DELTA);
   assert(strcmp(ev.text, "hi") == 0);

   const char *write = "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_start\","
                       "\"content_block\":{\"type\":\"tool_use\",\"name\":\"Write\"}}}";
   assert(claude->parse_line(write, &ev) == 1);
   assert(ev.type == CLI_EVENT_TOOL_START);
   assert(strcmp(ev.tool_name, "Write") == 0);
   assert(claude->is_write_event(&ev) == 1);

   const char *usage = "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_delta\","
                       "\"usage\":{\"input_tokens\":12,\"output_tokens\":7,"
                       "\"cache_creation_input_tokens\":3,\"cache_read_input_tokens\":4}}}";
   assert(claude->parse_line(usage, &ev) == 1);
   assert(ev.prompt_tokens == 12);
   assert(ev.completion_tokens == 7);
   assert(ev.cache_write_tokens == 3);
   assert(ev.cache_read_tokens == 4);

   const char *result = "{\"type\":\"result\",\"result\":\"hi\",\"duration_ms\":23}";
   assert(claude->parse_line(result, &ev) == 1);
   assert(ev.type == CLI_EVENT_TURN_COMPLETE);
   assert(strcmp(ev.text, "hi") == 0);
   assert(ev.latency_ms == 23);
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
         "printf '%s\\n' '{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
         "\"delta\":{\"type\":\"text_delta\",\"text\":\"ok\"}}}'\n"
         "printf '%s\\n' '{\"type\":\"result\",\"result\":\"ok\",\"duration_ms\":5}'\n",
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

   agent_result_t out;
   assert(provider_cli_adapter_execute(provider_cli_adapter_get("claude"), &agent, ".", "sys",
                                       "user", &out) == 0);
   assert(out.success == 1);
   assert(out.response != NULL);
   assert(strcmp(out.response, "ok") == 0);
   assert(out.turns == 1);
   free(out.response);
   unlink(path);
}

static void test_gemini_native_adapter_execution(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.name, sizeof(agent.name), "gemini-test");
   snprintf(agent.provider, sizeof(agent.provider), "gemini");
   snprintf(agent.backend, sizeof(agent.backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
   snprintf(agent.cli_kind, sizeof(agent.cli_kind), "gemini");
   snprintf(agent.cli_cmd, sizeof(agent.cli_cmd), "/no/such/gemini");
   agent.cli_idle_timeout_ms = 5000;
   agent.max_tokens = 2345;

   reset_seen_agent();
   setenv("GEMINI_API_KEY", "unit-test-gemini-key", 1);
   agent_result_t out;
   assert(provider_cli_adapter_execute(provider_cli_adapter_get("gemini"), &agent, ".", "sys",
                                       "user", &out) == 0);
   assert(out.success == 1);
   assert(out.response != NULL);
   assert(strcmp(out.response, "native ok") == 0);
   assert(strcmp(g_seen_system, "sys") == 0);
   assert(strcmp(g_seen_user, "user") == 0);
   assert(g_seen_max_tokens == 2345);
   assert(g_seen_temperature < 0.0);
   assert(strcmp(g_seen_agent.name, "gemini-test") == 0);
   assert(strcmp(g_seen_agent.provider, "gemini") == 0);
   assert(g_seen_agent.backend[0] == '\0');
   assert(g_seen_agent.cli_kind[0] == '\0');
   assert(g_seen_agent.cli_cmd[0] == '\0');
   assert(strcmp(g_seen_agent.endpoint, "https://generativelanguage.googleapis.com/v1beta") == 0);
   assert(strcmp(g_seen_agent.model, "gemini-2.5-flash") == 0);
   assert(strcmp(g_seen_agent.auth_type, "none") == 0);
   assert(g_seen_agent.api_key[0] == '\0');
   assert(strstr(g_seen_agent.extra_headers, "x-goog-api-key: unit-test-gemini-key") != NULL);
   free(out.response);
   unsetenv("GEMINI_API_KEY");
}

static void test_gemini_native_bearer_mechanism(void)
{
   const provider_cli_adapter_t *gemini = provider_cli_adapter_get("gemini");
   assert(gemini != NULL);

   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.name, sizeof(agent.name), "gemini-bearer-test");
   snprintf(agent.backend, sizeof(agent.backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
   snprintf(agent.cli_kind, sizeof(agent.cli_kind), "gemini");
   snprintf(agent.api_key, sizeof(agent.api_key), "bearer-key");

   setenv("GEMINI_API_KEY_AUTH_MECHANISM", "bearer", 1);
   agent_t native_agent;
   char err[256];
   assert(provider_cli_adapter_prepare_native_agent(gemini, &agent, &native_agent, err,
                                                    sizeof(err)) == 0);
   assert(strcmp(native_agent.provider, "gemini") == 0);
   assert(strcmp(native_agent.auth_type, "bearer") == 0);
   assert(strcmp(native_agent.api_key, "bearer-key") == 0);
   assert(strstr(native_agent.extra_headers, "x-goog-api-key") == NULL);
   unsetenv("GEMINI_API_KEY_AUTH_MECHANISM");
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
   setenv("MISTRAL_API_KEY", "unit-test-mistral-key", 1);
   agent_result_t out;
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
   free(out.response);
   unsetenv("MISTRAL_API_KEY");
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
   assert(provider_cli_check_available("/bin/cat ; /bin/true", "gemini") == 0);
   assert(provider_cli_check_available("/bin/cat", "gemini") == 1);
   assert(provider_cli_check_available("/no/such/provider-cli", "gemini") == 0);
}

static void test_tool_result_format(void)
{
   char buf[256];
   assert(provider_cli_format_json_tool_result("Read", "{\"ok\":true}", buf, sizeof(buf)) == 0);
   assert(strstr(buf, "\"type\":\"tool_result\"") != NULL);
   assert(strstr(buf, "\"name\":\"Read\"") != NULL);
   assert(strstr(buf, "\"ok\":true") != NULL);
}

/* A detached workspace must run the CLI agent on the client via exec_stream
 * (not fork/exec locally). This mock stands in for the reverse channel: it
 * records the marshalled argv + stdin and streams back claude stream-json. */
static char g_mock_argv0[64];
static char g_mock_argv_join[512];
static char g_mock_stdin[128];
static int g_mock_called;

static int mock_exec_stream(const workspace_provider_t *p, const char *const argv[],
                            const char *stdin_data, size_t stdin_len, const char *cwd,
                            ws_exec_chunk_fn on_chunk, void *cb_ctx)
{
   (void)p;
   (void)cwd;
   g_mock_called = 1;
   snprintf(g_mock_argv0, sizeof(g_mock_argv0), "%s", argv[0] ? argv[0] : "");
   g_mock_argv_join[0] = '\0';
   for (int i = 0; argv[i]; i++)
   {
      strncat(g_mock_argv_join, argv[i], sizeof(g_mock_argv_join) - strlen(g_mock_argv_join) - 2);
      strncat(g_mock_argv_join, " ", sizeof(g_mock_argv_join) - strlen(g_mock_argv_join) - 1);
   }
   snprintf(g_mock_stdin, sizeof(g_mock_stdin), "%.*s", (int)stdin_len,
            stdin_data ? stdin_data : "");

   /* two claude stream-json text deltas, split across chunk boundaries */
   const char *c1 = "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
                    "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello \"}}}\n{\"type\":\"str";
   const char *c2 = "eam_event\",\"event\":{\"type\":\"content_block_delta\","
                    "\"delta\":{\"type\":\"text_delta\",\"text\":\"world\"}}}\n";
   if (on_chunk(cb_ctx, c1, strlen(c1)) != 0)
      return -1;
   if (on_chunk(cb_ctx, c2, strlen(c2)) != 0)
      return -1;
   return 0;
}

static void test_detached_routes_to_exec_stream(void)
{
   const provider_cli_adapter_t *claude = provider_cli_adapter_get("claude");
   assert(claude != NULL && claude->build_argv != NULL);

   workspace_provider_t mock = {0};
   mock.kind = WS_PROVIDER_DETACHED;
   mock.exec_stream = mock_exec_stream;

   g_mock_called = 0;
   workspace_provider_set_active(&mock);

   agent_t agent;
   memset(&agent, 0, sizeof(agent));
   snprintf(agent.name, sizeof(agent.name), "claude");
   snprintf(agent.backend, sizeof(agent.backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
   snprintf(agent.cli_kind, sizeof(agent.cli_kind), "claude");

   agent_result_t out;
   int rc = provider_cli_adapter_execute(claude, &agent, "/work", "be brief", "say hi", &out);
   workspace_provider_clear_active();

   assert(g_mock_called == 1);                  /* ran on the client, not locally */
   assert(strcmp(g_mock_argv0, "claude") == 0); /* claude argv was marshalled */
   assert(strstr(g_mock_argv_join, "stream-json") != NULL);
   assert(strcmp(g_mock_stdin, "say hi") == 0); /* prompt fed on stdin */
   assert(rc == 0);
   assert(out.success == 1);
   assert(out.response && strcmp(out.response, "Hello world") == 0); /* deltas reassembled */
   free(out.response);
}

int main(void)
{
   test_registry_and_caps();
   test_common_json_parse_text_tool_and_error();
   test_claude_parse_stream_json();
   test_claude_stream_json_does_not_duplicate_final_result();
   test_detached_routes_to_exec_stream();
   test_gemini_native_adapter_execution();
   test_gemini_native_bearer_mechanism();
   test_mistral_native_adapter_execution();
   test_native_auth_cmd_uses_bearer_token_command();
   test_missing_and_invalid_cli_fail_cleanly();
   test_tool_result_format();
   printf("provider CLI adapter tests passed\n");
   return 0;
}
