/* Factory lifecycle fixture. Peer owners are explicit mocks; actual private HOME,
 * executable descriptors, launch configuration and cleanup use production code.
 * Actual SDK/config protocol fidelity is covered by test_codex_native_adoption.py. */
#include "server_native_execution.h"
#include "server_native_runtime.h"
#include "server_native_cache.h"
#include "server_session_capture.h"
#include "request_context.h"
#include "agent_exec.h"
#include "agent_protocol.h"
#include <aimee/tools/agent_tools.h>
#include <aimee/ir/session_event.h>
#include "modules/workspace/workspace_turn.h"
#include "db1_client/primary_sessions.h"
#include "cJSON.h"
#include "cli_session.h"
#include "platform_test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct server_native_runtime
{
   int unused;
};
struct server_native_cache
{
   int unused;
};
struct server_session_capture
{
   int unused;
};
static struct server_native_runtime runtime;
static struct server_native_cache cache;
static struct server_session_capture capture;
static request_context_t owner;
static server_session_terminal_action_fn terminal;
static void *terminal_resource;
static int mode, retained, opened, joined, child_closed, began, completed, aborted, closed;
static int terminal_committed, sdk_calls, restored, ready_calls, rejected_terminal;
static char parent_role[64], private_home[4096], host_workspace[4096];
static int child_executable, child_directory;
static const char digest[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const request_context_t *request_context_get(void)
{
   return &owner;
}
void agent_request_creds_snapshot(agent_request_creds_t *out)
{
   memset(out, 0, sizeof *out);
   snprintf(out->session_id, sizeof out->session_id, "transport");
   snprintf(out->vault_principal, sizeof out->vault_principal, "actor");
}
int server_session_capture_retain_current(const char *session, const char *actor,
                                          server_session_capture_t **out)
{
   assert(!strcmp(session, "transport") && !strcmp(actor, "actor"));
   *out = &capture;
   retained++;
   return 0;
}
void server_session_capture_release(server_session_capture_t *held)
{
   if (held)
   {
      assert(held == &capture && retained == 1);
      retained--;
   }
}
const char *server_session_capture_session(const server_session_capture_t *held)
{
   assert(held == &capture);
   return "destination";
}
const char *server_session_capture_turn(const server_session_capture_t *held)
{
   assert(held == &capture);
   return "turn";
}
int server_session_capture_history(server_session_capture_t *held, cJSON **out)
{
   assert(held == &capture && joined && child_closed);
   *out =
       cJSON_Parse(mode == 5 ? "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_"
                               "id\":\"pending\",\"name\":\"read_file\",\"arguments\":{}}]}]"
                             : "[]");
   return 0;
}
int server_session_capture_after_terminal(server_session_capture_t *held,
                                          server_session_terminal_action_fn action, void *resource)
{
   assert(held == &capture && joined && child_closed && !terminal);
   if (rejected_terminal)
      return -1;
   terminal = action;
   terminal_resource = resource;
   return 0;
}
int workspace_turn_resource_identity(const char *cwd, int *fd, char identity[65])
{
   assert(!strcmp(cwd, host_workspace));
   *fd = open(cwd, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   assert(*fd >= 0);
   snprintf(identity, 65, "%s", digest);
   return 0;
}
void agent_tools_scope_bookmark(agent_tools_scope_snapshot_t *out)
{
   memset(out, 0, sizeof *out);
   snprintf(out->role, sizeof out->role, "%s", parent_role);
}
void agent_tools_set_dispatch_role(const char *role)
{
   snprintf(parent_role, sizeof parent_role, "%s", role ? role : "primary");
}
void agent_tools_scope_restore(const agent_tools_scope_snapshot_t *scope)
{
   snprintf(parent_role, sizeof parent_role, "%s", scope->role);
   restored++;
}
int agent_tools_knowledge_write_allowed(void)
{
   return 0;
}
cJSON *build_tools_array_responses(void)
{
   return cJSON_CreateArray();
}
void agent_tools_filter_for_role(cJSON *tools, const char *role)
{
   assert(cJSON_IsArray(tools));
   assert(!strcmp(parent_role, role ? role : "primary"));
}
char *agent_build_exec_context_checked(const agent_t *agent, const agent_network_t *network,
                                       const char *role, const char *custom, int skip, char *error,
                                       size_t size)
{
   (void)network;
   (void)role;
   (void)custom;
   (void)error;
   (void)size;
   assert(!strcmp(agent->model, "selected-model") && skip);
   return strdup("host environment");
}
int agent_prepare_request_ir(const agent_t *agent, const cJSON *messages, cJSON *tools,
                             const char *system, int max_tokens, double temperature,
                             aimee_wire_t wire, int delegate, aimee_request_t *out)
{
   (void)max_tokens;
   (void)temperature;
   (void)delegate;
   assert(cJSON_GetArraySize(messages) == 0 && cJSON_IsArray(tools));
   assert(!strcmp(system, "host environment") && wire == AIMEE_WIRE_RESPONSES);
   out->model = strdup(agent->model);
   out->tools = calloc(1, sizeof *out->tools);
   assert(out->tools);
   out->n_tools = 1;
   out->tools[0].name = strdup("read_file");
   out->tools[0].schema = cJSON_Parse("{\"type\":\"object\"}");
   return 0;
}
int db1_session_ir_history_load(const char *session, const char *kind, const char *actor,
                                const char *active, char **out)
{
   assert(!strcmp(session, "destination") && !strcmp(kind, "primary"));
   assert(!strcmp(actor, "actor") && !strcmp(active, "turn"));
   *out = mode == 1 ? NULL
                    : strdup(mode == 10 ? "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\","
                                          "\"text\":\"prior task\"}]},"
                                          "{\"role\":\"assistant\",\"blocks\":[{\"type\":\"text\","
                                          "\"text\":\"prior original\"}]}]"
                                        : "[]");
   return mode == 1 ? -1 : 0;
}
server_native_runtime_t *server_native_runtime_open_request(const agent_t *selected,
                                                            const request_context_t *request_owner,
                                                            const char *source,
                                                            const aimee_request_t *profile)
{
   assert(request_owner == &owner && !strcmp(source, "transport"));
   assert(!strcmp(selected->model, "selected-model") &&
          profile->n_messages == (mode == 10 ? 2 : 0));
   assert(!strcmp(selected->provider, "chatgpt"));
   assert(profile->n_tools == 1 && profile->tools[0].kind == AIMEE_TOOL_NAMESPACE);
   assert(!strcmp(profile->tools[0].name, "aimee"));
   assert(profile->tools[0].n_children == 1 &&
          !strcmp(profile->tools[0].children[0].name, "read_file"));
   opened++;
   return &runtime;
}
const char *server_native_runtime_url(const server_native_runtime_t *r)
{
   assert(r == &runtime);
   return "http://127.0.0.1:45678";
}
const char *server_native_runtime_token(const server_native_runtime_t *r)
{
   assert(r == &runtime);
   return "local-capability-fixture";
}
const char *server_native_runtime_binding(const server_native_runtime_t *r)
{
   assert(r == &runtime);
   return digest;
}
int server_native_runtime_cache_scope(const server_native_runtime_t *r,
                                      const server_native_execution_profile_t *profile,
                                      char scope[65])
{
   assert(r == &runtime && !strcmp(profile->sdk, "codex"));
   assert(strlen(profile->build_digest) == 64 && strcmp(profile->build_digest, digest));
   assert(!strcmp(profile->launch_digest, digest) && !strcmp(profile->workspace_digest, digest));
   snprintf(scope, 65, "%s", digest);
   return 0;
}
void server_native_runtime_quiesce(server_native_runtime_t *r)
{
   assert(r == &runtime && child_closed);
   joined = 1;
}
void server_native_runtime_close(server_native_runtime_t *r)
{
   if (r)
   {
      assert(r == &runtime);
      assert(!sdk_calls || child_closed);
      joined = 1;
   }
}
int server_native_runtime_result(const server_native_runtime_t *r, aimee_response_t *out)
{
   assert(r == &runtime && joined && child_closed);
   if (mode == 3)
      return -1;
   out->role = strdup("assistant");
   out->n_content = 1;
   out->content = calloc(1, sizeof *out->content);
   out->content[0].type = AIMEE_BLK_TEXT;
   out->content[0].text = strdup("ORIGINAL canonical output");
   out->usage_in = 73;
   out->usage_out = 29;
   out->usage_cache_read = 11;
   out->usage_cache_write = 7;
   out->stop_reason = mode == 4 ? AIMEE_STOP_TOOL_USE : AIMEE_STOP_END_TURN;
   return 0;
}
int server_native_runtime_execute_tool(const aimee_block_t *call, aimee_block_t *result, void *ctx)
{
   (void)call;
   (void)result;
   assert(ctx == &runtime);
   return -1;
}
server_native_cache_t *server_native_cache_begin(const char *session, const char *actor,
                                                 const char *turn, const char *harness,
                                                 const char *scope, const aimee_request_t *profile)
{
   assert(!strcmp(session, "destination") && !strcmp(actor, "actor") && !strcmp(turn, "turn"));
   assert(!strcmp(harness, "codex") && !strcmp(scope, digest));
   assert(profile->n_messages == (mode == 10 ? 2 : 0));
   began++;
   return &cache;
}
int server_native_cache_history_offset(const server_native_cache_t *c)
{
   assert(c == &cache);
   return mode == 10 ? 1 : 0;
}
const char *server_native_cache_resume(const server_native_cache_t *c)
{
   assert(c == &cache);
   return "private-resume";
}
int server_native_cache_complete(server_native_cache_t *c, const char *id)
{
   assert(c == &cache && terminal_committed && joined && child_closed);
   assert(!strcmp(id, "native-result"));
   completed++;
   return mode == 7 ? -1 : 0;
}
int server_native_cache_abort(server_native_cache_t *c)
{
   assert(c == &cache && joined && child_closed);
   aborted++;
   return 0;
}
void server_native_cache_close(server_native_cache_t *c)
{
   assert(c == &cache);
   closed++;
}
static int sdk_peer(const cli_codex_host_launch_t *launch)
{
   assert(launch && launch->ready && launch->executable_fd >= 0 && launch->cwd_fd >= 0);
   assert(strcmp(launch->cwd, host_workspace));
   snprintf(private_home, sizeof private_home, "%s", launch->home);
   child_executable = launch->executable_fd;
   child_directory = launch->cwd_fd;
   struct stat st;
   assert(!fstat(child_executable, &st) && S_ISREG(st.st_mode));
   assert(!fstat(child_directory, &st) && S_ISDIR(st.st_mode));
   char path[4200];
   snprintf(path, sizeof path, "%s/.codex/auth.json", private_home);
   assert(access(path, F_OK));
   snprintf(path, sizeof path, "%s/.codex/config.toml", private_home);
   assert(!stat(path, &st) && (st.st_mode & 0777) == 0600);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(launch->expected_config, "model")),
                  "selected-model"));
   for (int i = 0; launch->environment[i]; i++)
   {
      assert(!strstr(launch->environment[i], "PARENT_SECRET"));
      assert(!strstr(launch->environment[i], "ANTHROPIC_API_KEY="));
      assert(!strstr(launch->environment[i], "AIMEE_"));
   }
   cli_codex_host_replay_t replay = {0};
   assert(!launch->ready(digest, &replay, launch->context));
   ready_calls++;
   assert(replay.history && replay.history->n_messages == (mode == 10 ? 1 : 0) &&
          !strcmp(replay.resume_id, "private-resume"));
   if (mode == 10)
   {
      assert(!strcmp(replay.history->messages[0].role, "assistant"));
      assert(!strcmp(replay.history->messages[0].blocks[0].text, "prior original"));
   }
   if (mode == 8)
      assert(launch->ready(digest, &replay, launch->context) < 0);
   sdk_calls++;
   child_closed = 1;
   return mode == 2 ? -1 : 0;
}
static int sdk_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   assert(!cfg->home && !cfg->history && !cfg->resume_id && cfg->codex_host);
   assert(cfg->tool_catalog && cfg->tool_call == server_native_runtime_execute_tool);
   int rc = sdk_peer(cfg->codex_host);
   assert(!cfg->bind_session("native-result", cfg->bind_context));
   out->response = strdup("SDK REWRITE MUST NOT BECOME CANONICAL");
   out->success = !rc;
   return rc;
}
const provider_cli_adapter_t codex_provider_cli_adapter = {.cli_kind = "codex",
                                                           .execute = sdk_execute};
int cli_codex_chat_stream(const cli_codex_chat_request_t *request, cli_codex_chat_event_cb cb,
                          void *context, cli_codex_chat_result_t *out)
{
   (void)cb;
   (void)context;
   assert(!request->home && !request->thread_id && !request->history && request->host_launch);
   int rc = sdk_peer(request->host_launch);
   snprintf(out->thread_id, sizeof out->thread_id, "native-result");
   return rc;
}
static void reset(int next)
{
   assert(!terminal && !retained);
   mode = next;
   opened = joined = child_closed = began = completed = aborted = closed = 0;
   terminal_committed = sdk_calls = restored = ready_calls = rejected_terminal = 0;
   child_executable = child_directory = -1;
   private_home[0] = 0;
   snprintf(parent_role, sizeof parent_role, "parent");
   snprintf(owner.vault_principal, sizeof owner.vault_principal, "actor");
   aimee_ir_session_begin_task();
}
static void finish(int success)
{
   assert(terminal && !completed && !closed);
   server_session_terminal_action_fn action = terminal;
   terminal = NULL;
   terminal_committed = 1;
   action(success, terminal_resource);
   terminal_resource = NULL;
}
int main(void)
{
   char temporary[4096];
   assert(snprintf(temporary, sizeof temporary, "%s/native-factory-XXXXXX", platform_tmpdir()) <
          (int)sizeof temporary);
   assert(mkdtemp(temporary));
   snprintf(host_workspace, sizeof host_workspace, "%s", temporary);
   run_cmd_set_cwd(host_workspace);
   setenv("AIMEE_OAUTH_RUNTIME_DIR", temporary, 1);
   setenv("PARENT_SECRET", "do-not-inherit", 1);
   agent_t selected = {0};
   snprintf(selected.cli_kind, sizeof selected.cli_kind, "codex");
   snprintf(selected.cli_cmd, sizeof selected.cli_cmd, "/bin/true");
   snprintf(selected.provider, sizeof selected.provider, "chatgpt");
   snprintf(selected.model, sizeof selected.model, "selected-model");
   provider_cli_cfg_t cfg = {.agent = &selected, .role = "worker", .user_prompt = "current task"};
#if defined(__linux__)
   for (int scenario = 0; scenario <= 10; scenario++)
   {
      reset(scenario);
      agent_result_t out = {0};
      if (scenario == 9)
         rejected_terminal = 1;
      int rc = provider_cli_adapter_host_execute(&cfg, &out);
      int success =
          scenario == 0 || scenario == 6 || scenario == 7 || scenario == 8 || scenario == 10;
      assert((rc == 0) == success && out.success == success && !retained);
      assert(restored == 1 && !strcmp(parent_role, "parent"));
      if (private_home[0])
      {
         assert(access(private_home, F_OK));
         assert(fcntl(child_executable, F_GETFD) == -1);
         assert(fcntl(child_directory, F_GETFD) == -1);
      }
      if (success)
      {
         assert(!strcmp(out.response, "ORIGINAL canonical output"));
         assert(out.prompt_tokens == 73 && out.completion_tokens == 29);
         assert(out.cache_read_tokens == 11 && out.cache_write_tokens == 7);
         assert(terminal && !completed && !aborted && !closed);
         finish(scenario != 6);
         assert(completed == (scenario == 6 ? 0 : 1));
         assert(aborted == ((scenario == 6 || scenario == 7) ? 1 : 0) && closed == 1);
      }
      else
      {
         assert(!terminal && !completed);
         assert(!began || (aborted == 1 && closed == 1));
      }
      free(out.response);
   }
   reset(0);
   snprintf(selected.provider, sizeof selected.provider, "codex-cli");
   snprintf(selected.auth_type, sizeof selected.auth_type, "codex-oauth");
   agent_result_t alias = {0};
   assert(!provider_cli_adapter_host_execute(&cfg, &alias));
   assert(!strcmp(selected.provider, "codex-cli"));
   free(alias.response);
   finish(1);
   snprintf(selected.provider, sizeof selected.provider, "chatgpt");
   selected.auth_type[0] = 0;
   reset(0);
   snprintf(owner.vault_principal, sizeof owner.vault_principal, "wrong-actor");
   agent_result_t denied = {0};
   assert(provider_cli_adapter_host_execute(&cfg, &denied) < 0);
   assert(!opened && !retained && !sdk_calls && !restored);
   reset(0);
   cli_codex_chat_request_t request = {.user_prompt = "primary task"};
   cli_codex_chat_result_t result = {0};
   assert(!server_native_codex_chat_stream(&selected, &request, NULL, NULL, &result));
   assert(result.prompt_tokens == 73 && result.completion_tokens == 29 && terminal);
   assert(!retained && restored == 1 && access(private_home, F_OK));
   finish(1);
   assert(completed == 1 && closed == 1);
   cfg.agent = NULL;
   assert(provider_cli_adapter_host_execute(&cfg, &denied) == 1);
#else
   reset(0);
   agent_result_t refused = {0};
   assert(provider_cli_adapter_host_execute(&cfg, &refused) < 0);
   assert(!sdk_calls && !retained && restored == 1 && !terminal);
   free(refused.response);
#endif
   run_cmd_set_cwd(NULL);
   cli_session_t cleanup = {.iso_home = strdup(temporary)};
   cli_session_destroy(&cleanup);
   assert(access(temporary, F_OK));
   puts("native factory canonical replay, original result, isolation and terminal ordering passed");
   return 0;
}
