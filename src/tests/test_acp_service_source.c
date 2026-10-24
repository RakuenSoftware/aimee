/* Actual signed service source, credential/request TLS, capture/control owners,
 * ACP constructor and descriptor-launched protocol child. Vault/DB/registry/
 * workspace/catalog are explicit fixture services; no positive runtime handoff
 * is substituted and no provider/SDK/model is invoked. */
#define main                      existing_source_fixture_main
#define db1_server_session_create source_fixture_session_create
#include "test_native_delegate_source.c"
#undef db1_server_session_create
#undef main
#include "server_acp_execution.h"
#include "agent_exec.h"
#include "server_session_capture.h"
#include "server_session_control.h"
#include "server_learning_outcome.h"
#include "modules/workspace/workspace_provider.h"
#include <aimee/tools/agent_tools.h>
#include <sys/wait.h>
#include "cli_proxy.h"
#include "server_http_internal.h"

extern const provider_cli_adapter_t acp_provider_cli_adapter;
static cJSON *capture_rows;
static char destination[128], active_turn[128], child_marker[1024], program[4096];
static agent_tools_scope_snapshot_t tools_scope;
static workspace_provider_t shared_workspace = {.kind = WS_PROVIDER_SHARED};
int db1_server_session_create(const char *id, const char *client, const char *principal)
{
   int rc = source_fixture_session_create(id, client, principal);
   if (!rc)
      snprintf(parents[n_parents - 1].created_at, sizeof parents[n_parents - 1].created_at,
               "fixture-incarnation");
   return rc;
}
const char *config_default_dir(void)
{
   return root;
}
int agent_registry_find(const char *name, agent_t *out)
{
   if (strcmp(name, registered.name))
      return -1;
   *out = registered;
   return 0;
}
const provider_cli_adapter_t *provider_cli_adapter_get(const char *name)
{
   assert(!strcmp(name, "acp"));
   return &acp_provider_cli_adapter;
}
cli_proxy_t *cli_proxy_start_scoped(unsigned port, const char *token, const char *sid,
                                    unsigned *bound, cli_proxy_route_fn route,
                                    cli_proxy_forward_fn forward, void *context)
{
   (void)port;
   (void)token;
   (void)sid;
   (void)bound;
   (void)route;
   (void)forward;
   (void)context;
   abort();
}
/* This deliberately tool-free role cannot create a proxy or CLI action.
 * Abort peers make any accidental newly reached capability fail this test. */
cli_proxy_t *cli_proxy_start_authorized(unsigned port, const char *token, const char *sid,
                                        unsigned *bound, cli_proxy_route_fn route,
                                        cli_proxy_authorize_fn authorize,
                                        cli_proxy_forward_fn forward, void *context)
{
   (void)port;
   (void)token;
   (void)sid;
   (void)bound;
   (void)route;
   (void)authorize;
   (void)forward;
   (void)context;
   abort();
}
int rh_cli_manifest(const route_req_t *request, char *response, int capacity)
{
   (void)request;
   (void)response;
   (void)capacity;
   abort();
}
int policy_action_scope_push(policy_action_scope_t **out)
{
   (void)out;
   abort();
}
int policy_action_scope_pop(policy_action_scope_t *scope)
{
   (void)scope;
   abort();
}
int agent_tools_effect_scope_push(agent_tools_effect_scope_t **out)
{
   (void)out;
   abort();
}
int agent_tools_effect_scope_pop(agent_tools_effect_scope_t *scope)
{
   (void)scope;
   abort();
}
agent_tools_bash_scope_t agent_tools_bind_bash_scope(const agent_tools_bash_scope_t *scope)
{
   (void)scope;
   abort();
}
char *policy_annotate_indexed(const char *tool, const char *arguments, const char *attempt,
                              char *result)
{
   (void)tool;
   (void)arguments;
   (void)attempt;
   (void)result;
   abort();
}
void cli_proxy_deactivate_hold_port(cli_proxy_t *p)
{
   assert(!p);
}
void cli_proxy_stop(cli_proxy_t *p)
{
   assert(!p);
}
int wire_fence_external_backend(void)
{
   return 0;
}
const workspace_provider_t *workspace_provider_active(void)
{
   return &shared_workspace;
}
void agent_tools_scope_bookmark(agent_tools_scope_snapshot_t *out)
{
   *out = tools_scope;
}
int agent_tools_scope_capture_owned(agent_tools_scope_snapshot_t *out)
{
   *out = tools_scope;
   out->isolated = 1;
   return 0;
}
void agent_tools_scope_restore(const agent_tools_scope_snapshot_t *scope)
{
   tools_scope = *scope;
}
void agent_tools_scope_destroy_owned(agent_tools_scope_snapshot_t *scope)
{
   memset(scope, 0, sizeof *scope);
}
int agent_tools_scope_isolated(void)
{
   return tools_scope.isolated;
}
const char *agent_tools_dispatch_role(void)
{
   return tools_scope.role[0] ? tools_scope.role : NULL;
}
int agent_tools_catalog(aimee_wire_t wire, aimee_request_t *out)
{
   assert(wire == AIMEE_WIRE_OPENAI_CHAT);
   memset(out, 0, sizeof *out);
   return 0;
}
int agent_tools_catalog_filter(aimee_request_t *out, const char *role)
{
   assert(!out->n_tools && role && !strcmp(role, "reviewer"));
   return 0;
}
agent_tool_permission_t agent_tools_permission_check(const char *name, const cJSON *args)
{
   (void)args;
   assert(!strcmp(name, "read_file") || !strcmp(name, "write_file"));
   return AGENT_TOOL_PERMISSION_ROLE;
}
char *agent_tools_permission_error(agent_tool_permission_t permission)
{
   (void)permission;
   abort();
}
int agent_tools_execution_definition(const char *name, aimee_tool_t *out)
{
   (void)name;
   (void)out;
   abort();
}
void agent_tools_set_effect_authorized(int yes)
{
   (void)yes;
}
char *dispatch_tool_call_ir_ctx_status(const aimee_tool_t *t, const char *a, int m, int *e)
{
   (void)t;
   (void)a;
   (void)m;
   (void)e;
   abort();
}
char *dispatch_tool_file_resource_ctx_status(const aimee_tool_t *t, const char *a, int m,
                                             agent_tool_file_resource_fn f, void *c, int *e)
{
   (void)t;
   (void)a;
   (void)m;
   (void)f;
   (void)c;
   (void)e;
   abort();
}
const char *agent_tools_validate_file_resource(const char *p, int w)
{
   (void)p;
   (void)w;
   abort();
}
int tool_validate_ir(const aimee_tool_t *t, const char *a, char *e, size_t n)
{
   (void)t;
   (void)a;
   (void)e;
   (void)n;
   abort();
}
int directive_check_tool(const char *t, const char *a, char *e, size_t n)
{
   (void)t;
   (void)a;
   (void)e;
   (void)n;
   abort();
}
int tool_policy_metadata(const char *t, aimee_tool_metadata_t *m, char *a, size_t n, char *e,
                         size_t z)
{
   (void)t;
   (void)m;
   (void)a;
   (void)n;
   (void)e;
   (void)z;
   abort();
}
int policy_check_tool_metadata_attempt(const char *t, const aimee_tool_metadata_t *m, const char *f,
                                       const char *a, const char *i, char *e, size_t z)
{
   (void)t;
   (void)m;
   (void)f;
   (void)a;
   (void)i;
   (void)e;
   (void)z;
   abort();
}
int db1_session_ir_running_history_load(const char *sid, const char *actor, const char *turn,
                                        char **out)
{
   assert(!strcmp(sid, destination) && !strcmp(actor, VAULT_SERVER_PRINCIPAL) &&
          !strcmp(turn, active_turn));
   cJSON *history = cJSON_CreateArray();
   assert(cJSON_AddItemToArray(history, aimee_ir_message_json(&message)));
   *out = cJSON_PrintUnformatted(history);
   cJSON_Delete(history);
   return *out ? 0 : -1;
}
int db1_session_ir_append(const char *sid, const char *id, const char *turn, const char *actor,
                          const char *payload, char sequence[32])
{
   assert(!strcmp(sid, destination) && !strcmp(actor, VAULT_SERVER_PRINCIPAL) &&
          !strcmp(turn, active_turn));
   cJSON *row = cJSON_CreateObject();
   assert(row);
   snprintf(sequence, 32, "%d", cJSON_GetArraySize(capture_rows) + 1);
   assert(cJSON_AddStringToObject(row, "sequence", sequence) &&
          cJSON_AddStringToObject(row, "session_id", sid) &&
          cJSON_AddStringToObject(row, "turn_id", turn) &&
          cJSON_AddStringToObject(row, "actor", actor) && cJSON_AddStringToObject(row, "id", id));
   assert(cJSON_AddItemToObject(row, "event", aimee_ir_json_parse_frame(payload, strlen(payload))));
   assert(cJSON_AddItemToArray(capture_rows, row));
   return 0;
}
int db1_session_ir_append_verified(const char *s, const char *i, const char *t, const char *a,
                                   const char *p, char q[32])
{
   (void)s;
   (void)i;
   (void)t;
   (void)a;
   (void)p;
   (void)q;
   abort();
}
int db1_session_native_source_append(const char *s, const char *i, const char *t, const char *a,
                                     const char *p, char q[32])
{
   (void)s;
   (void)i;
   (void)t;
   (void)a;
   (void)p;
   (void)q;
   abort();
}
char *db1_session_ir_page(const char *s, const char *after, int limit, const char *a)
{
   assert(!strcmp(s, destination) && !strcmp(a, VAULT_SERVER_PRINCIPAL));
   cJSON *page = cJSON_CreateArray();
   int start = atoi(after);
   for (int i = start; i < cJSON_GetArraySize(capture_rows) && limit-- > 0; i++)
      assert(cJSON_AddItemToArray(page, cJSON_Duplicate(cJSON_GetArrayItem(capture_rows, i), 1)));
   char *out = cJSON_PrintUnformatted(page);
   cJSON_Delete(page);
   return out;
}
void server_learning_observe_terminal(const char *s, const char *t, const char *q, const char *id,
                                      const char *a, const cJSON *e)
{
   (void)q;
   (void)id;
   (void)e;
   assert(!strcmp(s, destination) && !strcmp(t, active_turn) && !strcmp(a, VAULT_SERVER_PRINCIPAL));
}
static cJSON *read_rpc(void)
{
   char *line = NULL;
   size_t cap = 0;
   assert(getline(&line, &cap, stdin) > 0);
   cJSON *v = aimee_ir_json_parse_frame(line, strlen(line));
   free(line);
   assert(v);
   return v;
}
static void reply(const cJSON *rpc, const char *json)
{
   cJSON *out = cJSON_CreateObject();
   assert(cJSON_AddStringToObject(out, "jsonrpc", "2.0"));
   assert(cJSON_AddItemToObject(out, "id",
                                cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(rpc, "id"), 1)));
   assert(cJSON_AddItemToObject(out, "result", cJSON_Parse(json)));
   char *wire = cJSON_PrintUnformatted(out);
   assert(wire);
   assert(printf("%s\n", wire) > 0 && !fflush(stdout));
   free(wire);
   cJSON_Delete(out);
}
void server_learning_observe_child(const char *s, const char *t, const char *q, const char *id,
                                   const char *a, const cJSON *e)
{
   (void)s;
   (void)t;
   (void)q;
   (void)id;
   (void)a;
   (void)e;
   abort();
}
static int child(const char *marker)
{
   int fd = open(marker, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
   assert(fd >= 0 && write(fd, "x", 1) == 1 && !close(fd));
   cJSON *rpc = read_rpc();
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rpc, "method")),
                  "initialize"));
   reply(rpc, "{\"protocolVersion\":1,\"agentCapabilities\":{},\"authMethods\":[]}");
   cJSON_Delete(rpc);
   rpc = read_rpc();
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rpc, "method")),
                  "session/new"));
   const cJSON *params = cJSON_GetObjectItemCaseSensitive(rpc, "params");
   assert(!cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(params, "mcpServers")));
   reply(rpc, "{\"sessionId\":\"service-acp-session\"}");
   cJSON_Delete(rpc);
   rpc = read_rpc();
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rpc, "method")),
                  "session/prompt"));
   assert(puts("{\"jsonrpc\":\"2.0\",\"method\":\"session/"
               "update\",\"params\":{\"sessionId\":\"service-acp-session\",\"update\":{"
               "\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":\"text\",\"text\":"
               "\"ACP signed service source complete\"}}}}") > 0 &&
          !fflush(stdout));
   reply(rpc, "{\"stopReason\":\"end_turn\"}");
   cJSON_Delete(rpc);
   return 0;
}
static void setup_registry(void)
{
   char path[512];
   assert(snprintf(path, sizeof path, "%s/acp-service-source-XXXXXX", platform_tmpdir()) <
          (int)sizeof path);
   assert(mkdtemp(path));
   snprintf(root, sizeof root, "%s", path);
   assert(snprintf(worktree, sizeof worktree, "%s/child", root) < (int)sizeof worktree);
   assert(!mkdir(worktree, 0700));
   ssize_t n = readlink("/proc/self/exe", program, sizeof program - 1);
   assert(n > 0);
   program[n] = 0;
   assert(snprintf(child_marker, sizeof child_marker, "%s/child-launches", root) <
          (int)sizeof child_marker);
   registered = (agent_t){
       .enabled = 1, .tools_enabled = 1, .max_turns = 0, .max_tokens = 0, .timeout_ms = 5000};
   snprintf(registered.name, sizeof registered.name, "native-fixture");
   snprintf(registered.model, sizeof registered.model, "fixture-model");
   snprintf(registered.cli_kind, sizeof registered.cli_kind, "acp");
   snprintf(registered.auth_type, sizeof registered.auth_type, "none");
   assert(snprintf(registered.cli_cmd, sizeof registered.cli_cmd, "%s --source-child %s", program,
                   child_marker) < (int)sizeof registered.cli_cmd);
   service_owner_enabled = rule_count = 1;
   snprintf(trigger_rule.source, sizeof trigger_rule.source, "watch-dir");
   snprintf(trigger_rule.event, sizeof trigger_rule.event, "docs/proposals/pending");
   snprintf(trigger_rule.workspace, sizeof trigger_rule.workspace, "%s", root);
   snprintf(trigger_rule.schedule, sizeof trigger_rule.schedule, "HEAD");
   snprintf(trigger_rule.pipeline_template, sizeof trigger_rule.pipeline_template,
            "configured-workflow");
   snprintf(trigger_rule.mode, sizeof trigger_rule.mode, "autonomous");
}
int main(int argc, char **argv)
{
   if (argc == 3 && !strcmp(argv[1], "--source-child"))
      return child(argv[2]);
   setup_registry();
   run_cmd_set_cwd(root);
   session_id_set_override("independent-outer-session");
   cJSON *registration = trigger_registration("config");
   char handle[65];
   assert(!server_native_delegate_source_trigger(
       registration, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", handle,
       error, sizeof error));
   for (int i = 0; i < n_parents; i++)
      snprintf(parents[i].created_at, sizeof parents[i].created_at, "fixture-incarnation");
   snprintf(item.work_item_id, sizeof item.work_item_id, "work-acp-source");
   snprintf(item.repo, sizeof item.repo, "%s", root);
   snprintf(item.worktree, sizeof item.worktree, "%s", worktree);
   snprintf(item.execution_scope, sizeof item.execution_scope, "%s", handle);
   snprintf(item.state, sizeof item.state, "active");
   server_native_delegate_source_scope_t *scope = NULL;
   assert(!server_native_delegate_source_bind(handle, 76, item.work_item_id, "reviewer",
                                              "native-fixture", worktree, &request, 1, 0, 5000,
                                              &scope, &config, &selected, error, sizeof error));
   assert(!strcmp(request_context_get()->vault_principal, VAULT_SERVER_PRINCIPAL) &&
          request_context_get()->vault_transport == ATTEST_NONE && !request_context_get()->trusted);
   assert(!request_context_get()
               ->request_id[0]); /* Restored source never imports an HTTP correlation. */
   assert(!strcmp(session_id(), "independent-outer-session"));
   assert(!server_native_delegate_source_admit(scope, "engineer", &request, error, sizeof error));
   snprintf(destination, sizeof destination, "service-acp-child");
   snprintf(active_turn, sizeof active_turn, "service-acp-turn");
   assert(!db1_server_session_create(destination, "delegate", VAULT_SERVER_PRINCIPAL));
   const char *source_session = agent_get_request_session();
   assert(source_session && source_session[0]);
   server_control_scope_t *controls = server_control_scope_open_execution(
       destination, active_turn, VAULT_SERVER_PRINCIPAL, source_session);
   assert(controls);
   server_control_scope_disable_replies(controls);
   assert(!server_control_scope_bind(controls));
   server_session_capture_t *capture = server_session_capture_open(
       destination, active_turn, VAULT_SERVER_PRINCIPAL, source_session);
   assert(capture && !server_session_capture_bind(capture));
   capture_rows = cJSON_CreateArray();
   aimee_ir_session_begin_task();
   aimee_ir_session_set_sink(server_session_capture_sink, capture);
   aimee_session_event_t initial = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "actual-user-source", .message = &message};
   assert(!aimee_ir_session_emit(&initial));
   provider_cli_cfg_t cfg = {.agent = &selected,
                             .role = "reviewer",
                             .cwd = worktree,
                             .canonical_request = &request,
                             .user_prompt = block.text};
   agent_result_t result = {0};
   int rc = server_acp_execute_owned(&cfg, &result);
   if (rc)
      fprintf(stderr,
              "actual signed source ACP constructor rc=%d error=%s request_id_empty=%d "
              "outer_session_retained=%d\n",
              rc, result.error, !request_context_get()->request_id[0],
              !strcmp(session_id(), "independent-outer-session"));
   assert(!rc && result.success && !aimee_ir_response_complete_valid(&result.canonical_response));
   assert(result.canonical_response.n_content == 1 &&
          !strcmp(result.canonical_response.content[0].text, "ACP signed service source complete"));
   assert(!request_context_get()->request_id[0] &&
          !strcmp(session_id(), "independent-outer-session"));
   struct stat marker;
   assert(!stat(child_marker, &marker) && marker.st_size == 1);
   aimee_request_t profile = request;
   profile.model = selected.model;
   profile.mutated = 1;
   assert(!server_native_delegate_source_runtime_consume(&selected, request_context_get(), &profile,
                                                         source_session));
   assert(server_native_delegate_source_runtime_prepare("reviewer", &selected, &selected, &request,
                                                        &profile, source_session));
   assert(!stat(child_marker, &marker) && marker.st_size == 1);
   agent_result_clear_response(&result);
   aimee_session_event_t terminal = {.kind = AIMEE_SESSION_EXECUTION,
                                     .execution_status = "completed"};
   assert(!aimee_ir_session_emit(&terminal));
   aimee_ir_session_set_sink(NULL, NULL);
   assert(server_session_capture_bind(NULL) == capture);
   server_session_capture_close(capture);
   assert(server_control_scope_bind(NULL) == controls);
   server_control_scope_close(controls);
   server_native_delegate_source_unbind(scope);
   assert(!server_native_delegate_source_revoke(handle));
   cJSON_Delete(registration);
   cJSON_Delete(capture_rows);
   for (int i = 0; i < n_vault; i++)
      free(vault[i].wire);
   unlink(child_marker);
   assert(!rmdir(worktree) && !rmdir(root));
   session_id_set_override(NULL);
   request_context_clear();
   puts("actual signed NONE source -> ACP constructor/child/canonical result, consumed once: PASS");
   return 0;
}
