#include "server_native_runtime.h"
#include "modules/workspace/workspace_turn.h"
#include "computer_use.h"
#include <aimee/translation/aimee_backend.h>
#include "agent_exec.h"
#include "agent_coord.h"
#include "server_session_question.h"
#include <aimee/tools/agent_tools.h>
#include "agent_source_authority.h"
#include "agent_tasks.h"
#include "server_delegate_scope.h"
#include "server_session_control.h"
#include "server_session_capture.h"
#include "server_learning_outcome.h"
#include "modules/workspace/workspace_provider.h"
#include "util.h"
#include "server_native_replay.h"
#include <aimee/translation/aimee_frontend.h>
#include "server_http.h"
#include "server_http_identity.h"
#include "db1_client/server_sessions.h"
#include <aimee/core/connection/socket.h>
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local char tool_role[64], tool_cwd[MAX_PATH_LEN], tool_session[128];
static _Thread_local int job_id, effect_authorized, scope_admitted;
static void restored_probe(void);
static _Thread_local server_delegate_scope_t delegate_scope;
static _Thread_local const workspace_provider_t *workspace;
static server_control_scope_t *alice_controls;
static _Atomic(server_session_capture_t *) expected_capture;
static atomic_int reject_proof, reject_durable, snapshot_fault, destination_invalid;
static cJSON *captured_rows;
static pthread_mutex_t capture_lock = PTHREAD_MUTEX_INITIALIZER;
static workspace_provider_t alice_workspace = {.kind = WS_PROVIDER_SHARED};
static workspace_provider_t bob_workspace = {.kind = WS_PROVIDER_SHARED};
const char *agent_tools_dispatch_role(void)
{
   return tool_role[0] ? tool_role : NULL;
}
void agent_tools_set_dispatch_role(const char *role)
{
   snprintf(tool_role, sizeof tool_role, "%s", role ? role : "");
}
static _Thread_local char *const *shell_environment;
char *const *agent_tools_bind_bash_environment(char *const environment[])
{
   char *const *previous = shell_environment;
   shell_environment = environment;
   return previous;
}
void agent_tools_set_effect_authorized(int authorized)
{
   effect_authorized = authorized;
}
const char *run_cmd_get_cwd(void)
{
   return tool_cwd[0] ? tool_cwd : NULL;
}
void run_cmd_set_cwd(const char *cwd)
{
   snprintf(tool_cwd, sizeof tool_cwd, "%s", cwd ? cwd : "");
}
const char *session_id(void)
{
   return tool_session;
}
int session_id_override_active(void)
{
   return tool_session[0] != 0;
}
void session_id_set_override(const char *sid)
{
   snprintf(tool_session, sizeof tool_session, "%s", sid ? sid : "");
   if (scope_admitted && !tool_session[0])
   {
      restored_probe();
      scope_admitted = 0;
   }
}
void agent_set_durable_job(int id)
{
   job_id = id;
}
int agent_get_durable_job_id(void)
{
   return job_id;
}
/* Workspace ownership is an explicit peer mock here; the actual descriptor,
 * cross-thread restoration and replacement tests live in test_workspace_turn. */
struct workspace_turn_mirror_lease
{
   int marker;
};
static workspace_turn_mirror_lease_t fixture_mirror;
static _Thread_local const workspace_turn_mirror_lease_t *current_mirror;
int workspace_turn_mirror_capture(const char *cwd, workspace_turn_mirror_lease_t **out)
{
   (void)cwd;
   *out = &fixture_mirror;
   return 0;
}
int workspace_turn_mirror_valid(const workspace_turn_mirror_lease_t *lease, const char *cwd)
{
   (void)cwd;
   return lease == &fixture_mirror;
}
const workspace_turn_mirror_lease_t *
workspace_turn_mirror_bind(const workspace_turn_mirror_lease_t *lease)
{
   const workspace_turn_mirror_lease_t *previous = current_mirror;
   current_mirror = lease;
   return previous;
}
void workspace_turn_mirror_release(workspace_turn_mirror_lease_t *lease)
{
   assert(!lease || lease == &fixture_mirror);
}
const workspace_provider_t *workspace_provider_active(void)
{
   return workspace;
}
void workspace_provider_set_active(const workspace_provider_t *provider)
{
   workspace = provider;
}
void server_delegate_scope_capture(server_delegate_scope_t *out)
{
   *out = delegate_scope;
}
void server_delegate_scope_restore(const server_delegate_scope_t *scope)
{
   delegate_scope = *scope;
}
char *normalize_path(const char *path, const char *cwd, char *out, size_t capacity)
{
   (void)cwd;
   snprintf(out, capacity, "%s", path);
   return out;
}

/* This fixture has the real built-in definition owner and no attached remote
 * tool providers. They do not silently become extra registry authorities. */
void computer_use_policy_from_config(computer_use_policy_t *out)
{
   memset(out, 0, sizeof *out);
}
int computer_use_is_tool_name(const char *name)
{
   (void)name;
   return 0;
}
cJSON *mcp_client_registry_build_namespaced_tools(int timeout)
{
   (void)timeout;
   return NULL;
}
char *kb_client_tool_registry_snapshot_json(void)
{
   return NULL;
}

static void owner_tools(int alice)
{
   static _Thread_local char deny[64];
   static _Thread_local const char *denied[1];
   strcpy(deny, alice ? "git_push" : "write_file");
   denied[0] = deny;
   agent_tools_set_dispatch_role(alice ? "reviewer" : "");
   agent_tools_set_active_toolset(alice ? "review_indexed" : NULL);
   agent_tools_write_capable_set(!alice);
   agent_tools_shell_set(!alice);
   agent_tools_knowledge_write_set(!alice);
   agent_tools_denied_set(denied, 1);
   agent_tools_parent_write_guard_set(alice ? "/parent/alice" : "/parent/bob",
                                      alice ? "/work/alice" : "/work/bob");
   session_id_set_override(alice ? "session-a" : "session-b");
   agent_tools_begin_turn(alice ? 7 : 9);
   agent_tools_set_snap_id(alice ? 71 : 91);
   run_cmd_set_cwd(alice ? "/work/alice" : "/work/bob");
   workspace = alice ? &alice_workspace : &bob_workspace;
   job_id = alice ? 11 : 22;
   delegate_scope = (server_delegate_scope_t){.depth = alice ? 2 : 0};
   if (alice)
      strcpy(delegate_scope.parent, "parent-alice");
   delegate_scope.mailbox = alice ? &alice_workspace : NULL;
   if (alice)
      agent_source_authority_tls_set(1, "/source/alice", "src/alice.c\n");
   else
   {
      agent_source_authority_snapshot_t absent = {0};
      agent_source_authority_tls_restore(&absent);
   }
}

static _Thread_local agent_request_creds_t current_credentials;
static _Thread_local const agent_t *current_model;
static _Thread_local atomic_int *current_cancel;
static _Thread_local const agent_request_cancel_scope_t *current_cancel_scope;
static _Thread_local char identity[VAULT_PRINCIPAL_MAX];
static _Thread_local int protocol_captured;
static pthread_mutex_t ids_mutex = PTHREAD_MUTEX_INITIALIZER;
static char seen_ids[128][64];
static int seen_count;
static void probe_replay_scope(const char *path);
static atomic_int source_missing, source_tampered, source_recreated;
static atomic_int calls, cancelled, wait_for_cancel, source_changed, session_removed,
    malformed_source;

static void restored_probe(void)
{
   assert(!current_mirror);
   assert(!current_model && !request_context_get());
   aimee_frontend_state_scope_t guard = aimee_frontend_bind_state_guard(NULL, NULL);
   aimee_frontend_state_scope_t owner = aimee_frontend_bind_request_owner(NULL, NULL);
   assert(!guard.guard && !guard.context && !owner.guard && !owner.context);
   assert(!agent_tools_scope_isolated() && !agent_tools_dispatch_role());
   assert(!agent_tools_active_toolset());
   assert(!agent_tools_tool_denied("git_push") && !agent_tools_tool_denied("write_file"));
   assert(!agent_tools_parent_write_guard_root() && !agent_tools_parent_write_guard_write_root());
   assert(!agent_tools_readonly_delegate_blocks());
   assert(agent_tools_shell_allowed() && agent_tools_knowledge_write_allowed());
   assert(agent_tools_get_turn() == -1 && !agent_tools_get_snap_id());
   assert(!run_cmd_get_cwd() && !session_id_override_active());
   assert(!job_id && !workspace && !effect_authorized && !server_session_capture_current());
   assert(!delegate_scope.depth && !delegate_scope.parent[0] && !delegate_scope.mailbox);
   agent_source_authority_snapshot_t previous = {0};
   assert(!agent_source_authority_tls_capture_checked(&previous));
   assert(!previous.active && !previous.authority && !previous.root[0] && !previous.paths);
   agent_source_authority_snapshot_destroy(&previous);
}

void agent_request_creds_snapshot(agent_request_creds_t *out)
{
   *out = current_credentials;
}
void agent_request_creds_restore(const agent_request_creds_t *value)
{
   current_credentials = *value;
}
void agent_set_request_session(const char *session)
{
   snprintf(current_credentials.session_id, sizeof current_credentials.session_id, "%s", session);
}
void agent_set_request_codex_creds_for(const agent_t *agent, const char *token, const char *account)
{
   assert(!strcmp(agent->registration, "selected-account"));
   snprintf(current_credentials.codex_token, sizeof current_credentials.codex_token, "%s", token);
   snprintf(current_credentials.codex_account_id, sizeof current_credentials.codex_account_id, "%s",
            account);
}
void agent_set_request_cancel(atomic_int *flag)
{
   current_cancel = flag;
}
atomic_int *agent_get_request_cancel(void)
{
   return current_cancel;
}
const agent_request_cancel_scope_t *agent_request_cancel_scope_current(void)
{
   return current_cancel_scope;
}
const agent_request_cancel_scope_t *
agent_request_cancel_scope_bind(const agent_request_cancel_scope_t *scope)
{
   const agent_request_cancel_scope_t *previous = current_cancel_scope;
   current_cancel_scope = scope;
   return previous;
}
int agent_request_cancelled(void)
{
   if (current_cancel && atomic_load(current_cancel))
      return 1;
   for (const agent_request_cancel_scope_t *scope = current_cancel_scope; scope;
        scope = scope->parent)
      if (scope->flag && atomic_load(scope->flag))
         return 1;
   return 0;
}

const char *agent_get_request_vault_principal(void)
{
   const request_context_t *owner = request_context_get();
   return owner ? owner->vault_principal : "";
}
char *safe_strdup(const char *text)
{
   return text ? strdup(text) : NULL;
}
static atomic_int execution_mode, dispatch_count, mcp_execution, cli_shell_probe;
static char revoked_cli_token[65], revoked_cli_url[64], outer_cli_attempt[128];
static void exercise_nested_cli(void);
static _Thread_local char execution_attempt[128];
static void check_execution_owner(const char *name)
{
   assert(current_mirror == &fixture_mirror);
   const request_context_t *owner = request_context_get();
   assert(
       (!strcmp(name, "read_file") || (atomic_load(&cli_shell_probe) && !strcmp(name, "bash"))) &&
       owner && owner->model_execution);
   assert(!strcmp(owner->vault_principal, "webuser:alice") && !strcmp(identity, "webuser:alice"));
   if (atomic_load(&cli_shell_probe) && !strcmp(owner->path, "/v1/native/cli"))
      assert(!strcmp(owner->method, "POST"));
   else
      assert(!strcmp(owner->method, atomic_load(&mcp_execution) ? "POST" : "CALL") &&
             !strcmp(owner->path, atomic_load(&mcp_execution) ? "/v1/mcp/call" : "/native/tool"));
   assert(!(owner->capabilities & (CAP_WORKFLOW_ADMIN | CAP_GRANT_ADMIN | CAP_SHADOW_ADMIN)));
   assert(current_model && !strcmp(current_model->model, "selected-model"));
   assert(agent_tools_scope_isolated() && !strcmp(agent_tools_dispatch_role(), "reviewer"));
   assert(workspace == &alice_workspace && !strcmp(run_cmd_get_cwd(), "/work/alice"));
   assert(server_control_scope_current() == alice_controls);
   assert(server_session_capture_current() == atomic_load(&expected_capture));
}
int tool_validate_ir(const aimee_tool_t *function, const char *arguments, char *error, size_t size)
{
   check_execution_owner(atomic_load(&mcp_execution) &&
                                 !strcmp(function->name, "mcp__aimee__read_file")
                             ? "read_file"
                             : function->name);
   assert(cJSON_IsObject(function->schema) && function->description);
   if (atomic_load(&cli_shell_probe))
      assert(function->name && function->schema);
   else if (atomic_load(&mcp_execution) && strcmp(function->description, "admitted source schema"))
   {
      aimee_tool_t current = {0};
      assert(!agent_tools_execution_definition("read_file", &current));
      assert(cJSON_Compare(current.schema, function->schema, 1));
      assert(!strcmp(current.description, function->description));
      aimee_tool_free(&current);
   }
   else
      assert(!strcmp(function->description, "admitted source schema"));
   assert(strstr(arguments, "123.123456789012345678901234567890"));
   assert(!effect_authorized);
   if (atomic_load(&cli_shell_probe) == 3 && !strcmp(function->name, "read_file") &&
       cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(
           cJSON_GetObjectItemCaseSensitive(
               cJSON_GetObjectItemCaseSensitive(function->schema, "properties"), "amount"),
           "maximum")))
      return -1;
   snprintf(error, size, "%s", "");
   return 0;
}
agent_tool_permission_t agent_tools_permission_check(const char *name, const cJSON *arguments)
{
   check_execution_owner(name);
   assert(cJSON_IsObject(arguments) && !effect_authorized);
   return execution_mode == 1 ? AGENT_TOOL_PERMISSION_ROLE : AGENT_TOOL_PERMISSION_ALLOWED;
}
char *agent_tools_permission_error(agent_tool_permission_t permission)
{
   assert(permission == AGENT_TOOL_PERMISSION_ROLE);
   return strdup("role owner refused execution");
}
int directive_check_tool(const char *name, const char *arguments, char *error, size_t size)
{
   check_execution_owner(name);
   assert(strstr(arguments, "123.123456789012345678901234567890") && !effect_authorized);
   snprintf(error, size, "%s", "");
   return 0;
}
int tool_policy_metadata(const char *name, aimee_tool_metadata_t *metadata, char *effect,
                         size_t effect_size, char *error, size_t error_size)
{
   check_execution_owner(name);
   *metadata = (aimee_tool_metadata_t){.primary = AIMEE_TOOL_POLICY_READ,
                                       .categories = AIMEE_TOOL_POLICY_READ};
   snprintf(effect, effect_size, "%s", "read");
   snprintf(error, error_size, "%s", "");
   return 0;
}
int policy_check_tool_metadata_attempt(const char *name, const aimee_tool_metadata_t *metadata,
                                       const char *effect, const char *arguments,
                                       const char *attempt, char *error, size_t size)
{
   check_execution_owner(name);
   assert(metadata->primary == AIMEE_TOOL_POLICY_READ && !strcmp(effect, "read"));
   assert(strstr(arguments, "123.123456789012345678901234567890") && !effect_authorized);
   if (atomic_load(&cli_shell_probe) && !strcmp(request_context_get()->path, "/v1/native/cli"))
   {
      assert(!strncmp(attempt, "native-cli:", 11) && strlen(attempt) == 108);
      assert(!strncmp(attempt + 11, outer_cli_attempt + 10, 64) && attempt[75] == ':');
   }
   else
   {
      size_t prefix = strlen("native-ir:");
      assert(!strncmp(attempt, "native-ir:", prefix) && strlen(attempt) == prefix + 64);
      assert(strspn(attempt + prefix, "0123456789abcdef") == 64);
   }
   snprintf(execution_attempt, sizeof execution_attempt, "%s", attempt);
   snprintf(error, size, "%s", execution_mode == 2 ? "policy owner refused execution" : "");
   if (execution_mode == 7)
      atomic_store(agent_get_request_cancel(), 1);
   if (execution_mode == 8)
      (void)request_context_refuse_assembly("source revoked");
   if (execution_mode == 9)
   {
      char *answer = server_session_question("Which workspace?", 2);
      assert(answer && !strcmp(answer, "this workspace"));
      free(answer);
   }
   return execution_mode == 2 ? -1 : 0;
}
char *dispatch_tool_call_ir_ctx_status(const aimee_tool_t *function, const char *arguments,
                                       int timeout, int *is_error)
{
   check_execution_owner(function->name);
   assert(effect_authorized && timeout == 120000);
   assert(strstr(arguments, "123.123456789012345678901234567890"));
   dispatch_count++;
   if (atomic_load(&cli_shell_probe) && !strcmp(function->name, "bash"))
      exercise_nested_cli();
   *is_error = execution_mode == 3;
   if (execution_mode == 4)
      atomic_store(&reject_durable, 1);
   return strdup(execution_mode == 3 ? "actual tool owner failure" : "actual tool owner result");
}
char *policy_annotate_indexed(const char *name, const char *arguments, const char *attempt,
                              char *text)
{
   check_execution_owner(name);
   assert(!effect_authorized && strstr(arguments, "123.123456789012345678901234567890"));
   assert(!strcmp(execution_attempt, attempt));
   if (execution_mode == 5)
   {
      free(text);
      return NULL;
   }
   return text;
}

/* The real owner handshake is exercised by test_ingress_preinject. This runtime
 * fixture verifies callbacks reach it under their distinct captured identity. */
int ingress_preinject_fork_sources(const request_context_t *parent)
{
   const request_context_t *current = request_context_get();
   assert(parent && current && strcmp(parent->request_id, current->request_id));
   assert(!strcmp(parent->principal, current->principal));
   assert(!strcmp(parent->vault_principal, current->vault_principal));
   if (parent->memory_source_release[0])
      return request_context_set_source_release("0123456789abcdef0123456789abcdef");
   return 0;
}
void ingress_preinject_finish_sources(void)
{
   assert(request_context_get()->memory_source_release[0]);
   assert(request_context_set_source_release("") == 0);
}

const agent_t *agent_registry_bind_ingress_model(const agent_t *selected)
{
   const agent_t *previous = current_model;
   current_model = selected;
   if (selected)
      scope_admitted = 1;
   return previous;
}
int agent_codex_connection_credentials(const char *registration, agent_codex_credentials_t *out)
{
   const request_context_t *owner = request_context_get();
   assert(owner && !strcmp(owner->vault_principal, current_credentials.vault_principal));
   assert(!strcmp(owner->session_key, current_credentials.session_id));
   assert(!(owner->capabilities & (CAP_WORKFLOW_ADMIN | CAP_GRANT_ADMIN | CAP_SHADOW_ADMIN)));
   if (strcmp(registration, "selected-account"))
      return -1;
   memset(out, 0, sizeof *out);
   strcpy(out->token, "host-only-upstream-secret");
   strcpy(out->account_id, "selected-vendor-account");
   memset(out->source_digest,
          atomic_load(&malformed_source) ? 'z'
          : atomic_load(&source_changed) ? 'b'
                                         : 'a',
          64);
   return 0;
}
int agent_claude_connection_credentials(const char *registration, agent_claude_credentials_t *out)
{
   agent_codex_credentials_t source;
   if (agent_codex_connection_credentials(registration, &source))
      return -1;
   memset(out, 0, sizeof *out);
   strcpy(out->token, "host-only-claude-secret");
   memset(out->source_digest, atomic_load(&source_changed) ? 'f' : 'e', 64);
   return 0;
}
int agent_set_request_claude_creds_for(const agent_t *agent,
                                       const agent_claude_credentials_t *credentials)
{
   assert(agent && credentials && !strcmp(credentials->token, "host-only-claude-secret"));
   strcpy(current_credentials.claude_token, credentials->token);
   strcpy(current_credentials.claude_registration, agent->registration);
   return 0;
}
int agent_api_connection_credentials(const agent_t *agent, agent_api_credentials_t *out)
{
   const request_context_t *owner = request_context_get();
   assert(owner && !strcmp(owner->vault_principal, current_credentials.vault_principal));
   assert(!strcmp(owner->session_key, current_credentials.session_id));
   if (strcmp(agent->auth_type, "api_key") || strcmp(agent->registration, "selected-account"))
      return -1;
   memset(out, 0, sizeof *out);
   strcpy(out->api_key, "host-only-api-secret");
   memset(out->target_digest, 'e', 64);
   memset(out->source_digest,
          atomic_load(&malformed_source) ? 'z'
          : atomic_load(&source_changed) ? 'd'
                                         : 'c',
          64);
   return 0;
}
int agent_set_request_api_credentials_for(const agent_t *agent,
                                          const agent_api_credentials_t *credentials)
{
   assert(agent && !strcmp(agent->registration, "selected-account"));
   assert(credentials && !strcmp(credentials->api_key, "host-only-api-secret"));
   snprintf(current_credentials.api_key, sizeof current_credentials.api_key, "%s",
            credentials->api_key);
   snprintf(current_credentials.api_key_registration,
            sizeof current_credentials.api_key_registration, "%s", agent->registration);
   snprintf(current_credentials.api_key_target, sizeof current_credentials.api_key_target, "%s",
            credentials->target_digest);
   return 0;
}
int db1_server_session_get(const char *session, db1_server_session_t *out)
{
   memset(out, 0, sizeof *out);
   snprintf(out->created_at, sizeof out->created_at, "%s",
            atomic_load(&source_recreated) ? "2026-10-20T00:01:00Z" : "2026-10-20T00:00:00Z");
   if (atomic_load(&session_removed))
      return -1;
   if (!strcmp(session, "session-a"))
      strcpy(out->principal, "webuser:alice");
   else if (!strcmp(session, "session-b"))
      strcpy(out->principal, "webuser:bob");
   else if (!strcmp(session, "private-child"))
   {
      if (atomic_load(&destination_invalid) == 1)
         return -1;
      strcpy(out->principal,
             atomic_load(&destination_invalid) == 2 ? "webuser:bob" : "webuser:alice");
   }
   else
      return -1;
   return 0;
}
void server_http_identity_bind_request_owner(const request_context_t *owner)
{
   snprintf(identity, sizeof identity, "%s", owner ? owner->vault_principal : "");
}
int server_http_route_allowed_caps(int tcp, uint32_t caps, const char *method, const char *path,
                                   int writes)
{
   (void)writes;
   assert(!tcp);
   if (!strcmp(method, "GET") && !strcmp(path, "/v1/cli/manifest"))
      return 1;
   if (strcmp(method, "POST"))
      return 0;
   if (!strcmp(path, "/v1/chat/control") || !strcmp(path, "/v1/config/set"))
      return 1; /* Parent authority must still be outside the child surface. */
   return (!strcmp(path, "/v1/responses") || !strcmp(path, "/v1/messages")) ? (caps & CAP_CHAT) != 0
          : !strcmp(path, "/v1/mcp/call") ? (caps & CAP_TOOL_EXECUTE) != 0
                                          : 0;
}
void anthropic_http_capture_request_headers(const char *headers)
{
   protocol_captured = headers != NULL;
   if (headers)
   {
      assert(strstr(headers, "Anthropic-Version: 2023-06-01"));
      assert(strstr(headers, "Anthropic-Beta: fixture-beta"));
      assert(!strstr(headers, "Authorization") && !strstr(headers, "mallory") &&
             !strstr(headers, "forged"));
   }
}
int server_http_native_forward(int fd, const char *method, const char *path, const char *body,
                               int length)
{
   (void)method;
   probe_replay_scope(path);
   assert(protocol_captured);
   assert(length == (int)strlen(body));
   if (strncmp(path, "/v1/mcp/", 8))
      assert(length == 2 && !strcmp(body, "{}"));
   const request_context_t *owner = request_context_get();
   assert(owner && owner->vault_transport == ATTEST_WEBCHAT_TRUSTED);
   assert(!strcmp(identity, owner->vault_principal));
   assert(!strcmp(owner->method, method) && !strcmp(owner->path, path));
   assert(owner->model_execution);
   int alice = !strcmp(owner->vault_principal, "webuser:alice");
   assert(agent_tools_scope_isolated());
   assert(!strcmp(tool_role, alice ? "reviewer" : ""));
   const char *toolset = agent_tools_active_toolset();
   assert(alice ? toolset && !strcmp(toolset, "review_indexed") : !toolset);
   assert(agent_tools_readonly_delegate_blocks() == alice);
   assert(agent_tools_shell_allowed() == !alice && agent_tools_knowledge_write_allowed() == !alice);
   assert(agent_tools_tool_denied(alice ? "git_push" : "write_file"));
   assert(!strcmp(agent_tools_parent_write_guard_root(), alice ? "/parent/alice" : "/parent/bob"));
   assert(
       !strcmp(agent_tools_parent_write_guard_write_root(), alice ? "/work/alice" : "/work/bob"));
   assert(agent_tools_get_turn() == (alice ? 7 : 9));
   assert(agent_tools_get_snap_id() == (alice ? 71 : 91));
   assert(!strcmp(run_cmd_get_cwd(), alice ? "/work/alice" : "/work/bob"));
   assert(!strcmp(session_id(), owner->session_key));
   assert(job_id == (alice ? 11 : 22));
   assert(workspace == (alice ? &alice_workspace : &bob_workspace));
   assert(current_mirror == &fixture_mirror);
   assert(delegate_scope.depth == (alice ? 2 : 0));
   assert(!strcmp(delegate_scope.parent, alice ? "parent-alice" : ""));
   assert(delegate_scope.mailbox == (alice ? (void *)&alice_workspace : NULL));
   assert(!effect_authorized);
   assert(server_session_capture_current() == atomic_load(&expected_capture));
   assert(server_control_scope_current() ==
          (alice && !strncmp(path, "/v1/mcp/", 8) ? alice_controls : NULL));
   agent_source_authority_snapshot_t observed = {0};
   assert(!agent_source_authority_tls_capture_checked(&observed));
   assert(observed.active && observed.authority == alice);
   assert(!strcmp(observed.root, alice ? "/source/alice" : ""));
   assert(alice ? observed.paths && !strcmp(observed.paths, "src/alice.c\n") : !observed.paths);
   agent_source_authority_snapshot_destroy(&observed);
   if (!strncmp(path, "/v1/mcp/", 8))
   {
      cJSON *request = cJSON_Parse(body);
      assert(request &&
             !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "session_id")),
                     owner->session_key));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "cwd")),
                     "/work/alice"));
      assert(strstr(body, "9007199254740993"));
      const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(request, "arguments");
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "cwd")),
                     "/tool/specific/path"));
      cJSON_Delete(request);
   }

   assert(!owner->idempotency_key[0]);
   assert(strlen(owner->request_id) == 32);
   pthread_mutex_lock(&ids_mutex);
   assert(seen_count < (int)(sizeof seen_ids / sizeof seen_ids[0]));
   for (int i = 0; i < seen_count; i++)
      assert(strcmp(seen_ids[i], owner->request_id));
   strcpy(seen_ids[seen_count++], owner->request_id);
   pthread_mutex_unlock(&ids_mutex);
   assert(!strcmp(current_credentials.vault_principal, identity));
   assert(!strcmp(current_credentials.session_id, owner->session_key));
   assert(current_model && !strcmp(current_model->registration, "selected-account"));
   if (!strcmp(current_model->auth_type, "claude-oauth"))
   {
      assert(!current_credentials.codex_token[0] && !current_credentials.api_key[0]);
      if (!strncmp(path, "/v1/mcp/", 8))
         assert(!current_credentials.claude_token[0]);
      else
         assert(!strcmp(current_credentials.claude_token, "host-only-claude-secret"));
   }
   else if (!strcmp(current_model->auth_type, "api_key"))
   {
      assert(!current_credentials.codex_token[0]);
      if (!strncmp(path, "/v1/mcp/", 8))
         assert(
             !current_credentials.api_key[0]); /* Tools/delegates resolve their own credentials. */
      else
      {
         assert(!strcmp(current_credentials.api_key, "host-only-api-secret"));
         assert(!strcmp(current_credentials.api_key_registration, current_model->registration));
      }
   }
   else
   {
      assert(!current_credentials.api_key[0]);
      if (!strncmp(path, "/v1/mcp/", 8))
         assert(!current_credentials.codex_token[0] && !current_credentials.codex_account_id[0]);
      else
         assert(!strcmp(current_credentials.codex_token, "host-only-upstream-secret"));
   }
   atomic_fetch_add(&calls, 1);
   if (atomic_load(&wait_for_cancel))
   {
      for (int i = 0; i < 300 && !atomic_load(current_cancel); i++)
         usleep(10000);
      assert(atomic_load(current_cancel));
      atomic_store(&cancelled, 1);
      return -1;
   }
   char response[512], payload[256];
   int n =
       snprintf(payload, sizeof payload, "{\"owner\":\"%s\",\"session\":\"%s\",\"path\":\"%s\"}",
                identity, current_credentials.session_id, path);
   int size =
       snprintf(response, sizeof response,
                "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", n, payload);
   return aimee_core_socket_write_all(fd, response, (size_t)size);
}

/* The standalone replay fixture exercises the real durable proof owner. This
 * listener fixture verifies its guard/sink are confined to model requests. */
struct server_native_replay
{
   char actor[VAULT_PRINCIPAL_MAX], session[128];
};
static atomic_int owned_profile_probe, snapshot_generation, tool_source_probe, postpolicy_gap,
    flat_bound_probe, preframe_error_probe;
static cJSON *source_rows;
server_native_replay_t *server_native_replay_open(const char *session, const char *actor,
                                                  const char *scope, const char *model)
{
   assert(session[0] && strlen(scope) == 64 && !strcmp(model, "selected-model"));
   server_native_replay_t *r = calloc(1, sizeof *r);
   assert(r);
   strcpy(r->actor, actor);
   strcpy(r->session, session);
   const char *destination = server_session_capture_session(server_session_capture_current());
   const char *expected = destination                       ? destination
                          : !strcmp(actor, "webuser:alice") ? "session-a"
                                                            : "session-b";
   assert(!strcmp(session, expected));
   return r;
}
void server_native_replay_close(server_native_replay_t *r)
{
   free(r);
}
int server_native_replay_restore(aimee_request_t *request, void *context)
{
   (void)request;
   server_native_replay_t *r = context;
   assert(!strcmp(r->actor, identity));
   const char *destination = server_session_capture_session(server_session_capture_current());
   assert(!strcmp(r->session, destination ? destination : session_id()));
   return 0;
}
int server_native_replay_capture(const aimee_session_event_t *event, void *context)
{
   (void)event;
   if (atomic_load(&reject_proof))
      return -1;
   return server_native_replay_restore(NULL, context);
}
static int probe_tool_snapshot(const char *name, const char *space, const char *arguments)
{
   cJSON *input = aimee_ir_json_parse_frame(arguments, strlen(arguments));
   assert(input);
   aimee_block_t block = {.type = AIMEE_BLK_TOOL_USE,
                          .tool_id = "wire-call:original",
                          .tool_name = (char *)name,
                          .tool_namespace = (char *)space,
                          .tool_input = input};
   aimee_message_t message = {.role = "assistant", .blocks = &block, .n_blocks = 1};
   aimee_session_event_t observed = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "wire-response", .message = &message};
   int captured = aimee_ir_session_emit(&observed);
   cJSON_Delete(input);
   return captured;
}
static int probe_original_tool(void)
{
   int mode = atomic_load(&tool_source_probe);
   int owned = atomic_load(&owned_profile_probe);
   const char *name = atomic_load(&cli_shell_probe) ? "bash" : owned ? "read_file" : "exec_command";
   const char *space = owned ? "aimee" : NULL;
   if (atomic_load(&flat_bound_probe))
   {
      name = atomic_load(&flat_bound_probe) == 2 ? "mcp__aimee__read_file" : "read_file";
      space = NULL;
   }
   if (mode == 1 || mode == 6)
      name = "exec_command";
   if (mode == 2)
      space = NULL;
   if (mode == 8)
      name = "postpolicy_read";
   const char *arguments = "{\"amount\":123.123456789012345678901234567890}";
   if (mode < 3 || mode == 5 || mode == 8)
      return probe_tool_snapshot(name, space, arguments);
   aimee_delta_t deltas[] = {{.type = AIMEE_DELTA_TURN_START},
                             {.type = AIMEE_DELTA_BLOCK_START,
                              .kind = AIMEE_BLK_TOOL_USE,
                              .block_id = 0,
                              .tool_id = "wire-call:original",
                              .tool_name = name,
                              .tool_namespace = space},
                             {.type = AIMEE_DELTA_BLOCK_DELTA,
                              .kind = AIMEE_BLK_TOOL_USE,
                              .block_id = 0,
                              .tool_args_delta = mode == 4 ? "[]" : arguments},
                             {.type = AIMEE_DELTA_BLOCK_STOP, .block_id = 0},
                             {.type = AIMEE_DELTA_TURN_STOP,
                              .stop_reason = AIMEE_STOP_TOOL_USE,
                              .usage_in = 19,
                              .usage_out = 11,
                              .usage_cache_read = 3,
                              .usage_cache_write = 2,
                              .usage_reasoning = 1}};
   for (unsigned i = 0; i < sizeof deltas / sizeof *deltas; i++)
   {
      aimee_session_event_t event = {.kind = AIMEE_SESSION_DELTA,
                                     .role = "assistant",
                                     .message_id = mode == 7 && i == 2 ? "substituted-response"
                                                                       : "wire-response",
                                     .delta = &deltas[i]};
      int rc = aimee_ir_session_emit(&event);
      if (rc)
         return rc;
   }
   /* A final full snapshot must not create a second claimable original call. */
   return mode == 3 ? probe_tool_snapshot(name, space, arguments) : 0;
}
static void probe_replay_scope(const char *path)
{
   aimee_frontend_state_scope_t state_guard = aimee_frontend_bind_state_guard(NULL, NULL);
   aimee_frontend_bind_state_guard(state_guard.guard, state_guard.context);
   aimee_frontend_state_scope_t request_owner_scope = aimee_frontend_bind_request_owner(NULL, NULL);
   aimee_frontend_bind_request_owner(request_owner_scope.guard, request_owner_scope.context);
   void *sink_context = NULL;
   aimee_ir_session_sink_fn installed_sink = aimee_ir_session_get_sink(&sink_context);
   if (!strcmp(path, "/v1/responses") || !strcmp(path, "/v1/messages"))
   {
      assert(state_guard.guard && installed_sink);
      aimee_request_t request = {0};
      if (atomic_load(&owned_profile_probe))
      {
         assert(request_owner_scope.guard);
         request.model = strdup("SDK model selector");
         request.raw = cJSON_Parse("{\"conversation\":\"SDK private history\"}");
         request.raw_bytes = strdup("SDK original bytes");
         request.raw_length = strlen(request.raw_bytes);
         int admitted = request_owner_scope.guard(&request, request_owner_scope.context);
         if (atomic_load(&snapshot_fault) == 3)
         {
            assert(admitted == -1);
            aimee_request_free(&request);
            return;
         }
         assert(!admitted && !strcmp(request.model, "selected-model") && request.mutated);
         assert(!request.raw && !request.raw_bytes && !request.raw_length);
         assert(request.n_system == 1 && !strcmp(request.system[0].text, "host-owned system"));
         assert(request.n_messages == 1 && request.n_tools == 1);
         assert(!strcmp(request.messages[0].blocks[0].text, atomic_load(&snapshot_generation) == 1
                                                                ? "fresh durable one"
                                                                : "fresh durable two"));
         int flat = atomic_load(&flat_bound_probe);
         assert(aimee_ir_find_function(&request, flat ? NULL : "aimee",
                                       flat == 2 ? "mcp__aimee__read_file" : "read_file"));
         assert(request.prompt_cache_key && !request.prompt_cache_key[0]);
         request.max_tokens = 17;
         if (atomic_load(&postpolicy_gap))
         {
            cJSON *gap =
                cJSON_Parse("[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_"
                            "id\":\"unresolved-write\",\"name\":\"write_file\",\"input\":{}}]}]");
            assert(gap && aimee_ir_history_repair(gap) == 1);
            for (int i = 0; i < request.n_messages; i++)
               aimee_message_free(&request.messages[i]);
            free(request.messages);
            request.n_messages = cJSON_GetArraySize(gap);
            request.messages = calloc((size_t)request.n_messages, sizeof *request.messages);
            assert(request.messages);
            for (int i = 0; i < request.n_messages; i++)
            {
               int parsed =
                   aimee_ir_message_parse(cJSON_GetArrayItem(gap, i), &request.messages[i]);
               assert(!parsed);
            }
            cJSON_Delete(gap);
         }
         if (atomic_load(&cli_shell_probe) == 2)
         {
            assert(request.tools[0].n_children == 2);
            aimee_tool_free(&request.tools[0].children[1]);
            request.tools[0].n_children = 1;
         }
         if (atomic_load(&cli_shell_probe) == 3)
         {
            free(request.tools[0].children[1].description);
            request.tools[0].children[1].description = strdup("post-policy changed schema");
            cJSON *amount = cJSON_GetObjectItemCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(request.tools[0].children[1].schema, "properties"),
                "amount");
            assert(amount && cJSON_AddNumberToObject(amount, "maximum", 0));
         }
         int mode = atomic_load(&tool_source_probe);
         if (mode == 5 || mode == 8)
         {
            /* This is a real downstream catalog change after entry ownership. */
            free(request.tools[0].children[0].name);
            request.tools[0].children[0].name = strdup("postpolicy_read");
         }
      }
      else
         assert(!request_owner_scope.guard);
      int guarded = state_guard.guard(&request, state_guard.context);
      if (atomic_load(&postpolicy_gap))
      {
         assert(guarded == -1);
         aimee_request_free(&request);
         return;
      }
      assert(!guarded);
      if (atomic_load(&owned_profile_probe))
         assert(request.max_tokens == 17);
      if (atomic_load(&flat_bound_probe))
      {
         /* The actual post-policy flat catalog renders for every backend;
          * native scheduler names do not require a provider namespace feature. */
         for (aimee_wire_t wire = AIMEE_WIRE_ANTHROPIC; wire <= AIMEE_WIRE_RESPONSES; wire++)
         {
            char *native = aimee_backend_render_wire(&request, wire);
            assert(native);
            cJSON *rendered = aimee_ir_json_parse_frame(native, strlen(native));
            assert(rendered && cJSON_GetArraySize(cJSON_GetObjectItem(rendered, "tools")) == 1);
            free(native);
            cJSON_Delete(rendered);
         }
      }
      aimee_request_free(&request);
      if (atomic_load(&preframe_error_probe))
      {
         /* Actual ingress emits this host diagnostic when transport fails
          * before any vendor message_start. It is not an assistant message. */
         aimee_delta_t error = {.type = AIMEE_DELTA_ERROR,
                                .error_message = "Provider transport failed before first frame"};
         aimee_session_event_t diagnostic = {.kind = AIMEE_SESSION_DELTA,
                                             .role = "assistant",
                                             .message_id = "native",
                                             .delta = &error};
         assert(!aimee_ir_session_emit(&diagnostic));
         return;
      }
      aimee_session_event_t event = {.kind = AIMEE_SESSION_PROJECTION,
                                     .projection_model = "selected-model",
                                     .projection_boundary = "response",
                                     .projection_reasoning = 1};
      int captured = aimee_ir_session_emit(&event);
      if (atomic_load(&expected_capture))
      {
         assert(captured == (atomic_load(&reject_durable) ? -1 : 0));
         if (!captured)
         {
            captured = probe_original_tool();
            int mode = atomic_load(&tool_source_probe);
            int refuses =
                mode == 1 || mode == 2 || mode == 4 || mode == 5 || mode == 6 || mode == 7;
            assert(captured == ((atomic_load(&reject_proof) || refuses) ? -1 : 0));
         }
      }
      else
         assert(captured == (atomic_load(&reject_proof) ? -1 : 0));
   }
   else
      assert(!state_guard.guard && !request_owner_scope.guard && !installed_sink);
}
static int send_request(const server_native_runtime_t *runtime, const char *path, const char *token)
{
   const char *port = strrchr(server_native_runtime_url(runtime), ':') + 1;
   int fd = aimee_core_socket_connect("127.0.0.1", port, 2000);
   assert(fd >= 0);
   assert(!aimee_core_socket_set_timeouts(fd, 5000, 2000));
   char request[2048];
   const char *body =
       strncmp(path, "/v1/mcp/", 8)
           ? "{}"
           : "{\"session_id\":\"forged-session\",\"cwd\":\"/forged/project\","
             "\"arguments\":{\"cwd\":\"/tool/specific/path\",\"large\":9007199254740993}}";
   int n = snprintf(request, sizeof request,
                    "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%s\r\nAuthorization: Bearer %s\r\n"
                    "X-Aimee-Webuser: mallory\r\naimee-session-id: forged\r\n"
                    "Anthropic-Version: 2023-06-01\r\nAnthropic-Beta: fixture-beta\r\n"
                    "Content-Length: %zu\r\n\r\n%s",
                    path, port, token, strlen(body), body);
   assert(!aimee_core_socket_write_all(fd, request, (size_t)n));
   return fd;
}
static void request(const server_native_runtime_t *runtime, const char *path, const char *token,
                    char response[1024])
{
   int fd = send_request(runtime, path, token);
   size_t used = 0;
   while (used < 1023)
   {
      long n = aimee_core_socket_read(fd, response + used, 1023 - used);
      assert(n >= 0);
      if (!n)
         break;
      used += (size_t)n;
   }
   response[used] = 0;
   aimee_core_socket_close(fd);
}
static void test_cache_profile(const agent_t *selected, const request_context_t *owner,
                               const server_native_runtime_t *original)
{
   char build[65], launch[65], lease[65], baseline[65], changed[65];
   memset(build, 'b', 64);
   memset(launch, 'c', 64);
   memset(lease, 'd', 64);
   build[64] = launch[64] = lease[64] = 0;
   server_native_execution_profile_t profile = {
       .sdk = "codex", .build_digest = build, .launch_digest = launch, .workspace_digest = lease};
   int rc = server_native_runtime_cache_scope(original, &profile, baseline);
   assert(rc == 0 && strlen(baseline) == 64);
   for (int i = 0; i < 3; i++)
   {
      char *field = i == 0 ? build : i == 1 ? launch : lease;
      char previous = field[0];
      field[0] = 'e';
      rc = server_native_runtime_cache_scope(original, &profile, changed);
      assert(rc == 0 && strcmp(baseline, changed));
      field[0] = previous;
   }
   profile.workspace_digest = NULL;
   rc = server_native_runtime_cache_scope(original, &profile, changed);
   assert(rc < 0 && !changed[0]);
   profile.workspace_digest = lease;

   owner_tools(1);
   agent_tools_begin_turn(300);
   agent_tools_set_snap_id(3000);
   server_native_runtime_t *copy = server_native_runtime_open(selected, owner, "session-a");
   assert(copy);
   rc = server_native_runtime_cache_scope(copy, &profile, changed);
   assert(rc == 0 && !strcmp(baseline, changed)); /* turn/snapshot/transport nonce are ephemeral */
   server_native_runtime_close(copy);
   agent_tools_set_dispatch_role("implement");
   copy = server_native_runtime_open(selected, owner, "session-a");
   assert(copy);
   assert(!strcmp(server_native_runtime_binding(original), server_native_runtime_binding(copy)));
   rc = server_native_runtime_cache_scope(copy, &profile, changed);
   assert(rc == 0 && strcmp(baseline, changed)); /* replay lineage is not cache authority */
   server_native_runtime_close(copy);

   for (int i = 0; i < 4; i++)
   {
      owner_tools(1);
      if (i == 0)
         agent_tools_write_capable_set(1);
      else if (i == 1)
         agent_tools_shell_set(1);
      else if (i == 2)
         agent_tools_knowledge_write_set(1);
      else
         agent_tools_parent_write_guard_set("/parent/alice", "/different/write/root");
      copy = server_native_runtime_open(selected, owner, "session-a");
      assert(copy);
      rc = server_native_runtime_cache_scope(copy, &profile, changed);
      assert(rc == 0 && strcmp(baseline, changed));
      server_native_runtime_close(copy);
   }
   owner_tools(1);
   const char *denied[] = {"write_file", "git_push", "write_file"};
   agent_tools_denied_set(denied, 3);
   copy = server_native_runtime_open(selected, owner, "session-a");
   assert(copy);
   rc = server_native_runtime_cache_scope(copy, &profile, baseline);
   assert(rc == 0);
   server_native_runtime_close(copy);
   const char *reordered[] = {"git_push", "write_file"};
   agent_tools_denied_set(reordered, 2);
   copy = server_native_runtime_open(selected, owner, "session-a");
   assert(copy);
   rc = server_native_runtime_cache_scope(copy, &profile, changed);
   assert(rc == 0 && !strcmp(baseline, changed));
   server_native_runtime_close(copy);
   agent_source_authority_tls_set(1, "/different/source", "src/alice.c\n");
   copy = server_native_runtime_open(selected, owner, "session-a");
   assert(copy);
   rc = server_native_runtime_cache_scope(copy, &profile, changed);
   assert(rc == 0 && strcmp(baseline, changed));
   server_native_runtime_close(copy);
   owner_tools(1);
}

int db1_session_ir_running_history_load(const char *session, const char *actor, const char *turn,
                                        char **out)
{
   server_session_capture_t *owner = server_session_capture_current();
   assert(owner && !strcmp(session, server_session_capture_session(owner)));
   assert(!strcmp(actor, "webuser:alice") && !strcmp(turn, "native-turn"));
   *out = NULL;
   if (atomic_load(&snapshot_fault) == 1)
      return -1;
   const char *wire = atomic_load(&snapshot_fault) == 2
                          ? "[{\"role\":\"operator\",\"blocks\":[]}]"
                          : "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":"
                            "\"canonical source prompt\"}]}]";
   if (!atomic_load(&snapshot_fault) && atomic_load(&snapshot_generation))
      wire = atomic_load(&snapshot_generation) == 1 ? "[{\"role\":\"user\",\"blocks\":[{\"type\":"
                                                      "\"text\",\"text\":\"fresh durable one\"}]}]"
                                                    : "[{\"role\":\"user\",\"blocks\":[{\"type\":"
                                                      "\"text\",\"text\":\"fresh durable two\"}]}]";
   if (atomic_load(&snapshot_fault) == 3)
      wire = "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":\"unresolved-"
             "write\",\"name\":\"write_file\",\"input\":{}}]}]";
   *out = strdup(wire);
   return *out ? 0 : -1;
}
/* The model callback emits typed observations; inspect what the actual retained
 * writer commits, without manufacturing a host execution terminal. */
int db1_session_ir_append(const char *session, const char *id, const char *turn,
                          const char *principal, const char *wire, char sequence[32])
{
   server_session_capture_t *owner = atomic_load(&expected_capture);
   assert(owner && !strcmp(session, server_session_capture_session(owner)));
   assert(!strcmp(principal, "webuser:alice") && !strcmp(turn, "native-turn"));
   pthread_mutex_lock(&capture_lock);
   if (atomic_load(&reject_durable))
   {
      pthread_mutex_unlock(&capture_lock);
      return -1;
   }
   cJSON *row = aimee_ir_json_parse_frame(wire, strlen(wire));
   assert(row && captured_rows && id[0]);
   if (!cJSON_GetArraySize(captured_rows))
   {
      cJSON_Delete(source_rows);
      source_rows = cJSON_CreateArray();
   }
   cJSON_AddItemToArray(captured_rows, row);
   snprintf(sequence, 32, "%d", cJSON_GetArraySize(captured_rows));
   cJSON *envelope = cJSON_CreateObject();
   assert(envelope && source_rows);
   cJSON_AddNumberToObject(envelope, "version", 1);
   cJSON_AddStringToObject(envelope, "session_id", session);
   cJSON_AddStringToObject(envelope, "sequence", sequence);
   cJSON_AddStringToObject(envelope, "event_id", id);
   cJSON_AddStringToObject(envelope, "turn_id", turn);
   cJSON_AddStringToObject(envelope, "actor", principal);
   cJSON_AddItemToObject(envelope, "event", cJSON_Duplicate(row, 1));
   cJSON_AddItemToArray(source_rows, envelope);
   pthread_mutex_unlock(&capture_lock);
   return 0;
}
/* This fixture has no private verifier owner. Privileged proof appends must
 * remain unreachable; ordinary event capture retains its existing test path. */
int db1_session_ir_append_verified(const char *session, const char *event, const char *turn,
                                   const char *actor, const char *payload, char sequence[32])
{
   (void)session;
   (void)event;
   (void)turn;
   (void)actor;
   (void)payload;
   (void)sequence;
   assert(!"unexpected verifier proof in ordinary capture fixture");
   return -1;
}
int db1_session_native_source_append(const char *session, const char *id, const char *turn,
                                     const char *actor, const char *payload, char sequence[32])
{
   cJSON *event = aimee_ir_json_parse_frame(payload, strlen(payload));
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(event, "native_owner")));
   cJSON_Delete(event);
   return db1_session_ir_append(session, id, turn, actor, payload, sequence);
}
int db1_session_native_history_load(const char *actor, const char *session, const char *mode,
                                    const char *turn, char **out)
{
   (void)actor;
   (void)session;
   (void)mode;
   (void)turn;
   *out = NULL;
   assert(!"unexpected source sidecar lookup in ordinary runtime fixture");
   return -1;
}
char *db1_session_ir_page(const char *session, const char *after, int limit, const char *actor)
{
   assert(!strcmp(session, server_session_capture_session(atomic_load(&expected_capture))) &&
          !strcmp(actor, "webuser:alice"));
   assert(limit >= 1 && limit <= 128);
   pthread_mutex_lock(&capture_lock);
   cJSON *page = cJSON_CreateArray();
   int offset = atoi(after);
   for (int i = offset;
        !atomic_load(&source_missing) && i < cJSON_GetArraySize(source_rows) && i < offset + limit;
        i++)
   {
      cJSON *row = cJSON_Duplicate(cJSON_GetArrayItem(source_rows, i), 1);
      cJSON *event = cJSON_GetObjectItemCaseSensitive(row, "event");
      cJSON *delta = cJSON_GetObjectItemCaseSensitive(event, "payload");
      cJSON *fragment = cJSON_GetObjectItemCaseSensitive(delta, "tool_args_delta");
      if (atomic_load(&source_tampered) && fragment)
         cJSON_SetValuestring(fragment, "{\"amount\":999}");
      cJSON_AddItemToArray(page, row);
   }
   char *wire = cJSON_PrintUnformatted(page);
   cJSON_Delete(page);
   pthread_mutex_unlock(&capture_lock);
   return wire;
}

typedef struct
{
   server_native_runtime_t *runtime;
   const aimee_block_t *proposed;
   server_native_tool_claim_t claim;
   int result, reference;
} source_claim_worker_t;
static void *claim_source_worker(void *context)
{
   source_claim_worker_t *worker = context;
   worker->result =
       worker->reference
           ? server_native_runtime_claim_reference(worker->runtime, worker->proposed->tool_id,
                                                   worker->proposed->tool_namespace,
                                                   worker->proposed->tool_name, &worker->claim)
           : server_native_runtime_claim_tool(worker->runtime, worker->proposed, &worker->claim);
   return NULL;
}

static void test_source_claim(server_native_runtime_t *runtime, const char *name, int streamed,
                              int reference)
{
   const char *arguments = "{\"amount\":123.123456789012345678901234567890}";
   cJSON *input = aimee_ir_json_parse_frame(arguments, strlen(arguments));
   aimee_block_t proposed = {.type = AIMEE_BLK_TOOL_USE,
                             .tool_id = "wire-call:original",
                             .tool_namespace = "aimee",
                             .tool_name = (char *)name,
                             .tool_input = input};
   server_native_tool_claim_t claim = {0};
   cJSON *wrong = cJSON_Parse("{\"amount\":999}");
   proposed.tool_input = wrong;
   int rc = server_native_runtime_claim_tool(runtime, &proposed, &claim);
   assert(rc == -1 && !claim.original.n_messages);
   cJSON_Delete(wrong);
   proposed.tool_input = input;
   if (reference)
   {
      assert(server_native_runtime_claim_reference(runtime, "unknown-id", "aimee", name, &claim) <
             0);
      assert(server_native_runtime_claim_reference(runtime, proposed.tool_id, "foreign", name,
                                                   &claim) < 0);
      assert(server_native_runtime_claim_reference(runtime, proposed.tool_id, "aimee", "foreign",
                                                   &claim) < 0);
   }
   proposed.tool_namespace = NULL;
   rc = (reference ? server_native_runtime_claim_reference(runtime, proposed.tool_id,
                                                           proposed.tool_namespace,
                                                           proposed.tool_name, &claim)
                   : server_native_runtime_claim_tool(runtime, &proposed, &claim));
   assert(rc == -1);
   proposed.tool_namespace = "aimee";
   atomic_store(&source_missing, 1);
   rc = (reference ? server_native_runtime_claim_reference(runtime, proposed.tool_id,
                                                           proposed.tool_namespace,
                                                           proposed.tool_name, &claim)
                   : server_native_runtime_claim_tool(runtime, &proposed, &claim));
   assert(rc == -1);
   atomic_store(&source_missing, 0);
   atomic_store(&source_recreated, 1);
   rc = (reference ? server_native_runtime_claim_reference(runtime, proposed.tool_id,
                                                           proposed.tool_namespace,
                                                           proposed.tool_name, &claim)
                   : server_native_runtime_claim_tool(runtime, &proposed, &claim));
   assert(rc == -1);
   atomic_store(&source_recreated, 0);
   if (streamed)
   {
      /* Replacing an earlier argument fragment while preserving BLOCK_STOP
       * must invalidate the entire committed source prefix. */
      atomic_store(&source_tampered, 1);
      rc = (reference ? server_native_runtime_claim_reference(runtime, proposed.tool_id,
                                                              proposed.tool_namespace,
                                                              proposed.tool_name, &claim)
                      : server_native_runtime_claim_tool(runtime, &proposed, &claim));
      assert(rc == -1);
      atomic_store(&source_tampered, 0);
   }
   if (streamed)
   {
      source_claim_worker_t workers[8] = {0};
      pthread_t threads[8];
      for (int i = 0; i < 8; i++)
      {
         workers[i].runtime = runtime;
         workers[i].proposed = &proposed;
         workers[i].reference = reference && i % 2;
         rc = pthread_create(&threads[i], NULL, claim_source_worker, &workers[i]);
         assert(!rc);
      }
      int winners = 0;
      for (int i = 0; i < 8; i++)
      {
         rc = pthread_join(threads[i], NULL);
         assert(!rc);
         if (!workers[i].result)
         {
            winners++;
            claim = workers[i].claim;
            memset(&workers[i].claim, 0, sizeof workers[i].claim);
         }
         else
            assert(workers[i].result == -1 && !workers[i].claim.original.n_messages);
         server_native_tool_claim_free(&workers[i].claim);
      }
      assert(winners == 1);
   }
   else
   {
      rc = (reference ? server_native_runtime_claim_reference(runtime, proposed.tool_id,
                                                              proposed.tool_namespace,
                                                              proposed.tool_name, &claim)
                      : server_native_runtime_claim_tool(runtime, &proposed, &claim));
      assert(!rc);
   }
   assert(claim.original.n_messages == 1 && claim.block_index == 0);
   const aimee_block_t *original = &claim.original.messages[0].blocks[0];
   char *wire = cJSON_PrintUnformatted(original->tool_input);
   assert(wire && !strcmp(wire, arguments));
   free(wire);
   assert(aimee_ir_find_function(&claim.original, "aimee", name));
   rc = (reference ? server_native_runtime_claim_reference(runtime, proposed.tool_id,
                                                           proposed.tool_namespace,
                                                           proposed.tool_name, &claim)
                   : server_native_runtime_claim_tool(runtime, &proposed, &claim));
   assert(rc == -1 && claim.original.n_messages == 1); /* Failure preserves output. */
   server_native_tool_claim_free(&claim);
   cJSON_Delete(input);
}

void server_learning_observe_terminal(const char *session, const char *turn, const char *sequence,
                                      const char *id, const char *actor, const cJSON *event)
{
   (void)id;
   /* Only the explicit transport-failure regression owns a host terminal;
    * model callbacks themselves still cannot manufacture completion. */
   assert(atomic_load(&preframe_error_probe));
   assert(!strcmp(session, "private-child") && !strcmp(turn, "native-turn") &&
          !strcmp(actor, "webuser:alice") && !strcmp(sequence, "3"));
   const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "kind"));
   const cJSON *payload = cJSON_GetObjectItemCaseSensitive(event, "payload");
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "status"));
   assert(kind && !strcmp(kind, "execution") && status && !strcmp(status, "failed"));
}
static void test_retained_capture(const agent_t *selected, const request_context_t *owner)
{
   for (int variant = 0; variant < 6; variant++)
   {
      int failure = variant % 3;
      const char *destination = variant < 3 ? "session-a" : "private-child";
      owner_tools(1);
      server_session_capture_t *capture =
          server_session_capture_open(destination, "native-turn", "webuser:alice", "session-a");
      assert(capture && !server_session_capture_bind(capture));
      server_native_runtime_t *runtime = server_native_runtime_open(selected, owner, "session-a");
      assert(runtime && server_session_capture_bind(NULL) == capture);
      const cJSON *source = server_native_runtime_source_history(runtime);
      assert(cJSON_IsArray(source) && cJSON_GetArraySize(source) == 1);
      char *initial = cJSON_PrintUnformatted(source);
      assert(initial && strstr(initial, "canonical source prompt"));
      free(initial);
      atomic_store(&expected_capture, capture);
      atomic_store(&reject_proof, failure == 1);
      atomic_store(&reject_durable, failure == 2);
      captured_rows = cJSON_CreateArray();
      assert(captured_rows);
      char response[1024];
      request(runtime, "/v1/responses", server_native_runtime_token(runtime), response);
      assert(strstr(response, "200 OK"));
      assert(server_session_capture_failed(capture) == (failure != 0));
      if (!failure)
      {
         /* A new model callback repeats its thread-local native message ID. It
          * must append a new model turn instead of replacing the first one. */
         request(runtime, "/v1/responses", server_native_runtime_token(runtime), response);
         assert(strstr(response, "200 OK"));
         const cJSON *first = cJSON_GetArrayItem(captured_rows, 1);
         const cJSON *second = cJSON_GetArrayItem(captured_rows, 3);
         const char *first_id =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(first, "message_id"));
         const char *second_id =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(second, "message_id"));
         assert(first_id && second_id && strcmp(first_id, second_id));
      }
      assert(cJSON_GetArraySize(captured_rows) == (failure == 2 ? 0 : failure == 1 ? 2 : 4));
      if (failure != 2)
      {
         char *wire = cJSON_PrintUnformatted(cJSON_GetArrayItem(captured_rows, 1));
         assert(wire && strstr(wire, "native-turn:") && strstr(wire, ":wire-response") &&
                strstr(wire, "wire-call:original") && strstr(wire, "exec_command") &&
                strstr(wire, "123.123456789012345678901234567890"));
         free(wire);
      }
      assert(!aimee_ir_session_capture_failed()); /* The failing worker has its own TLS. */
      atomic_store(&reject_proof, 0);
      atomic_store(&reject_durable, 0);
      if (!failure)
         server_session_capture_close(capture);
      int before = atomic_load(&calls);
      request(runtime, "/v1/responses", server_native_runtime_token(runtime), response);
      assert(strstr(response, "502") && atomic_load(&calls) == before);
      server_native_runtime_close(runtime);
      if (failure)
         server_session_capture_close(capture);
      atomic_store(&expected_capture, NULL);
      cJSON_Delete(captured_rows);
      captured_rows = NULL;
   }
}

static void test_native_preframe_error(const agent_t *selected, const request_context_t *owner)
{
   owner_tools(1);
   aimee_tool_t function = {.name = "read_file"};
   aimee_tool_t space = {
       .kind = AIMEE_TOOL_NAMESPACE, .name = "aimee", .children = &function, .n_children = 1};
   aimee_block_t system = {.type = AIMEE_BLK_TEXT, .text = "host-owned system"};
   aimee_request_t profile = {.model = "selected-model",
                              .frontend = AIMEE_WIRE_ANTHROPIC,
                              .tools = &space,
                              .n_tools = 1,
                              .system = &system,
                              .n_system = 1,
                              .prompt_cache_key = "",
                              .mutated = 1};
   server_session_capture_t *capture =
       server_session_capture_open("private-child", "native-turn", "webuser:alice", "session-a");
   assert(capture && !server_session_capture_bind(capture));
   server_native_runtime_t *runtime =
       server_native_runtime_open_request(selected, owner, "session-a", &profile);
   assert(runtime && server_session_capture_bind(NULL) == capture);
   atomic_store(&expected_capture, capture);
   captured_rows = cJSON_CreateArray();
   atomic_store(&owned_profile_probe, 1);
   atomic_store(&snapshot_generation, 1);
   atomic_store(&preframe_error_probe, 1);
   aimee_session_event_t started = {.kind = AIMEE_SESSION_EXECUTION, .execution_status = "running"};
   assert(!server_session_capture_append(capture, &started, NULL, NULL));
   char response[1024];
   request(runtime, "/v1/messages", server_native_runtime_token(runtime), response);
   assert(!server_session_capture_failed(capture));
   server_native_runtime_quiesce(runtime);
   aimee_response_t original = {0};
   assert(server_native_runtime_result(runtime, &original) < 0);
   server_native_tool_claim_t claim = {0};
   assert(server_native_runtime_claim_reference(runtime, "invented-call", "aimee", "read_file",
                                                &claim) < 0);
   aimee_session_event_t failed = {.kind = AIMEE_SESSION_EXECUTION,
                                   .execution_status = "failed",
                                   .execution_error =
                                       "Provider transport failed before first frame"};
   assert(!server_session_capture_append(capture, &failed, NULL, NULL));
   assert(!server_session_capture_accepting(capture));
   assert(cJSON_GetArraySize(captured_rows) == 3);
   char *wire = cJSON_PrintUnformatted(captured_rows);
   assert(wire && strstr(wire, "Provider transport failed before first frame") &&
          strstr(wire, "failed") && !strstr(wire, "tool_use") && !strstr(wire, "\"message\""));
   free(wire);
   server_native_runtime_close(runtime);
   server_session_capture_close(capture);
   atomic_store(&expected_capture, NULL);
   atomic_store(&owned_profile_probe, 0);
   atomic_store(&snapshot_generation, 0);
   atomic_store(&preframe_error_probe, 0);
   cJSON_Delete(captured_rows);
   captured_rows = NULL;
}

static void test_native_original_catalog(const agent_t *selected, const request_context_t *owner)
{
   for (int variant = 0; variant < 20; variant++)
   {
      int mode = variant % 10 + 1, reference = variant >= 10;
      if (reference && mode != 3 && mode != 8)
         continue;
      owner_tools(1);
      aimee_tool_t function = {.name = "read_file"};
      aimee_tool_t space = {
          .kind = AIMEE_TOOL_NAMESPACE, .name = "aimee", .children = &function, .n_children = 1};
      aimee_request_t profile = {.model = "selected-model",
                                 .frontend = AIMEE_WIRE_RESPONSES,
                                 .tools = &space,
                                 .n_tools = 1,
                                 .mutated = 1};
      aimee_block_t system = {.type = AIMEE_BLK_TEXT, .text = "host-owned system"};
      profile.system = &system;
      profile.n_system = 1;
      profile.prompt_cache_key = "";
      server_session_capture_t *capture =
          server_session_capture_open("private-child", "native-turn", "webuser:alice", "session-a");
      assert(capture && !server_session_capture_bind(capture));
      server_native_runtime_t *runtime =
          server_native_runtime_open_request(selected, owner, "session-a", &profile);
      if (mode == 3)
      {
         cJSON *reservation = cJSON_CreateObject();
         cJSON_AddStringToObject(reservation, "session_id", "private-child");
         cJSON_AddStringToObject(reservation, "turn_id", "native-turn");
         cJSON_AddStringToObject(reservation, "actor", "webuser:alice");
         cJSON_AddStringToObject(reservation, "harness", "codex");
         cJSON_AddStringToObject(reservation, "binding_sha256",
                                 server_native_runtime_binding(runtime));
         cJSON_AddStringToObject(reservation, "owner_id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
         cJSON_AddStringToObject(reservation, "epoch", "0");
         cJSON_AddStringToObject(
             reservation, "containment_sha256",
             "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
         assert(!server_native_runtime_set_native_owner(runtime, reservation));
         assert(server_native_runtime_set_native_owner(runtime, reservation));
         cJSON_Delete(reservation);
      }
      assert(runtime && server_session_capture_bind(NULL) == capture);
      atomic_store(&expected_capture, capture);
      captured_rows = cJSON_CreateArray();
      atomic_store(&owned_profile_probe, 1);
      atomic_store(&snapshot_generation, 1);
      atomic_store(&tool_source_probe, mode <= 8 ? mode : 0);
      if (mode == 9)
         atomic_store(&snapshot_fault, 3);
      if (mode == 10)
         atomic_store(&postpolicy_gap, 1);
      char response[1024];
      int before = atomic_load(&calls);
      request(runtime, "/v1/mcp/call", server_native_runtime_token(runtime), response);
      assert(strstr(response, "404") && atomic_load(&calls) == before);
      request(runtime, "/v1/responses/compact", server_native_runtime_token(runtime), response);
      assert(strstr(response, "404") && atomic_load(&calls) == before);
      request(runtime, "/v1/responses", server_native_runtime_token(runtime), response);
      int refuses = mode != 3 && mode != 8;
      assert(server_session_capture_failed(capture) == refuses);
      /* A refused complete call is never committed or released to the SDK.
       * Earlier stream fragments remain as observed, without an invented stop. */
      int expected_rows = mode >= 9                ? 0
                          : mode == 3              ? 8
                          : mode == 8              ? 2
                          : mode == 7              ? 3
                          : mode >= 4 && mode != 5 ? 4
                                                   : 1;
      assert(cJSON_GetArraySize(captured_rows) == expected_rows);
      if (!refuses)
      {
         char *wire = cJSON_PrintUnformatted(captured_rows);
         assert(wire && strstr(wire, "123.123456789012345678901234567890"));
         free(wire);
         test_source_claim(runtime, mode == 8 ? "postpolicy_read" : "read_file", mode == 3,
                           reference);
         if (reference && mode == 3)
         {
            /* Once an ID is reused, its reference cannot select the newest
             * call, even though the old occurrence was already claimed. Exact
             * proposals can still distinguish the one unclaimed occurrence. */
            atomic_store(&snapshot_generation, 2);
            request(runtime, "/v1/responses", server_native_runtime_token(runtime), response);
            assert(!server_session_capture_failed(capture));
            server_native_tool_claim_t repeated = {0};
            assert(server_native_runtime_claim_reference(runtime, "wire-call:original", "aimee",
                                                         "read_file", &repeated) < 0);
            const char *arguments = "{\"amount\":123.123456789012345678901234567890}";
            cJSON *exact = aimee_ir_json_parse_frame(arguments, strlen(arguments));
            aimee_block_t proposed = {.type = AIMEE_BLK_TOOL_USE,
                                      .tool_id = "wire-call:original",
                                      .tool_namespace = "aimee",
                                      .tool_name = "read_file",
                                      .tool_input = exact};
            assert(!server_native_runtime_claim_tool(runtime, &proposed, &repeated));
            server_native_tool_claim_free(&repeated);
            cJSON_Delete(exact);
         }
      }
      aimee_response_t original = {0};
      assert(server_native_runtime_result(runtime, &original) < 0); /* callbacks still live */
      server_native_runtime_quiesce(runtime);
      int result_status = server_native_runtime_result(runtime, &original);
      if (mode == 3)
      {
         assert(!result_status && original.n_content == 1 &&
                original.stop_reason == AIMEE_STOP_TOOL_USE);
         int completions = reference ? 2 : 1;
         assert(original.usage_in == 19 * completions && original.usage_out == 11 * completions);
         assert(original.usage_cache_read == 3 * completions &&
                original.usage_cache_write == 2 * completions &&
                original.usage_reasoning == completions);
         assert(!strcmp(original.content[0].tool_id, "wire-call:original") &&
                !strcmp(original.content[0].tool_namespace, "aimee"));
         char *exact = cJSON_PrintUnformatted(original.content[0].tool_input);
         assert(exact && strstr(exact, "123.123456789012345678901234567890"));
         free(exact);
      }
      else
         assert(result_status < 0); /* malformed or no original TURN_STOP */
      aimee_response_free(&original);
      server_native_runtime_close(runtime);
      server_session_capture_close(capture);
      atomic_store(&expected_capture, NULL);
      atomic_store(&owned_profile_probe, 0);
      atomic_store(&snapshot_generation, 0);
      atomic_store(&tool_source_probe, 0);
      atomic_store(&snapshot_fault, 0);
      atomic_store(&postpolicy_gap, 0);
      cJSON_Delete(captured_rows);
      captured_rows = NULL;
   }
}

typedef struct
{
   server_native_runtime_t *runtime;
   const aimee_block_t *call;
   aimee_block_t result;
   int rc;
} execute_worker_t;
static void *execute_wrong_thread(void *context)
{
   execute_worker_t *worker = context;
   worker->rc = server_native_runtime_execute_tool(worker->call, &worker->result, worker->runtime);
   return NULL;
}
static void *reply_to_execution_question(void *unused)
{
   (void)unused;
   char *id = NULL;
   for (int tick = 0; tick < 200 && !id; tick++)
   {
      pthread_mutex_lock(&capture_lock);
      const cJSON *event;
      cJSON_ArrayForEach(event, captured_rows)
      {
         const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "kind"));
         if (kind && !strcmp(kind, "control_request"))
         {
            const char *type =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "control_kind"));
            assert(type && !strcmp(type, "question"));
            id =
                strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "request_id")));
            break;
         }
      }
      pthread_mutex_unlock(&capture_lock);
      if (!id)
         usleep(10000);
   }
   assert(id && !strncmp(id, "native-turn:", strlen("native-turn:")));
   cJSON *reply =
       cJSON_Parse("{\"decision\":\"allow\",\"answers\":{\"answer\":[\"this workspace\"]}}");
   int rc = server_control_reply("session-a", id, "webuser:alice", reply);
   assert(!rc);
   cJSON_Delete(reply);
   free(id);
   return NULL;
}
static const char *fixture_env(const char *key)
{
   size_t n = strlen(key);
   for (char *const *p = shell_environment; p && *p; p++)
      if (!strncmp(*p, key, n) && (*p)[n] == '=')
         return *p + n + 1;
   return NULL;
}
static int cli_child_request(const char *url, const char *token, const char *body)
{
   const char *port = strrchr(url, ':') + 1;
   int fd = aimee_core_socket_connect("127.0.0.1", port, 2000);
   assert(fd >= 0 && !aimee_core_socket_set_timeouts(fd, 5000, 2000));
   char header[1024], response[4096];
   int n =
       snprintf(header, sizeof header,
                "POST /v1/native/cli HTTP/1.1\r\nHost: 127.0.0.1:%s\r\nAuthorization: Bearer %s\r\n"
                "X-Aimee-Webuser: spoofed-actor\r\nsession_id: foreign-session\r\n"
                "Content-Length: %zu\r\n\r\n",
                port, token, strlen(body));
   assert(!aimee_core_socket_write_all(fd, header, (size_t)n));
   assert(!aimee_core_socket_write_all(fd, body, strlen(body)));
   size_t used = 0;
   long got;
   while ((got = aimee_core_socket_read(fd, response + used, sizeof response - used - 1)) > 0)
      used += (size_t)got;
   /* An early authentication refusal may close unread request bytes with RST.
    * Require a complete HTTP response, rather than mistaking that for timeout. */
   response[used] = 0;
   assert(used && strstr(response, "\r\n\r\n"));
   aimee_core_socket_close(fd);
   int status = 0;
   assert(sscanf(response, "HTTP/1.1 %d", &status) == 1);
   return status;
}
static void exercise_nested_cli(void)
{
   snprintf(outer_cli_attempt, sizeof outer_cli_attempt, "%s", execution_attempt);
   assert(!strncmp(outer_cli_attempt, "native-ir:", 10) && strlen(outer_cli_attempt) == 74);
   const char *token = fixture_env("AIMEE_API_BEARER");
   const char *url = fixture_env("AIMEE_API_ENDPOINT");
   assert(token && url && !strcmp(fixture_env("AIMEE_NATIVE_CLI"), "1"));
   snprintf(revoked_cli_token, sizeof revoked_cli_token, "%s", token);
   snprintf(revoked_cli_url, sizeof revoked_cli_url, "%s", url);
   const char *request = "{\"method\":\"mcp.call\",\"tool\":\"read_file\","
                         "\"arguments\":{\"amount\":123.123456789012345678901234567890},"
                         "\"_cli_request_id\":\"0123456789abcdef0123456789abcdef\"}";
   int before = dispatch_count;
   /* The producer really waits for its child HTTP request here. A producer
    * queue implementation deadlocks rather than reaching these assertions. */
   if (atomic_load(&cli_shell_probe) == 1)
   {
      assert(cli_child_request(url, token, request) == 200);
      assert(dispatch_count == before + 1);
      assert(cli_child_request(url, token, request) == 200);
      assert(dispatch_count == before + 1); /* lost-ACK replay never repeats effect */
   }
   else
   {
      assert(cli_child_request(url, token, request) != 200);
      assert(dispatch_count == before); /* final policy removed/changed definition */
   }
   const char *unadvertised =
       "{\"method\":\"mcp.call\",\"tool\":\"git_push\","
       "\"arguments\":{},\"_cli_request_id\":\"1123456789abcdef0123456789abcdef\"}";
   assert(cli_child_request(url, token, unadvertised) != 200);
   const char *foreign = "{\"method\":\"mcp.call\",\"tool\":\"read_file\","
                         "\"arguments\":{},\"session_id\":\"foreign\","
                         "\"_cli_request_id\":\"2123456789abcdef0123456789abcdef\"}";
   assert(cli_child_request(url, token, foreign) != 200);
   const char *foreign_cwd = "{\"method\":\"mcp.call\",\"tool\":\"read_file\","
                             "\"arguments\":{},\"cwd\":\"/work/bob\","
                             "\"_cli_request_id\":\"3123456789abcdef0123456789abcdef\"}";
   assert(cli_child_request(url, token, foreign_cwd) != 200);
   assert(dispatch_count == before + (atomic_load(&cli_shell_probe) == 1));
}
static void test_native_supervised_execution(const agent_t *selected,
                                             const request_context_t *owner)
{
   for (int mode = 0; mode <= 12; mode++)
   {
      atomic_store(&cli_shell_probe, mode >= 10 ? mode - 9 : 0);
      owner_tools(1);
      assert(!server_control_scope_bind(alice_controls));
      server_session_capture_t *capture =
          server_session_capture_open("session-a", "native-turn", "webuser:alice", "session-a");
      assert(capture && !server_session_capture_bind(capture));
      cJSON *schema =
          cJSON_Parse("{\"type\":\"object\",\"properties\":{\"amount\":{\"type\":\"number\"}}}");
      aimee_tool_t function = {
          .name = "read_file", .description = "admitted source schema", .schema = schema};
      aimee_tool_t children[2] = {function, function};
      if (mode >= 10)
         children[0].name = "bash";
      aimee_tool_t space = {.kind = AIMEE_TOOL_NAMESPACE,
                            .name = "aimee",
                            .children = children,
                            .n_children = mode >= 10 ? 2 : 1};
      aimee_block_t system = {.type = AIMEE_BLK_TEXT, .text = "host-owned system"};
      aimee_request_t profile = {.model = "selected-model",
                                 .frontend = AIMEE_WIRE_RESPONSES,
                                 .system = &system,
                                 .n_system = 1,
                                 .tools = &space,
                                 .n_tools = 1,
                                 .prompt_cache_key = "",
                                 .mutated = 1};
      request_context_t selected_owner = *owner;
      if (mode == 6)
         selected_owner.capabilities &= ~CAP_TOOL_EXECUTE;
      const server_native_tool_binding_t bindings[] = {
          {.space = "aimee", .name = "bash", .registry_name = "bash"},
          {.space = "aimee", .name = "read_file", .registry_name = "read_file"}};
      server_native_runtime_t *runtime =
          mode >= 10 ? server_native_runtime_open_bound_request(selected, &selected_owner,
                                                                "session-a", &profile, bindings, 2)
                     : server_native_runtime_open_request(selected, &selected_owner, "session-a",
                                                          &profile);
      assert(runtime);
      if (mode >= 10)
      {
         char launch[65];
         memset(launch, 'a', 64);
         launch[64] = 0;
         assert(!server_native_runtime_attest_launch(runtime, launch));
      }
      cJSON_Delete(schema);
      atomic_store(&expected_capture, capture);
      captured_rows = cJSON_CreateArray();
      atomic_store(&owned_profile_probe, 1);
      atomic_store(&snapshot_generation, 1);
      char response[1024];
      request(runtime, "/v1/responses", server_native_runtime_token(runtime), response);
      assert(strstr(response, "200 OK") && cJSON_GetArraySize(captured_rows) == 2);
      cJSON *input = cJSON_Parse("{\"amount\":123.123456789012345678901234567890}");
      aimee_block_t call = {.type = AIMEE_BLK_TOOL_USE,
                            .tool_id = "wire-call:original",
                            .tool_namespace = "aimee",
                            .tool_name = mode >= 10 ? "bash" : "read_file",
                            .tool_input = input};
      execute_worker_t wrong_thread = {.runtime = runtime, .call = &call};
      pthread_t worker;
      int rc = pthread_create(&worker, NULL, execute_wrong_thread, &wrong_thread);
      assert(!rc);
      rc = pthread_join(worker, NULL);
      assert(!rc && wrong_thread.rc == -1 && !wrong_thread.result.tool_result);
      /* A worker cannot borrow this producer's live browser sink. */
      request_context_set(owner);
      server_http_identity_bind_request_owner(owner);
      aimee_ir_session_set_sink(server_session_capture_sink, capture);
      atomic_int parent_cancel = 1, parent_scope_flag = 0;
      agent_request_cancel_scope_t parent_scope = {.flag = &parent_scope_flag};
      current_cancel_scope = &parent_scope;
      agent_set_request_cancel(&parent_cancel);
      aimee_block_t result = {0};
      int before = dispatch_count;
      rc = server_native_runtime_execute_tool(&call, &result, runtime);
      assert(rc == -1 && dispatch_count == before && !server_session_capture_failed(capture));
      atomic_store(&parent_cancel, 0);
      execution_mode = mode;
      pthread_t responder;
      if (mode == 9)
      {
         rc = pthread_create(&responder, NULL, reply_to_execution_question, NULL);
         assert(!rc);
      }
      protocol_captured = 7; /* A typed callback must preserve producer protocol state. */
      rc = server_native_runtime_execute_tool(&call, &result, runtime);
      if (mode == 9)
      {
         int joined = pthread_join(responder, NULL);
         assert(!joined);
      }
      assert(protocol_captured == 7);
      protocol_captured = 0;
      assert(agent_get_request_cancel() == &parent_cancel && current_cancel_scope == &parent_scope);
      assert(!effect_authorized && !current_model && workspace == &alice_workspace);
      assert(!atomic_load(&parent_cancel) && !request_context_get()->context_refused);
      assert(!strcmp(request_context_get()->request_id, owner->request_id));
      int dispatched = mode != 1 && mode != 2 && mode != 6 && mode != 7 && mode != 8;
      assert(dispatch_count == before + dispatched + (mode == 10));
      if (mode == 6)
      {
         assert(rc == -1 && !server_session_capture_failed(capture) && !result.tool_result);
         assert(cJSON_GetArraySize(captured_rows) == 2);
      }
      else if (mode >= 4 && mode != 9 && mode < 10)
      {
         assert(rc == -1 && server_session_capture_failed(capture) && !result.tool_result);
         assert(cJSON_GetArraySize(captured_rows) == 2); /* No invented outcome after an effect. */
      }
      else
      {
         assert(!rc && result.type == AIMEE_BLK_TOOL_RESULT &&
                result.tool_is_error == (mode == 1 || mode == 2 || mode == 3));
         assert(!strcmp(result.tool_id, "wire-call:original") &&
                cJSON_IsString(result.tool_result));
         assert(cJSON_GetArraySize(captured_rows) == (mode == 9 ? 5 : 3));
         char *wire = cJSON_PrintUnformatted(cJSON_GetArrayItem(captured_rows, mode == 9 ? 4 : 2));
         assert(wire && strstr(wire, "native-tool:") && strstr(wire, "wire-call:original"));
         free(wire);
      }
      if (mode >= 10)
      {
         assert(!shell_environment);
         assert(cli_child_request(revoked_cli_url, revoked_cli_token, "{}") == 401);
         char *source = cJSON_PrintUnformatted(captured_rows);
         assert(source && strstr(source, "\"name\":\"bash\"") &&
                strstr(source, "wire-call:original") && !strstr(source, "\"name\":\"read_file\""));
         free(source);
      }
      rc = server_native_runtime_execute_tool(&call, &result, runtime);
      assert(rc == -1 && dispatch_count == before + dispatched + (mode == 10));
      aimee_block_free_contents(&result);
      cJSON_Delete(input);
      agent_set_request_cancel(NULL);
      current_cancel_scope = NULL;
      aimee_ir_session_set_sink(NULL, NULL);
      request_context_set(NULL);
      server_http_identity_bind_request_owner(NULL);
      atomic_store(&reject_durable, 0);
      server_native_runtime_close(runtime);
      server_session_capture_close(capture);
      assert(server_control_scope_bind(NULL) == alice_controls);
      atomic_store(&expected_capture, NULL);
      atomic_store(&owned_profile_probe, 0);
      atomic_store(&snapshot_generation, 0);
      cJSON_Delete(captured_rows);
      captured_rows = NULL;
      scope_admitted = 0;
      atomic_store(&cli_shell_probe, 0);
   }
}

static int native_mcp_request(const server_native_runtime_t *runtime, const char *method,
                              const char *body, const char *headers, cJSON **out)
{
   const char *port = strrchr(server_native_runtime_url(runtime), ':') + 1;
   int fd = aimee_core_socket_connect("127.0.0.1", port, 2000);
   assert(fd >= 0 && !aimee_core_socket_set_timeouts(fd, 5000, 2000));
   char request[8192], response[16384];
   int length = snprintf(
       request, sizeof request,
       "%s /v1/mcp/call HTTP/1.1\r\nHost: 127.0.0.1:%s\r\nAuthorization: Bearer %s\r\n"
       "Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\n%s"
       "Content-Length: %zu\r\n\r\n%s",
       method, port, server_native_runtime_token(runtime), headers ? headers : "",
       body ? strlen(body) : 0, body ? body : "");
   assert(length > 0 && (size_t)length < sizeof request);
   assert(!aimee_core_socket_write_all(fd, request, (size_t)length));
   size_t used = 0;
   for (;;)
   {
      assert(used < sizeof response - 1);
      long n = aimee_core_socket_read(fd, response + used, sizeof response - used - 1);
      assert(n >= 0);
      if (!n)
         break;
      used += (size_t)n;
   }
   response[used] = 0;
   aimee_core_socket_close(fd);
   int status = 0;
   assert(sscanf(response, "HTTP/1.1 %d", &status) == 1);
   const char *payload = strstr(response, "\r\n\r\n");
   assert(payload);
   *out = payload[4] ? aimee_ir_json_parse_frame(payload + 4, strlen(payload + 4)) : NULL;
   return status;
}
static void test_native_bound_mcp(const agent_t *selected, const request_context_t *owner)
{
   const int modes[] = {0, 1, 2, 3, 4, 5, 9, 10};
   for (unsigned variant = 0; variant < sizeof modes / sizeof modes[0]; variant++)
   {
      int mode = modes[variant];
      owner_tools(1);
      assert(!server_control_scope_bind(alice_controls));
      server_session_capture_t *capture =
          server_session_capture_open("session-a", "native-turn", "webuser:alice", "session-a");
      assert(capture && !server_session_capture_bind(capture));
      cJSON *schema =
          cJSON_Parse("{\"type\":\"object\",\"properties\":{\"amount\":{\"type\":\"number\"}}}");
      aimee_tool_t function = {.name = "mcp__aimee__read_file",
                               .description = "admitted source schema",
                               .schema = schema};
      aimee_block_t system = {.type = AIMEE_BLK_TEXT, .text = "host-owned system"};
      aimee_request_t profile = {.model = "selected-model",
                                 .frontend = AIMEE_WIRE_RESPONSES,
                                 .system = &system,
                                 .n_system = 1,
                                 .tools = &function,
                                 .n_tools = 1,
                                 .prompt_cache_key = "",
                                 .mutated = 1};
      server_native_tool_binding_t binding = {.space = NULL,
                                              .name = "mcp__aimee__read_file",
                                              .registry_name = "read_file",
                                              .mcp_name = "exposed_read"};
      server_native_runtime_t *runtime = server_native_runtime_open_bound_request(
          selected, owner, "session-a", &profile, &binding, 1);
      assert(runtime);
      assert(server_native_runtime_attest_launch(runtime, "invalid-proof"));
      if (!variant)
      {
         server_native_tool_binding_t duplicate[] = {binding, binding};
         assert(!server_native_runtime_open_bound_request(selected, owner, "session-a", &profile,
                                                          duplicate, 2));
         server_native_tool_binding_t invalid = binding;
         invalid.registry_name = "not-a-registered-tool";
         assert(!server_native_runtime_open_bound_request(selected, owner, "session-a", &profile,
                                                          &invalid, 1));
         invalid = binding;
         invalid.name = "not-an-advertised-tool";
         assert(!server_native_runtime_open_bound_request(selected, owner, "session-a", &profile,
                                                          &invalid, 1));
         const char *digest = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
         server_native_execution_profile_t execution = {.sdk = "claude",
                                                        .build_digest = digest,
                                                        .launch_digest = digest,
                                                        .workspace_digest = digest};
         char baseline[65], changed[65];
         assert(!server_native_runtime_cache_scope(runtime, &execution, baseline));
         invalid = binding;
         invalid.mcp_name = "different_exposed_name";
         server_native_runtime_t *other = server_native_runtime_open_bound_request(
             selected, owner, "session-a", &profile, &invalid, 1);
         assert(other && !server_native_runtime_cache_scope(other, &execution, changed));
         assert(strcmp(baseline, changed));
         server_native_runtime_close(other);
         other = server_native_runtime_open_bound_request(selected, owner, "session-a", &profile,
                                                          &binding, 1);
         assert(other && !server_native_runtime_cache_scope(other, &execution, changed));
         assert(!strcmp(baseline, changed));
         server_native_runtime_close(other);
      }
      assert(!server_native_runtime_attest_launch(
          runtime, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
      assert(server_native_runtime_attest_launch(
          runtime, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
      cJSON_Delete(schema);
      assert(server_session_capture_bind(NULL) == capture);
      assert(server_control_scope_bind(NULL) == alice_controls);
      atomic_store(&expected_capture, capture);
      captured_rows = cJSON_CreateArray();
      atomic_store(&owned_profile_probe, 1);
      atomic_store(&flat_bound_probe, 2);
      atomic_store(&snapshot_generation, 1);
      char response[1024];
      request(runtime, "/v1/messages?beta=true&model=foreign", server_native_runtime_token(runtime),
              response);
      assert(strstr(response, "404") && !cJSON_GetArraySize(captured_rows));
      request(runtime, variant ? "/v1/responses" : "/v1/messages?beta=true",
              server_native_runtime_token(runtime), response);
      assert(strstr(response, "200 OK") && cJSON_GetArraySize(captured_rows) == 2);
      owner_tools(0); /* Worker must bind the captured owner, not this producer. */
      atomic_store(&mcp_execution, 1);
      atomic_store(&execution_mode, mode);
      cJSON *rpc = NULL;
      int rpc_status;
      rpc_status = native_mcp_request(runtime, "GET", NULL, NULL, &rpc);
      assert(rpc_status == 405 && !rpc);
      rpc_status = native_mcp_request(runtime, "POST",
                                      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                                      "\"params\":{\"protocolVersion\":\"2025-11-25\"}}",
                                      NULL, &rpc);
      assert(rpc_status == 200);
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(rpc, "result"),
                                                              "protocolVersion")),
                     "2025-11-25"));
      cJSON_Delete(rpc);
      rpc_status = native_mcp_request(
          runtime, "POST", "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", NULL,
          &rpc);
      assert(rpc_status == 202 && !rpc);
      rpc_status = native_mcp_request(runtime, "POST",
                                      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}",
                                      "MCP-Protocol-Version: 2025-11-25\r\n", &rpc);
      assert(rpc_status == 200);
      const cJSON *list = cJSON_GetObjectItem(cJSON_GetObjectItem(rpc, "result"), "tools");
      assert(cJSON_GetArraySize(list) == 1);
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(list, 0), "name")),
                     "exposed_read"));
      cJSON_Delete(rpc);
      rpc_status =
          native_mcp_request(runtime, "POST", "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}",
                             "MCP-Protocol-Version: unsupported\r\n", &rpc);
      assert(rpc_status == 400 && !rpc);
      rpc_status = native_mcp_request(
          runtime, "POST", "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}",
          "MCP-Protocol-Version: 2025-11-25\r\nMCP-Protocol-Version: 2025-11-25\r\n", &rpc);
      assert(rpc_status == 400 && !rpc);
      int before = atomic_load(&dispatch_count);
      pthread_t responder;
      if (mode == 9)
         assert(!pthread_create(&responder, NULL, reply_to_execution_question, NULL));
      const char *call =
          mode == 10
              ? "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/"
                "call\",\"params\":{\"name\":\"read_file\","
                "\"arguments\":{},\"_meta\":{\"claudecode/toolUseId\":\"wire-call:original\"}}}"
              : "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/"
                "call\",\"session_id\":\"session-b\","
                "\"params\":{\"name\":\"exposed_read\",\"cwd\":\"/"
                "forged\",\"arguments\":{\"amount\":123.12345678901235},"
                "\"_meta\":{\"claudecode/toolUseId\":\"wire-call:original\"}}}";
      rpc_status =
          native_mcp_request(runtime, "POST", call, "MCP-Protocol-Version: 2025-11-25\r\n", &rpc);
      assert(rpc_status == 200);
      if (mode == 9)
         assert(!pthread_join(responder, NULL));
      assert(atomic_load(&dispatch_count) ==
             before + (mode == 0 || mode == 3 || mode == 4 || mode == 5 || mode == 9));
      assert(!server_session_capture_current() && workspace == &bob_workspace && job_id == 22);
      if (mode == 4 || mode == 5 || mode == 10)
      {
         assert(cJSON_IsObject(cJSON_GetObjectItem(rpc, "error")));
         assert(server_session_capture_failed(capture) && cJSON_GetArraySize(captured_rows) == 2);
      }
      else
      {
         const cJSON *result = cJSON_GetObjectItem(rpc, "result");
         assert(cJSON_IsObject(result));
         assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")) ==
                (mode == 1 || mode == 2 || mode == 3));
         assert(!server_session_capture_failed(capture));
         assert(cJSON_GetArraySize(captured_rows) == (mode == 9 ? 5 : 3));
         /* A duplicate reference never dispatches or invents a second result. */
         cJSON_Delete(rpc);
         rpc_status = native_mcp_request(runtime, "POST", call, NULL, &rpc);
         assert(rpc_status == 200);
         assert(cJSON_IsObject(cJSON_GetObjectItem(rpc, "error")) &&
                server_session_capture_failed(capture));
      }
      cJSON_Delete(rpc);
      atomic_store(&reject_durable, 0);
      server_native_runtime_close(runtime);
      server_session_capture_close(capture);
      atomic_store(&mcp_execution, 0);
      atomic_store(&expected_capture, NULL);
      atomic_store(&owned_profile_probe, 0);
      atomic_store(&flat_bound_probe, 0);
      atomic_store(&snapshot_generation, 0);
      cJSON_Delete(captured_rows);
      captured_rows = NULL;
      scope_admitted = 0;
      owner_tools(1);
   }
}

static void test_owned_native_context(const agent_t *selected, const request_context_t *owner)
{
   owner_tools(1);
   aimee_block_t system = {.type = AIMEE_BLK_TEXT, .text = "host-owned system"};
   aimee_block_t stale = {.type = AIMEE_BLK_TEXT, .text = "stale initial profile history"};
   aimee_message_t message = {.role = "user", .blocks = &stale, .n_blocks = 1};
   aimee_tool_t function = {.name = "read_file"};
   aimee_tool_t tools = {
       .kind = AIMEE_TOOL_NAMESPACE, .name = "aimee", .children = &function, .n_children = 1};
   aimee_request_t seed = {.model = "selected-model",
                           .frontend = AIMEE_WIRE_RESPONSES,
                           .system = &system,
                           .n_system = 1,
                           .messages = &message,
                           .n_messages = 1,
                           .tools = &tools,
                           .n_tools = 1,
                           .prompt_cache_key = "",
                           .max_tokens = 128,
                           .has_max_tokens = 1,
                           .mutated = 1};
   server_native_runtime_t *missing =
       server_native_runtime_open_request(selected, owner, "session-a", &seed);
   assert(!missing); /* A profile cannot replace missing durable history ownership. */
   server_session_capture_t *capture =
       server_session_capture_open("private-child", "native-turn", "webuser:alice", "session-a");
   assert(capture && !server_session_capture_bind(capture));
   seed.model = "unselected-model";
   missing = server_native_runtime_open_request(selected, owner, "session-a", &seed);
   assert(!missing && !server_session_capture_failed(capture));
   seed.model = "selected-model";
   seed.raw_bytes = "native request";
   seed.raw_length = strlen(seed.raw_bytes);
   missing = server_native_runtime_open_request(selected, owner, "session-a", &seed);
   assert(!missing && !server_session_capture_failed(capture));
   seed.raw_bytes = NULL;
   seed.raw_length = 0;
   seed.raw = cJSON_Parse("{\"conversation\":\"SDK private history\"}");
   missing = server_native_runtime_open_request(selected, owner, "session-a", &seed);
   assert(!missing && !server_session_capture_failed(capture));
   cJSON_Delete(seed.raw);
   seed.raw = NULL;
   aimee_request_t profile = {0};
   int cloned = aimee_ir_request_clone(&seed, &profile);
   assert(!cloned);
   server_native_runtime_t *runtime =
       server_native_runtime_open_request(selected, owner, "session-a", &profile);
   assert(runtime);
   server_native_execution_profile_t execution = {
       .sdk = "codex",
       .build_digest = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
       .launch_digest = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
       .workspace_digest = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"};
   char baseline[65], changed[65];
   int scoped = server_native_runtime_cache_scope(runtime, &execution, baseline);
   assert(!scoped);
   free(profile.system[0].text);
   profile.system[0].text = strdup("different host profile");
   server_native_runtime_t *other =
       server_native_runtime_open_request(selected, owner, "session-a", &profile);
   assert(other);
   scoped = server_native_runtime_cache_scope(other, &execution, changed);
   assert(!scoped && strcmp(baseline, changed));
   aimee_request_free(&profile);
   assert(server_session_capture_bind(NULL) == capture);
   atomic_store(&expected_capture, capture);
   captured_rows = cJSON_CreateArray();
   atomic_store(&owned_profile_probe, 1);
   char response[1024];
   for (int i = 1; i <= 2; i++)
   {
      atomic_store(&snapshot_generation, i);
      request(runtime, "/v1/responses", server_native_runtime_token(runtime), response);
      assert(strstr(response, "200 OK") && !server_session_capture_failed(capture));
   }
   /* Identical provider IDs/arguments from two distinct original responses
    * are ambiguous; an SDK observation cannot select whichever ran last. */
   cJSON *ambiguous_input = cJSON_Parse("{\"amount\":123.123456789012345678901234567890}");
   aimee_block_t ambiguous = {.type = AIMEE_BLK_TOOL_USE,
                              .tool_id = "wire-call:original",
                              .tool_namespace = "aimee",
                              .tool_name = "read_file",
                              .tool_input = ambiguous_input};
   server_native_tool_claim_t no_claim = {0};
   int claimed = server_native_runtime_claim_tool(runtime, &ambiguous, &no_claim);
   assert(claimed == -1 && !no_claim.original.n_messages);
   assert(server_native_runtime_claim_reference(runtime, "wire-call:original", "aimee", "read_file",
                                                &no_claim) < 0);
   assert(!no_claim.original.n_messages);
   cJSON_Delete(ambiguous_input);
   /* One authoritative callback stays in flight. A second model generation
    * cannot fork its running snapshot or execute under the first admission. */
   int before = atomic_load(&calls);
   atomic_store(&wait_for_cancel, 1);
   atomic_store(&cancelled, 0);
   int fd = send_request(runtime, "/v1/responses", server_native_runtime_token(runtime));
   for (int i = 0; i < 300 && atomic_load(&calls) == before; i++)
      usleep(10000);
   assert(atomic_load(&calls) == before + 1);
   request(runtime, "/v1/responses", server_native_runtime_token(runtime), response);
   assert(strstr(response, "502") && atomic_load(&calls) == before + 1);
   request(other, "/v1/responses", server_native_runtime_token(other), response);
   assert(strstr(response, "502") && atomic_load(&calls) == before + 1);
   server_native_runtime_close(other);
   close(fd);
   for (int i = 0; i < 300 && !atomic_load(&cancelled); i++)
      usleep(10000);
   assert(atomic_load(&cancelled));
   server_native_runtime_close(runtime); /* Joins the callback before profile/capture release. */
   atomic_store(&wait_for_cancel, 0);
   atomic_store(&owned_profile_probe, 0);
   atomic_store(&snapshot_generation, 0);
   server_session_capture_close(capture);
   atomic_store(&expected_capture, NULL);
   cJSON_Delete(captured_rows);
   captured_rows = NULL;
}

static void test_source_snapshot_refusal(const agent_t *selected, const request_context_t *owner)
{
   for (int failure = 1; failure <= 2; failure++)
   {
      owner_tools(1);
      server_session_capture_t *capture =
          server_session_capture_open("private-child", "native-turn", "webuser:alice", "session-a");
      assert(capture && !server_session_capture_bind(capture));
      atomic_store(&snapshot_fault, failure);
      server_native_runtime_t *runtime = server_native_runtime_open(selected, owner, "session-a");
      assert(!runtime && server_session_capture_failed(capture));
      server_session_capture_close(capture);
   }
   atomic_store(&snapshot_fault, 0);
}

static void test_private_source_revocation(const agent_t *selected, const request_context_t *owner)
{
   for (int failure = 1; failure <= 2; failure++)
   {
      owner_tools(1);
      server_session_capture_t *capture =
          server_session_capture_open("private-child", "native-turn", "webuser:alice", "session-a");
      assert(capture && !server_session_capture_bind(capture));
      server_native_runtime_t *runtime = server_native_runtime_open(selected, owner, "session-a");
      assert(runtime && server_session_capture_bind(NULL) == capture);
      atomic_store(&destination_invalid, failure);
      int before = atomic_load(&calls);
      char response[1024];
      request(runtime, failure == 1 ? "/v1/responses" : "/v1/mcp/call",
              server_native_runtime_token(runtime), response);
      assert(strstr(response, "502") && atomic_load(&calls) == before);
      assert(server_session_capture_failed(capture) && !aimee_ir_session_capture_failed());
      atomic_store(&destination_invalid, 0);
      request(runtime, "/v1/responses", server_native_runtime_token(runtime), response);
      assert(strstr(response, "502") && atomic_load(&calls) == before);
      server_native_runtime_close(runtime);
      server_session_capture_close(capture);
   }
}

int main(void)
{
   agent_t selected = {.enabled = 1};
   strcpy(selected.name, "selected");
   strcpy(selected.registration, "selected-account");
   strcpy(selected.model, "selected-model");
   strcpy(selected.auth_type, "codex-oauth");
   request_context_t a = {.vault_transport = ATTEST_WEBCHAT_TRUSTED,
                          .capabilities = CAP_CHAT | CAP_TOOL_EXECUTE};
   strcpy(a.vault_principal, "webuser:alice");
   strcpy(a.principal, "alice");
   strcpy(a.idempotency_key, "browser-turn-only");
   strcpy(a.request_id, "browser-request-only");
   a.capabilities |= CAP_WORKFLOW_ADMIN | CAP_GRANT_ADMIN | CAP_SHADOW_ADMIN;
   request_context_t b = a;
   strcpy(b.vault_principal, "webuser:bob");
   strcpy(b.principal, "bob");
   b.capabilities = CAP_CHAT;
   assert(!server_native_runtime_open(&selected, NULL, "session-a"));
   assert(!server_native_runtime_open(&selected, &b, "session-a"));
   owner_tools(1);
   atomic_store(&malformed_source, 1);
   assert(!server_native_runtime_open(&selected, &a, "session-a"));
   atomic_store(&malformed_source, 0);
   alice_controls = server_control_scope_open("session-a", "native-turn", "webuser:alice");
   assert(alice_controls && !server_control_scope_bind(alice_controls));
   server_native_runtime_t *alice = server_native_runtime_open(&selected, &a, "session-a");
   assert(server_control_scope_bind(NULL) == alice_controls);
   owner_tools(0);
   server_native_runtime_t *bob = server_native_runtime_open(&selected, &b, "session-b");
   assert(alice && bob);
   test_cache_profile(&selected, &a, alice);
   /* Destroy/change every original borrowed host value after the owner copy.
    * Callback workers must retain each captured policy and source independently. */
   owner_tools(1);
   agent_tools_set_dispatch_role("changed-after-open");
   agent_tools_set_active_toolset("coding");
   agent_source_authority_tls_set(1, "/foreign", "foreign.c");
   setenv("AIMEE_ACTIVE_TOOLSET", "foreign-toolset", 1);
   setenv("AIMEE_DELEGATE_SOURCE_PATHS", "foreign-process-path", 1);

   assert(strcmp(server_native_runtime_binding(alice), server_native_runtime_binding(bob)));
   assert(strlen(server_native_runtime_token(alice)) == 64);
   char response[1024];
   for (int i = 0; i < 16; i++)
   {
      request(alice, "/v1/responses", server_native_runtime_token(alice), response);
      assert(strstr(response, "200 OK") && strstr(response, "webuser:alice") &&
             strstr(response, "session-a") && !strstr(response, "forged"));
      assert(!strstr(response, "upstream-secret"));
      request(bob, "/v1/responses", server_native_runtime_token(bob), response);
      assert(strstr(response, "webuser:bob") && strstr(response, "session-b"));
   }
   assert(!request_context_get());
   assert(!current_credentials.vault_principal[0]);
   int before = atomic_load(&calls);
   request(alice, "/v1/responses", server_native_runtime_token(bob), response);
   assert(strstr(response, "401") && atomic_load(&calls) == before);
   request(alice, "/v1/chat/control", server_native_runtime_token(alice), response);
   assert(strstr(response, "404") && atomic_load(&calls) == before);
   request(alice, "/v1/config/set", server_native_runtime_token(alice), response);
   assert(strstr(response, "404") && atomic_load(&calls) == before);
   request(bob, "/v1/mcp/call", server_native_runtime_token(bob), response);
   assert(strstr(response, "404") && atomic_load(&calls) == before);
   request(alice, "/v1/mcp/call", server_native_runtime_token(alice), response);
   assert(strstr(response, "200 OK") && strstr(response, "webuser:alice"));
   before = atomic_load(&calls);
   atomic_store(&source_changed, 1);
   request(alice, "/v1/responses", server_native_runtime_token(alice), response);
   assert(strstr(response, "502") && atomic_load(&calls) == before);
   atomic_store(&source_changed, 0);
   atomic_store(&session_removed, 1);
   request(alice, "/v1/responses", server_native_runtime_token(alice), response);
   assert(strstr(response, "502") && atomic_load(&calls) == before);
   atomic_store(&session_removed, 0);
   atomic_store(&wait_for_cancel, 1);
   int fd = send_request(alice, "/v1/responses", server_native_runtime_token(alice));
   for (int i = 0; i < 300 && atomic_load(&calls) == before; i++)
      usleep(10000);
   assert(atomic_load(&calls) > before);
   aimee_core_socket_shutdown(fd);
   aimee_core_socket_close(fd);
   for (int i = 0; i < 300 && !atomic_load(&cancelled); i++)
      usleep(10000);
   assert(atomic_load(&cancelled));
   server_native_runtime_close(alice);
   server_native_runtime_close(bob);
   atomic_store(&wait_for_cancel, 0);
   owner_tools(1);
   agent_t api_selected = selected;
   strcpy(api_selected.provider, "openai");
   strcpy(api_selected.auth_type, "api_key");
   assert(!server_control_scope_bind(alice_controls));
   server_native_runtime_t *api = server_native_runtime_open(&api_selected, &a, "session-a");
   assert(server_control_scope_bind(NULL) == alice_controls);
   assert(api);
   request(api, "/v1/responses", server_native_runtime_token(api), response);
   assert(strstr(response, "200 OK") && !strstr(response, "api-secret"));
   request(api, "/v1/mcp/call", server_native_runtime_token(api), response);
   assert(strstr(response, "200 OK") && !strstr(response, "api-secret"));
   before = atomic_load(&calls);
   atomic_store(&source_changed, 1);
   request(api, "/v1/responses", server_native_runtime_token(api), response);
   assert(strstr(response, "502") && atomic_load(&calls) == before);
   server_native_runtime_t *rotated = server_native_runtime_open(&api_selected, &a, "session-a");
   assert(rotated &&
          strcmp(server_native_runtime_binding(api), server_native_runtime_binding(rotated)));
   request(rotated, "/v1/responses", server_native_runtime_token(rotated), response);
   assert(strstr(response, "200 OK"));
   server_native_runtime_close(rotated);
   server_native_runtime_close(api);
   atomic_store(&source_changed, 0);
   agent_t claude_selected = selected;
   strcpy(claude_selected.provider, "anthropic");
   strcpy(claude_selected.auth_type, "claude-oauth");
   assert(!server_control_scope_bind(alice_controls));
   server_native_runtime_t *claude = server_native_runtime_open(&claude_selected, &a, "session-a");
   assert(server_control_scope_bind(NULL) == alice_controls);
   assert(claude);
   request(claude, "/v1/messages", server_native_runtime_token(claude), response);
   assert(strstr(response, "200 OK") && !strstr(response, "claude-secret"));
   request(claude, "/v1/mcp/call", server_native_runtime_token(claude), response);
   assert(strstr(response, "200 OK"));
   before = atomic_load(&calls);
   atomic_store(&source_changed, 1);
   request(claude, "/v1/messages", server_native_runtime_token(claude), response);
   assert(strstr(response, "502") && atomic_load(&calls) == before);
   rotated = server_native_runtime_open(&claude_selected, &a, "session-a");
   assert(rotated &&
          strcmp(server_native_runtime_binding(claude), server_native_runtime_binding(rotated)));
   request(rotated, "/v1/messages", server_native_runtime_token(rotated), response);
   assert(strstr(response, "200 OK"));
   server_native_runtime_close(rotated);
   server_native_runtime_close(claude);
   atomic_store(&source_changed, 0);
   test_retained_capture(&selected, &a);
   test_native_preframe_error(&selected, &a);
   test_native_original_catalog(&selected, &a);
   test_native_supervised_execution(&selected, &a);
   test_native_bound_mcp(&selected, &a);
   test_owned_native_context(&selected, &a);
   test_source_snapshot_refusal(&selected, &a);
   test_private_source_revocation(&selected, &a);
   unsetenv("AIMEE_ACTIVE_TOOLSET");
   unsetenv("AIMEE_DELEGATE_SOURCE_PATHS");
   agent_source_authority_snapshot_t absent = {0};
   agent_source_authority_tls_restore(&absent);
   agent_tools_denied_set(NULL, 0);
   puts("native runtime: owned actor/model/permissions/source, resource leases and cancellation "
        "passed");
   server_control_scope_close(alice_controls);
   cJSON_Delete(source_rows);
   return 0;
}
