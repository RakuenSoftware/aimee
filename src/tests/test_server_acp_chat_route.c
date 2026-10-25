/* Actual public chat worker, canonical capture and strong ACP owner. Only
 * configuration/storage/workspace services are fixture providers; no model or
 * provider SDK is used. The child below is a real descriptor-launched process. */
#define main                                existing_chat_ir_fixture_main
#define agent_request_cancelled             original_chat_cancelled
#define db1_session_ir_running_history_load unused_chat_history_load
#define write_all                           original_chat_write_all
#define db1_session_ir_append               original_chat_append
#define db1_session_ir_page                 original_chat_page
#define db1_server_session_owner_get        original_chat_get
#define db1_server_session_create           original_chat_create
#include "test_server_chat_ir.c"
#undef write_all
#undef db1_session_ir_append
#undef db1_session_ir_page
#undef db1_server_session_owner_get
#undef db1_session_ir_running_history_load
#undef db1_delegate_execution_terminal_lookup
#undef db1_delegate_execution_experience_record
#undef db1_session_ir_append_verified
#undef db1_session_native_source_append
#undef db1_server_session_create
#undef db1_interaction_event_record
#undef db1_execution_test_grade_record
#undef main
#undef agent_request_cancelled
/* The full-worker fixture reuses the protocol child and unrelated peers, but
 * links real workspace/runtime owners and supplies a multi-session store peer. */
#ifdef AIMEE_ACP_FULL_WORKER_FIXTURE
#define db1_server_session_create           full_unused_route_session_create
#define db1_server_session_owner_get        full_unused_route_session_get
#define db1_session_ir_append               full_unused_route_ir_append
#define db1_session_ir_page                 full_unused_route_ir_page
#define db1_session_ir_running_history_load full_unused_route_history_load
#endif
#include "agent_config.h"
#include "agent_exec.h"
#include "persona.h"
#include "request_context.h"
#include "provider_cli_adapter.h"
#include "primary_session_adapter.h"
#include "cli_mcp_serve.h"
#include "cli_client.h"
#include "server_http_internal.h"
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include "cmd_agent_delegate_impl.h"
#include "modules/workspace/workspace_turn.h"
#include "platform_test_util.h"
#include "compute_pool.h"
#include "aimee_sha256.h"
#include "module_json_call.h"
#include "module_commands.h"
#include "agent_coord.h"
#include "server_coord_dispatcher.h"
#include <aimee/tools/agent_tools.h>
#include "modules/tools/tool_catalog_data.h"
#include <aimee/audit/audit_worm.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>

/* Route-catalog service peer: exact existing MCP rows; unrelated operator
 * metadata omitted. Real rh_cli_manifest builds all command/marshal data. */
const http_route_t g_v1_routes[] = {
    {"GET", "/v1/mcp/tools_list", NULL, RM_EXACT, "mcp.tools_list", 0, rh_dispatch_op},
    {"POST", "/v1/mcp/call", NULL, RM_EXACT, "mcp.call", 0, rh_dispatch_op},
    {0}};
int rh_dispatch_op(const route_req_t *request, char *output, int capacity)
{
   (void)request;
   (void)output;
   (void)capacity;
   assert(!"route metadata must not execute operator dispatch");
   return 500;
}
int rh_dispatch_op_async(const route_req_t *request, char *output, int capacity)
{
   return rh_dispatch_op(request, output, capacity);
}
/* No module-added command metadata is registered by this fixture. */
int aimee_module_commands_refresh(int ttl_ms)
{
   (void)ttl_ms;
   return 0;
}
int err_json(char *output, int capacity, int status, const char *message)
{
   (void)output;
   (void)capacity;
   (void)status;
   (void)message;
   assert(!"bounded real CLI manifest must serialize successfully");
   return 500;
}
static char workspace[512], executable[4096], command[8192];
static int inline_selection, child_mode;
static int headless, expected_controls, actual_controls;
static atomic_int route_cancel;
static const char file_content[] = "first line\nsecond line\nthird line\n";
static cJSON *stored_rows;
static const char user_text[] = "Run the owned ACP public route fixture.";
static const char next_user_text[] = "Continue the owned ACP public route fixture.";
static const char system_text[] = "Host ACP fixture identity.";
static char worker_error[512];
static char *last_business_text;
static int last_business_rc;
static int lifetime_invalidation_seen;

/* The test's approved workspace provider returns a retained descriptor only
 * for the actual registered root. The ACP owner must retain/revalidate it. */
int workspace_turn_resource_identity(const char *cwd, int *fd, char digest[65])
{
   *fd = -1;
   digest[0] = 0;
   if (!cwd || strcmp(cwd, workspace))
      return -1;
   *fd = open(workspace, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   if (*fd < 0)
      return -1;
   memset(digest, 'a', 64);
   digest[64] = 0;
   return 0;
}
#ifndef AIMEE_ACP_FULL_WORKER_FIXTURE
/* Shared-primary fixture: container execution is covered by the real full
 * worker target. These dependencies must never affirm a container here. */
int workspace_turn_source_identity(const char *cwd, int *fd, char digest[65])
{
   (void)cwd;
   (void)fd;
   (void)digest;
   abort();
}
int workspace_turn_container_identity(char id[65], char image[72])
{
   (void)id;
   (void)image;
   abort();
}
int workspace_turn_container_retire(void)
{
   abort();
}
int workspace_turn_container_forwarder(unsigned port)
{
   (void)port;
   abort();
}
int workspace_turn_container_transport(const char **binary, const char **endpoint)
{
   (void)binary;
   (void)endpoint;
   abort();
}
int workspace_turn_container_file_owner_start(int exe, int cwd, const char *command,
                                              int (*validate)(void *), void *context,
                                              long long deadline)
{
   (void)exe;
   (void)cwd;
   (void)command;
   (void)validate;
   (void)context;
   (void)deadline;
   abort();
}
int workspace_turn_container_prepared(const char **command, const char **home, const char **mcp)
{
   (void)command;
   (void)home;
   (void)mcp;
   abort();
}
int workspace_turn_container_file(const char *path, int writing, const char *content, size_t length,
                                  int timeout, char **out, size_t *size, int *unknown)
{
   (void)path;
   (void)writing;
   (void)content;
   (void)length;
   (void)timeout;
   (void)out;
   (void)size;
   (void)unknown;
   abort();
}
#endif
int workspace_turn_bind_active(const char *cwd)
{
   assert(!strcmp(cwd, workspace));
   return 0;
}
void workspace_turn_unbind_active(void)
{
}
const char *workspace_turn_active_cwd(void)
{
   return NULL;
}
const char *workspace_turn_drift_notice(void)
{
   return NULL;
}
int workspace_turn_reject_foreign_cwd(int bound, int trusted, const char *cwd)
{
   (void)bound;
   (void)trusted;
   return strcmp(cwd, workspace) != 0;
}

static void route_configured_agent(agent_t *agent)
{
   memset(agent, 0, sizeof *agent);
   snprintf(agent->name, sizeof agent->name, "configured-acp");
   snprintf(agent->backend, sizeof agent->backend, "%s", AGENT_BACKEND_PROVIDER_CLI);
   snprintf(agent->cli_kind, sizeof agent->cli_kind, "acp");
   assert(snprintf(agent->cli_cmd, sizeof agent->cli_cmd, "%s %s", executable, command) <
          (int)sizeof agent->cli_cmd);
   agent->enabled = agent->tools_enabled = agent->write_capable = 1;
   agent->max_turns = child_mode == 13 ? 3 : -1;
   agent->timeout_ms = child_mode >= 23 ? 5000 : 1500;
   if (child_mode == 11)
      agent->tools_enabled = 0;
}
int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof *cfg);
   if (inline_selection)
   {
      assert(!delegate_add_inline_acp_agent(cfg, executable, command, "code", cfg->default_agent,
                                            sizeof cfg->default_agent));
      cfg->agents[0].timeout_ms = child_mode >= 23 ? 5000 : 1500;
   }
   else
   {
      cfg->agent_count = 1;
      route_configured_agent(cfg->agents);
      snprintf(cfg->default_agent, sizeof cfg->default_agent, "%s", cfg->agents[0].name);
   }
   return 0;
}
int agent_registry_find(const char *name, agent_t *out)
{
   if (strcmp(name, "configured-acp"))
      return -1;
   route_configured_agent(out);
   if (child_mode == 22)
      snprintf(out->cli_cmd, sizeof out->cli_cmd, "/bin/false");
   return 0;
}
/* No host-service source is issued by this headed public-route fixture. */
int server_native_delegate_source_runtime_prepare(const char *role, const agent_t *original,
                                                  const agent_t *selected,
                                                  const aimee_request_t *seed,
                                                  const aimee_request_t *profile,
                                                  const char *session)
{
   (void)role;
   (void)original;
   (void)selected;
   (void)seed;
   (void)profile;
   (void)session;
   return -1;
}
int server_native_delegate_source_runtime_consume(const agent_t *selected,
                                                  const request_context_t *context,
                                                  const aimee_request_t *profile,
                                                  const char *session)
{
   (void)selected;
   (void)context;
   (void)profile;
   (void)session;
   assert(!"no service source can be consumed by a headed fixture");
   return -1;
}
int session_primary_get(const char *sid, char *out, size_t n)
{
   assert(!strcmp(sid, "session-one"));
   (void)out;
   (void)n;
   return 0;
}
int session_persona_get(const char *sid, char *out, size_t n)
{
   (void)sid;
   snprintf(out, n, "engineer");
   return 1;
}
int persona_load(const char *root, const char *name, persona_t *out)
{
   (void)root;
   assert(!strcmp(name, "engineer"));
   memset(out, 0, sizeof *out);
   out->persona_text = strdup(system_text);
   out->principles_text = strdup("");
   return 0;
}
void persona_free(persona_t *p)
{
   free(p->persona_text);
   free(p->principles_text);
}
int agent_run_with_tools(agent_config_t *cfg, const char *role, const char *system,
                         const char *prompt, int tokens, agent_result_t *out)
{
   (void)cfg;
   (void)role;
   (void)system;
   (void)prompt;
   (void)tokens;
   snprintf(out->error, sizeof out->error,
            "unowned provider engine is unavailable in this ACP-only fixture");
   return AGENT_RC_CONTEXT_REFUSED;
}
int route_observed_host_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   int rc = provider_cli_adapter_host_execute(cfg, out);
   free(last_business_text);
   last_business_text = NULL;
   last_business_rc = rc;
   if (!rc)
   {
      assert(out->success);
      last_business_text = aimee_ir_response_text_alloc(&out->canonical_response);
      assert(last_business_text && out->canonical_response.usage_unobserved);
   }
   return rc;
}
int server_native_execute_owned(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   (void)cfg;
   (void)out;
   assert(!"ACP must use the actual strong external-agent owner");
   return -1;
}

int write_all(int fd, const char *text, size_t size)
{
   if (size == 1 && text[0] == '\n')
      return 0;
   cJSON *frame = cJSON_ParseWithLength(text, size);
   assert(frame);
   if (cJSON_GetObjectItemCaseSensitive(frame, "event"))
   {
      const cJSON *event = cJSON_GetObjectItemCaseSensitive(frame, "event");
      const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "kind"));
      if (kind && !strcmp(kind, "message"))
      {
         const cJSON *payload = cJSON_GetObjectItem(event, "payload"),
                     *block = cJSON_GetArrayItem(cJSON_GetObjectItem(payload, "blocks"), 0);
         const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(block, "tool_id"));
         if (id && !strncmp(id, "acp-mcp:", 8))
         {
            if (child_mode == 29)
            {
               lifetime_invalidation_seen++;
               request_context_t changed = *request_context_get();
               snprintf(changed.vault_principal, sizeof changed.vault_principal, "foreign-actor");
               request_context_set(&changed);
            }
            if (child_mode == 30)
            {
               lifetime_invalidation_seen++;
               atomic_store(&route_cancel, 1);
            }
         }
      }
      if (kind && !strcmp(kind, "control_request"))
      {
         const char *request =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "request_id"));
         assert(request);
         actual_controls++;
         assert(!headless);
         cJSON *reply = cJSON_Parse("{\"decision\":\"allow\"}");
         assert(server_control_reply("session-one", request, "foreign-actor", reply) < 0);
         assert(!server_control_reply("session-one", request, "alice", reply));
         cJSON_Delete(reply);
      }
      cJSON_Delete(frame);
      return original_chat_write_all(fd, text, size);
   }
   const char *error = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "message"));
   if (error)
      snprintf(worker_error, sizeof worker_error, "%s", error);
   cJSON_Delete(frame);
   return 0;
}
int db1_server_session_create(const char *sid, const char *client, const char *actor)
{
   (void)client;
   assert(!strcmp(sid, "session-one") && !strcmp(actor, "alice"));
   return 0;
}
int db1_server_session_owner_get(const char *sid, db1_server_session_owner_t *row)
{
   int rc = original_chat_get(sid, row);
   if (!rc)
      snprintf(row->created_at, sizeof row->created_at, "fixture-incarnation");
   return rc;
}
int db1_session_ir_append(const char *sid, const char *id, const char *turn, const char *actor,
                          const char *payload, char sequence[32])
{
   int rc = original_chat_append(sid, id, turn, actor, payload, sequence);
   if (rc)
      return rc;
   cJSON *row = cJSON_CreateObject();
   assert(row);
   assert(cJSON_AddNumberToObject(row, "version", 1) &&
          cJSON_AddStringToObject(row, "session_id", sid) &&
          cJSON_AddStringToObject(row, "sequence", sequence) &&
          cJSON_AddStringToObject(row, "event_id", id) &&
          cJSON_AddStringToObject(row, "turn_id", turn) &&
          cJSON_AddStringToObject(row, "actor", actor) &&
          cJSON_AddItemToObject(row, "event", cJSON_Parse(payload)) &&
          cJSON_AddItemToArray(stored_rows, row));
   return 0;
}
char *db1_session_ir_page(const char *sid, const char *after, int limit, const char *actor)
{
   assert(!strcmp(sid, "session-one") && !strcmp(actor, "alice"));
   cJSON *page = cJSON_CreateArray();
   assert(page);
   int start = atoi(after);
   for (int i = start; i < cJSON_GetArraySize(stored_rows) && i < start + limit; i++)
      assert(cJSON_AddItemToArray(page, cJSON_Duplicate(cJSON_GetArrayItem(stored_rows, i), 1)));
   char *wire = cJSON_PrintUnformatted(page);
   cJSON_Delete(page);
   return wire;
}
int db1_session_ir_running_history_load(const char *sid, const char *actor, const char *turn,
                                        char **out)
{
   assert(!strcmp(sid, "session-one") && !strcmp(actor, "alice") && turn[0]);
   cJSON *history = cJSON_CreateArray();
   assert(history);
   const cJSON *row;
   cJSON_ArrayForEach(row, stored_rows)
   {
      const cJSON *event = cJSON_GetObjectItemCaseSensitive(row, "event");
      const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "kind"));
      if (kind && !strcmp(kind, "message"))
         assert(cJSON_AddItemToArray(
             history, cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(event, "payload"), 1)));
   }
   *out = cJSON_PrintUnformatted(history);
   cJSON_Delete(history);
   return *out ? 0 : -1;
}

/* Unrelated scheduling/presence/primary-adapter peers do not execute work. */
int server_compute_budget_acquire(server_ctx_t *server)
{
   (void)server;
   return 1;
}
void server_compute_budget_release(server_ctx_t *server, int count)
{
   (void)server;
   assert(count == 1);
}
void compute_pool_set_job(pool_job_kind_t kind, const char *format, ...)
{
   (void)format;
   assert(kind == POOL_JOB_CHAT);
}
void presence_emit_turn_delta(const char *sid, const char *turn, const char *json)
{
   (void)sid;
   (void)turn;
   (void)json;
   assert(!"IR delivery must not use legacy presence deltas");
}
const workspace_turn_mirror_lease_t *
workspace_turn_mirror_bind(const workspace_turn_mirror_lease_t *lease)
{
   assert(!lease);
   return NULL;
}
void workspace_turn_mirror_release(workspace_turn_mirror_lease_t *lease)
{
   assert(!lease);
}
int workspace_turn_container_bound(void)
{
   return 0;
}
void server_coord_dispatcher_notify(void)
{
   assert(!"no coordination job in public chat fixture");
}
void ingress_preinject_set_session_id(const char *sid)
{
   assert(sid && (!sid[0] || !strcmp(sid, "session-one")));
}
void ingress_preinject_set_health_turn_id(const char *turn)
{
   (void)turn;
}
void ingress_preinject_set_task_requirements(const cJSON *request)
{
   assert(request);
}
int ingress_preinject_acquire_send_guard(void **guard)
{
   *guard = NULL;
   return -1;
}
void ingress_preinject_release_send_guard(void *guard)
{
   assert(!guard);
}

int primary_session_adapter_can_handle(const agent_t *agent)
{
   assert(agent && !strcmp(agent->cli_kind, "acp"));
   return 0;
}

int primary_session_adapter_turn(const primary_session_request_t *req, agent_result_t *out,
                                 char *session_id_out, size_t session_id_len)
{
   assert(!"ACP cannot fall through another primary adapter");
   return -1;
}

int primary_session_adapter_compact(const primary_session_request_t *req,
                                    session_compact_result_t *result, char *session_id_out,
                                    size_t session_id_len, char *error_out, size_t error_len)
{
   assert(!"ACP cannot fall through another primary adapter");
   return -1;
}

static cJSON *child_read(void)
{
   char *line = NULL;
   size_t cap = 0;
   ssize_t n = getline(&line, &cap, stdin);
   assert(n > 0);
   cJSON *frame = aimee_ir_json_parse_frame(line, (size_t)n);
   free(line);
   assert(frame);
   return frame;
}
/* Bounded backend peers live in acp_route_policy_fixture.c. Unrelated public
 * module dispatch remains unavailable; ACP cannot borrow a model/KB service. */
void acp_route_policy_fault(int);
void acp_route_policy_reset(const char *, const char *, const char *);
int acp_route_policy_dispatches(void);
int acp_route_policy_outcomes(void);
int acp_route_policy_writes(void);
cJSON *acp_route_resource_call(uint32_t, uint32_t, const char *, const cJSON *, const cJSON *);
cJSON *acp_route_learning_call(uint32_t, uint32_t, const char *, const cJSON *, const cJSON *);
cJSON *aimee_module_command_call_context(uint32_t event, uint32_t stage, const char *verb,
                                         const cJSON *args, const cJSON *caller)
{
   return event == 6914 ? acp_route_resource_call(event, stage, verb, args, caller)
                        : acp_route_learning_call(event, stage, verb, args, caller);
}
uint64_t aimee_module_call_deadline_ns(int timeout)
{
   struct timespec now;
   assert(!clock_gettime(CLOCK_MONOTONIC, &now));
   return timeout > 0
              ? (uint64_t)now.tv_sec * 1000000000u + now.tv_nsec + (uint64_t)timeout * 1000000u
              : 0;
}
int aimee_module_commands_dispatch(const char *method, const cJSON *args, cJSON **result)
{
   (void)method;
   (void)args;
   *result = NULL;
   return -1;
}
int cli_workspace_reverse_channel_start(void)
{
   assert(!"native scoped MCP never opens a reverse workspace channel");
   return -1;
}
int cli_workspace_reverse_channel_sync(void)
{
   assert(!"native scoped MCP never synchronizes ambient workspaces");
   return -1;
}
void cli_workspace_reverse_channel_stop(void)
{
}
int cli_index_ensure_remote(const char *root)
{
   (void)root;
   assert(!"native scoped MCP never auto-ingests a repository");
   return -1;
}
static char audit_key[48];
void audit_log(const char *event, const char *format, ...)
{
   (void)format;
   snprintf(audit_key, sizeof audit_key, "%s", event);
}
void audit_last_event_reset(void)
{
   audit_key[0] = 0;
}
const char *audit_last_event(void)
{
   return audit_key;
}
void audit_action_log(const char *actor, const char *tool, const char *hash, const char *command,
                      const char *mode, const char *reason, const char *verdict, long long task)
{
   (void)actor;
   (void)tool;
   (void)hash;
   (void)command;
   (void)mode;
   (void)reason;
   (void)verdict;
   (void)task;
}

static void child_send(cJSON *frame)
{
   char *line = cJSON_PrintUnformatted(frame);
   assert(line && puts(line) >= 0 && !fflush(stdout));
   free(line);
   cJSON_Delete(frame);
}
typedef struct
{
   pid_t pid;
   FILE *input, *output;
   int mode;
} mcp_child_t;
static mcp_child_t mcp_start(const cJSON *spec, int mode)
{
   const char *command = cJSON_GetStringValue(cJSON_GetObjectItem(spec, "command"));
   const cJSON *args = cJSON_GetObjectItem(spec, "args"), *env = cJSON_GetObjectItem(spec, "env");
   assert(command && !strncmp(command, "/proc/", 6) && cJSON_IsArray(args) &&
          cJSON_GetArraySize(args) == 1 && cJSON_IsArray(env));
   if (mode == 99)
      command = executable; /* Probe the expired cap through the same real entry after original
                               execFD revocation. */
#ifdef AIMEE_ACP_BUILT_MCP_CLI
   /* Optional transport conformance lane uses the actual shipping CLI entry,
    * including expired-capability probes; the ordinary fixture is unchanged. */
   command = AIMEE_ACP_BUILT_MCP_CLI;
   assert(command && command[0] == '/' && !access(command, X_OK));
#endif
   char *argv[] = {(char *)command, "mcp-serve", NULL};
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(args, 0)), argv[1]));
   char *environment[32] = {0};
   int count = 0;
   const cJSON *e;
   cJSON_ArrayForEach(e, env)
   {
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(e, "name")),
                 *value = cJSON_GetStringValue(cJSON_GetObjectItem(e, "value"));
      assert(name && value && count < 31 && strlen(name) < 80 && strlen(value) < 8192);
      if (mode == 27 && !strcmp(name, "AIMEE_SESSION_ID"))
         value = "foreign-session";
      if (mode == 28 && !strcmp(name, "AIMEE_API_BEARER"))
      {
#ifdef AIMEE_ACP_BUILT_MCP_MISSING_TOKEN
         continue;
#else
         value = "wrong-private-capability";
#endif
      }
      size_t length = strlen(name) + strlen(value) + 2;
      environment[count] = malloc(length);
      assert(environment[count]);
      snprintf(environment[count++], length, "%s=%s", name, value);
   }
   int input[2], output[2];
   assert(!pipe(input) && !pipe(output));
   pid_t pid = fork();
   assert(pid >= 0);
   if (!pid)
   {
      if (mode == 99)
         assert(!chdir(workspace));
      if (mode == 26)
      {
         assert(!mkdir("foreign", 0700) || errno == EEXIST);
         assert(!chdir("foreign"));
      }
      assert(dup2(input[0], 0) >= 0 && dup2(output[1], 1) >= 0);
      int diagnostic = open("mcp-stderr.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
      assert(diagnostic >= 0 && dup2(diagnostic, 2) >= 0);
      for (int fd = 3; fd < 1024; fd++)
         close(fd);
      execve(command, argv, environment);
      _exit(127);
   }
   close(input[0]);
   close(output[1]);
   for (int j = 0; j < count; j++)
      free(environment[j]);
   mcp_child_t child = {
       .pid = pid, .input = fdopen(input[1], "w"), .output = fdopen(output[0], "r"), .mode = mode};
   assert(child.input && child.output);
   return child;
}
static cJSON *mcp_exchange(mcp_child_t *child, cJSON *request)
{
   char directory[512], diagnostic[1024];
   const char *root = workspace[0] ? workspace : getcwd(directory, sizeof directory);
   assert(root && root[0] == '/' &&
          snprintf(diagnostic, sizeof diagnostic, "%s/mcp-stage.log", root) <
              (int)sizeof diagnostic);
   const cJSON *id = cJSON_GetObjectItem(request, "id");
   double expected = id ? id->valuedouble : 0;
   char *raw = cJSON_PrintUnformatted(request);
   assert(raw && fprintf(child->input, "%s\n", raw) > 0 && !fflush(child->input));
   free(raw);
   cJSON_Delete(request);
   for (int i = 0; i < 32; i++)
   {
      char *line = NULL;
      size_t capacity = 0;
      ssize_t length = getline(&line, &capacity, child->output);
      if (length <= 0)
      {
         FILE *debug = fopen(diagnostic, "a");
         if (debug)
         {
            fprintf(debug, "EOF expected MCP id=%g\n", expected);
            fclose(debug);
         }
      }
      assert(length > 0 && length < 4 * 1024 * 1024);
      cJSON *frame = aimee_ir_json_parse_frame(line, (size_t)length);
      free(line);
      assert(frame);
      FILE *debug = fopen(diagnostic, "a");
      if (debug)
      {
         char *wire = cJSON_PrintUnformatted(frame);
         fprintf(debug, "{\"fixture_mode\":%d,\"frame\":%s}\n", child->mode, wire);
         free(wire);
         fclose(debug);
      }
      const cJSON *received = cJSON_GetObjectItem(frame, "id");
      if (received)
      {
         assert(cJSON_IsNumber(received) && received->valuedouble == expected);
         return frame;
      }
      const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(frame, "method"));
      assert(method && !strncmp(method, "notifications/", 14));
      cJSON_Delete(frame);
   }
   assert(!"MCP response missing");
   return NULL;
}
static void mcp_close_child(mcp_child_t *child)
{
   assert(!fclose(child->input));
   assert(!fclose(child->output));
   int status = 0;
   assert(waitpid(child->pid, &status, 0) == child->pid && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
}
static int mcp_sequence(const cJSON *servers, const char *root, int mode)
{
   assert(cJSON_IsArray(servers) && cJSON_GetArraySize(servers) == 1);
   const cJSON *spec = cJSON_GetArrayItem(servers, 0);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(spec, "name")), "aimee-owned"));
   if (mode == 35)
   {
      int fd = open("mcp-server-private.json", O_WRONLY | O_CREAT | O_EXCL, 0600);
      assert(fd >= 0);
      char *wire = cJSON_PrintUnformatted(spec);
      assert(wire && write(fd, wire, strlen(wire)) == (ssize_t)strlen(wire));
      free(wire);
      assert(!close(fd));
   }
   mcp_child_t child = mcp_start(spec, mode);
   cJSON *reply = mcp_exchange(
       &child, cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"initialize\",\"params\":{"
                           "\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{"
                           "\"name\":\"owned-ACP-fixture\",\"version\":\"1\"}}}"));
   assert(cJSON_IsObject(cJSON_GetObjectItem(reply, "result")));
   cJSON_Delete(reply);
   if (mode == 27 || mode == 28)
   {
      cJSON *attack = cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"tools/"
                                  "call\",\"params\":{\"name\":\"write_file\",\"arguments\":{"
                                  "\"content\":\"unauthorized\"}}}");
      char target[1024];
      assert(snprintf(target, sizeof target, "%s/owned.txt", root) < (int)sizeof target);
      cJSON_AddStringToObject(
          cJSON_GetObjectItem(cJSON_GetObjectItem(attack, "params"), "arguments"), "path", target);
      reply = mcp_exchange(&child, attack);
      assert(cJSON_IsObject(cJSON_GetObjectItem(reply, "error")) ||
             cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(reply, "result"), "isError")));
      cJSON_Delete(reply);
      mcp_close_child(&child);
      return 1;
   }
   reply = mcp_exchange(&child,
                        cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"tools/list\"}"));
   if (mode == 27 || mode == 28)
   {
      assert(cJSON_IsObject(cJSON_GetObjectItem(reply, "error")));
      cJSON_Delete(reply);
      mcp_close_child(&child);
      return 1;
   }
   const cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(reply, "result"), "tools");
   assert(cJSON_IsArray(tools) && cJSON_GetArraySize(tools) > 0);
   int read = 0, write = 0;
   const cJSON *tool;
   cJSON_ArrayForEach(tool, tools)
   {
      const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "name"));
      assert(name && cJSON_IsObject(cJSON_GetObjectItem(tool, "inputSchema")));
      read |= !strcmp(name, "read_file");
      write |= !strcmp(name, "write_file");
   }
   assert(read && (write == (mode != 25)));
   cJSON_Delete(reply);
   if (mode == 31)
   {
      mcp_close_child(&child);
      return 1;
   }
   if (mode == 36 || mode == 37)
   {
      char command[4096];
      assert(snprintf(command, sizeof command,
                      "aimee --json tool call write_file --arguments '{\"path\":\"owned.txt\","
                      "\"content\":\"first line\\nsecond line\\nthird line\\n\"}' && "
                      "aimee --json tool call read_file --arguments "
                      "'{\"path\":\"owned.txt\",\"raw\":true}'") < (int)sizeof command);
      if (mode == 37)
         assert(snprintf(command, sizeof command,
                         "aimee --json tool call bash --arguments '{\"command\":\"aimee --json "
                         "tool call read_file path=owned.txt raw=true\"}'") < (int)sizeof command);
      cJSON *request = cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":101,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"bash\",\"arguments\":{}}}");
      assert(request &&
             cJSON_AddStringToObject(
                 cJSON_GetObjectItem(cJSON_GetObjectItem(request, "params"), "arguments"),
                 "command", command));
      reply = mcp_exchange(&child, request);
      const cJSON *result = cJSON_GetObjectItem(reply, "result");
      assert(cJSON_IsObject(result) && cJSON_IsFalse(cJSON_GetObjectItem(result, "isError")));
      char *wire = cJSON_PrintUnformatted(result);
      assert(wire && strstr(wire, "second line"));
      free(wire);
      cJSON_Delete(reply);
      mcp_close_child(&child);
      return 0;
   }
   char path[1024];
   assert(snprintf(path, sizeof path, "%s/owned.txt", root) < (int)sizeof path);
   for (int step = 0; step < 2; step++)
   {
      cJSON *request =
          cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":101,\"method\":\"tools/call\",\"params\":{}}");
      cJSON_ReplaceItemInObject(request, "id", cJSON_CreateNumber(101 + step));
      cJSON *params = cJSON_GetObjectItem(request, "params"),
            *arguments = cJSON_AddObjectToObject(params, "arguments");
      assert(cJSON_AddStringToObject(params, "name", step ? "read_file" : "write_file") &&
             cJSON_AddStringToObject(arguments, "path", path));
      if (!step)
         assert(cJSON_AddStringToObject(arguments, "content", file_content));
      else
         assert(cJSON_AddBoolToObject(arguments, "raw", 1));
      reply = mcp_exchange(&child, request);
      const cJSON *result = cJSON_GetObjectItem(reply, "result");
      if (mode == 25 || mode == 26 || mode == 29 || mode == 30 || mode == 32 || mode == 33)
      {
         assert(cJSON_IsObject(cJSON_GetObjectItem(reply, "error")) ||
                (cJSON_IsObject(result) && cJSON_IsTrue(cJSON_GetObjectItem(result, "isError"))));
         cJSON_Delete(reply);
         mcp_close_child(&child);
         return mode == 33 ? 0 : 1;
      }
      assert(cJSON_IsObject(result) && !cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")));
      if (step)
      {
         const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(
             cJSON_GetArrayItem(cJSON_GetObjectItem(result, "content"), 0), "text"));
         assert(text && strstr(text, "second line"));
      }
      cJSON_Delete(reply);
   }
   mcp_close_child(&child);
   return 0;
}

#ifndef AIMEE_ACP_FULL_WORKER_FIXTURE
static void mcp_probe_expired(void)
{
   char path[1024];
   assert(snprintf(path, sizeof path, "%s/mcp-server-private.json", workspace) < (int)sizeof path);
   FILE *f = fopen(path, "rb");
   assert(f);
   char data[32768];
   size_t size = fread(data, 1, sizeof data, f);
   assert(!fclose(f) && size > 0 && size < sizeof data);
   cJSON *spec = aimee_ir_json_parse_frame(data, size);
   assert(spec);
   mcp_child_t client = mcp_start(spec, 99);
   cJSON_Delete(spec);
   cJSON *reply = mcp_exchange(
       &client,
       cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":199,\"method\":\"initialize\",\"params\":{}}"));
   assert(cJSON_IsObject(cJSON_GetObjectItem(reply, "result")));
   cJSON_Delete(reply);
   cJSON *attack = cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":200,\"method\":\"tools/"
                               "call\",\"params\":{\"name\":\"write_file\",\"arguments\":{"
                               "\"content\":\"late unauthorized write\"}}}");
   assert(snprintf(path, sizeof path, "%s/owned.txt", workspace) < (int)sizeof path);
   cJSON_AddStringToObject(cJSON_GetObjectItem(cJSON_GetObjectItem(attack, "params"), "arguments"),
                           "path", path);
   int dispatched = acp_route_policy_dispatches();
   reply = mcp_exchange(&client, attack);
   assert(cJSON_IsObject(cJSON_GetObjectItem(reply, "error")) ||
          cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(reply, "result"), "isError")));
   cJSON_Delete(reply);
   mcp_close_child(&client);
   assert(acp_route_policy_dispatches() == dispatched);
}

#endif
static int child_main(int argc, char **argv)
{
   assert(argc == 6 && !strcmp(argv[1], "--acp-route-child") &&
          !strcmp(argv[5], "quoted argument"));
   struct stat selected, self;
   assert(!fstat(3, &selected) && !stat("/proc/self/exe", &self) &&
          selected.st_dev == self.st_dev && selected.st_ino == self.st_ino);
   for (int fd = 4; fd < 64; fd++)
      assert(fcntl(fd, F_GETFD) < 0);
   assert(!getenv("PARENT_SECRET") && !getenv("OPENAI_API_KEY") && !getenv("ANTHROPIC_API_KEY"));
   extern char **environ;
   int environment_count = 0;
   for (char **entry = environ; *entry; entry++)
      environment_count++;
   assert(environment_count == 3 && getenv("PATH") && !strcmp(getenv("LANG"), "C.UTF-8"));
   assert(getenv("HOME") && strstr(getenv("HOME"), "/aimee-acp-") &&
          strcmp(getenv("HOME"), argv[3]));
   int mode = atoi(argv[2]);
   char actual[4096];
   assert(getcwd(actual, sizeof actual) && !strcmp(actual, argv[3]));
   int marker = open(argv[4], O_WRONLY | O_CREAT | O_EXCL, 0600);
   assert(marker >= 0);
   close(marker);
   cJSON *frame = child_read();
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(frame, "method")), "initialize"));
   cJSON_Delete(frame);
   child_send(cJSON_Parse("{\"id\":1,\"result\":{\"protocolVersion\":1}}"));
   frame = child_read();
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(frame, "method")), "session/new"));
   assert(!strcmp(
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "params"), "cwd")),
       argv[3]));
   cJSON *mcp_servers =
       mode >= 23 ? cJSON_Duplicate(
                        cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "params"), "mcpServers"), 1)
                  : NULL;
   cJSON_Delete(frame);
   child_send(cJSON_Parse("{\"id\":2,\"result\":{\"sessionId\":\"acp-owned-session\"}}"));
   frame = child_read();
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(frame, "method")), "session/prompt"));
   const cJSON *prompt = cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "params"), "prompt");
   int history_turn = mode == 21 && cJSON_GetArraySize(prompt) == 3;
   assert(cJSON_IsArray(prompt) && cJSON_GetArraySize(prompt) == (history_turn ? 3 : 2));
   if (history_turn)
   {
      const char *history =
          cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(prompt, 1), "text"));
      assert(history && strstr(history, user_text) &&
             strstr(history, "ACP public route complete") && strstr(history, "quoted as data"));
   }
   assert(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(prompt, 0), "text")),
                 system_text));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(
                      cJSON_GetArrayItem(prompt, history_turn ? 2 : 1), "text")),
                  history_turn ? next_user_text : user_text));
   char *wire = cJSON_PrintUnformatted(frame);
   assert(wire && strstr(wire, user_text) && strstr(wire, system_text));
   free(wire);
   cJSON_Delete(frame);
   if (mode >= 23)
   {
      child_send(cJSON_Parse("{\"method\":\"session/"
                             "update\",\"params\":{\"sessionId\":\"acp-owned-session\",\"update\":{"
                             "\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":"
                             "\"text\",\"text\":\"Preparing the owned MCP call.\"}}}}"));
      int stopped = mcp_sequence(mcp_servers, argv[3], mode);
      cJSON_Delete(mcp_servers);
      if (stopped)
         return 0;
      if (mode == 34)
      {
         child_send(cJSON_Parse("{\"id\":3,\"result\":{\"stopReason\":\"end_turn\"}}"));
         return 0;
      }
   }
   if (mode == 1 || mode == 2)
   {
      child_send(cJSON_Parse("{\"id\":20,\"method\":\"session/"
                             "request_permission\",\"params\":{\"sessionId\":\"acp-owned-session\","
                             "\"toolCall\":{\"title\":\"Write fixture "
                             "file\"},\"options\":[{\"optionId\":\"always\",\"kind\":\"allow_"
                             "always\"},{\"optionId\":\"once\",\"kind\":\"allow_once\"}]}}"));
      frame = child_read();
      assert(mode == 1);
      assert(!strcmp(
          cJSON_GetStringValue(cJSON_GetObjectItem(
              cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "outcome"), "optionId")),
          "once"));
      cJSON_Delete(frame);
   }
   if (mode == 1 || mode == 3 || mode == 5)
   {
      char target[1024];
      assert(snprintf(target, sizeof target, "%s/owned.txt", argv[3]) < (int)sizeof target);
      frame = cJSON_Parse("{\"id\":\"write-rpc\",\"method\":\"fs/"
                          "write_text_file\",\"params\":{\"sessionId\":\"acp-owned-session\"}}");
      cJSON *params = cJSON_GetObjectItem(frame, "params");
      assert(cJSON_AddStringToObject(params, "path", target) &&
             cJSON_AddStringToObject(params, "content", file_content));
      cJSON *replay = cJSON_Duplicate(frame, 1);
      child_send(frame);
      frame = child_read();
      assert(cJSON_IsObject(cJSON_GetObjectItem(frame, "result")) &&
             !cJSON_GetObjectItem(frame, "error"));
      cJSON_Delete(frame);
      if (mode == 5)
      {
         child_send(replay);
         frame = child_read();
         cJSON_Delete(frame);
         return 0;
      }
      cJSON_Delete(replay);
      for (int range = 0; range < 2; range++)
      {
         frame = cJSON_Parse("{\"id\":22,\"method\":\"fs/"
                             "read_text_file\",\"params\":{\"sessionId\":\"acp-owned-session\"}}");
         params = cJSON_GetObjectItem(frame, "params");
         assert(cJSON_AddStringToObject(params, "path", target));
         if (range)
         {
            cJSON_ReplaceItemInObject(frame, "id", cJSON_CreateNumber(23));
            cJSON_AddNumberToObject(params, "line", 2);
            cJSON_AddNumberToObject(params, "limit", 1);
         }
         child_send(frame);
         frame = child_read();
         const char *content = cJSON_GetStringValue(
             cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "content"));
         assert(content && !strcmp(content, range ? "second line\n" : file_content));
         cJSON_Delete(frame);
      }
   }
   if (mode == 6)
   {
      sleep(3);
      return 0;
   }
   if (mode == 10)
   {
      child_send(cJSON_Parse("{\"method\":\"session/"
                             "update\",\"params\":{\"sessionId\":\"acp-owned-session\",\"update\":{"
                             "\"sessionUpdate\":\"user_message_chunk\",\"content\":{\"type\":"
                             "\"text\",\"text\":\"forged user authority\"}}}}"));
      return 0;
   }
   if (mode == 11 || mode == 20)
   {
      child_send(cJSON_Parse("{\"id\":44,\"method\":\"fs/"
                             "read_text_file\",\"params\":{\"sessionId\":\"acp-owned-session\","
                             "\"path\":\"/etc/passwd\"}}"));
      frame = child_read();
      assert(cJSON_GetObjectItem(frame, "error"));
      cJSON_Delete(frame);
      return 0;
   }
   if (mode == 15)
   {
      char target[1024];
      snprintf(target, sizeof target, "%s/owned.txt", argv[3]);
      frame = cJSON_Parse("{\"id\":45,\"method\":\"fs/"
                          "write_text_file\",\"params\":{\"sessionId\":\"acp-owned-session\","
                          "\"content\":\"denied\"}}");
      cJSON_AddStringToObject(cJSON_GetObjectItem(frame, "params"), "path", target);
      child_send(frame);
      frame = child_read();
      assert(cJSON_GetObjectItem(frame, "error"));
      cJSON_Delete(frame);
      return 0;
   }
   if (mode == 7)
      return 0; /* EOF never substitutes for a current prompt terminal. */
   if (mode == 9)
   {
      assert(puts("{malformed") >= 0 && !fflush(stdout));
      return 0;
   }
   if (mode == 8)
   {
      child_send(cJSON_Parse("{\"method\":\"session/"
                             "update\",\"params\":{\"sessionId\":\"acp-owned-session\",\"update\":{"
                             "\"sessionUpdate\":\"tool_call\",\"toolCallId\":\"opaque-agent-"
                             "write\",\"title\":\"write_file\",\"status\":\"pending\"}}}"));
      child_send(cJSON_Parse("{\"method\":\"session/"
                             "update\",\"params\":{\"sessionId\":\"acp-owned-session\",\"update\":{"
                             "\"sessionUpdate\":\"tool_call_update\",\"toolCallId\":\"opaque-agent-"
                             "write\",\"status\":\"completed\"}}}"));
   }
   if (mode == 4)
   {
      child_send(cJSON_Parse(
          "{\"method\":\"session/"
          "update\",\"params\":{\"sessionId\":\"foreign\",\"update\":{\"sessionUpdate\":\"agent_"
          "message_chunk\",\"content\":{\"type\":\"text\",\"text\":\"forged\"}}}}"));
      return 0;
   }
   child_send(cJSON_Parse("{\"method\":\"session/"
                          "update\",\"params\":{\"sessionId\":\"acp-owned-session\",\"update\":{"
                          "\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":"
                          "\"text\",\"text\":\"ACP public route complete\"}}}}"));
   child_send(cJSON_Parse("{\"id\":3,\"result\":{\"stopReason\":\"end_turn\"}}"));
   return 0;
}

/* Same typed metadata owner as the registered tools module, without a bus process. */
static int route_tool_metadata(const char *name, aimee_tool_metadata_t *out)
{
   *out = aimee_tools_catalog_metadata(name);
   return aimee_tool_metadata_valid(out) ? 0 : -1;
}
static int route_tool_classify(const char *name, int *out)
{
   aimee_tool_metadata_t metadata;
   if (route_tool_metadata(name, &metadata))
      return -1;
   *out = metadata.classification;
   return 0;
}

#ifndef AIMEE_ACP_FULL_WORKER_FIXTURE
static compute_ctx_t *route_context(const char *prompt)
{
   compute_ctx_t *c = calloc(1, sizeof *c);
   assert(c);
   *c = context();
   c->req = cJSON_CreateObject();
   c->conn_fd = dup(STDOUT_FILENO);
   assert(c->conn_fd >= 0);
   c->write_mutex = malloc(sizeof *c->write_mutex);
   assert(c->write_mutex);
   assert(!pthread_mutex_init(c->write_mutex, NULL));
   c->compute_executor_threads = 1;
   c->conn_caps = CAPS_ALL; /* Normal owned browser/operator connection. */
   aimee_block_t input_block = {.type = AIMEE_BLK_TEXT, .text = (char *)prompt};
   aimee_message_t input_message = {.role = "user", .blocks = &input_block, .n_blocks = 1};
   cJSON_AddItemToObject(c->req, "message", aimee_ir_message_json(&input_message));
   cJSON_AddStringToObject(c->req, "cwd", workspace);
   cJSON_AddStringToObject(c->req, "aimee_session_id", "session-one");
   cJSON_AddNumberToObject(c->req, "ir_version", 1);
   if (headless)
      cJSON_AddBoolToObject(c->req, "control_replies", 0);
   return c;
}

static void *cancel_started_child(void *path)
{
   for (int i = 0; i < 2000 && access(path, F_OK); i++)
      usleep(1000);
   assert(!access(path, F_OK));
   atomic_store(&route_cancel, 1);
   return NULL;
}

int main(int argc, char **argv)
{
   if (argc == 2 && !strcmp(argv[1], "mcp-serve"))
   {
      signal(SIGPIPE, SIG_IGN);
      return cli_mcp_serve();
   }
   if (cli_native_bridge_enabled())
   {
      assert(argc >= 5 && !strcmp(argv[1], "--json") && !strcmp(argv[2], "tool") &&
             !strcmp(argv[3], "call") && !getenv("PARENT_SECRET"));
      const char *token = getenv("AIMEE_API_BEARER"), *endpoint = getenv("AIMEE_API_ENDPOINT");
      char digest[65], cwd[1024];
      assert(token && token[0] && endpoint && !strncmp(endpoint, "http://127.0.0.1:", 17) &&
             !aimee_sha256_hex(token, strlen(token), digest) && getcwd(cwd, sizeof cwd));
      cJSON *observed = cJSON_CreateObject();
      assert(observed && cJSON_AddStringToObject(observed, "tool", argv[4]) &&
             cJSON_AddStringToObject(observed, "cwd", cwd) &&
             cJSON_AddStringToObject(observed, "token_sha256", digest));
      char *wire = cJSON_PrintUnformatted(observed);
      int fd = open("nested-cli-observed.jsonl", O_WRONLY | O_CREAT | O_APPEND, 0600);
      assert(fd >= 0 && wire && write(fd, wire, strlen(wire)) == (ssize_t)strlen(wire) &&
             write(fd, "\n", 1) == 1 && !close(fd));
      free(wire);
      cJSON_Delete(observed);
      return cli_native_bridge_main(argc, argv);
   }
   if (argc > 1 && !strcmp(argv[1], "--acp-route-child"))
      return child_main(argc, argv);
   setvbuf(stdout, NULL, _IOLBF, 0);
   assert(readlink("/proc/self/exe", executable, sizeof executable - 1) > 0);
   assert(snprintf(workspace, sizeof workspace, "%s/aimee-acp-route-XXXXXX", platform_tmpdir()) <
          (int)sizeof workspace);
   assert(mkdtemp(workspace));
   char bin[1024], shim[1024], owned_path[8192];
   assert(snprintf(bin, sizeof bin, "%s/bin", workspace) < (int)sizeof bin && !mkdir(bin, 0700));
   assert(snprintf(shim, sizeof shim, "%s/aimee", bin) < (int)sizeof shim &&
          !symlink(executable, shim));
   assert(snprintf(owned_path, sizeof owned_path, "%s:%s", bin,
                   getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin") < (int)sizeof owned_path);
   assert(!setenv("PATH", owned_path, 1));
   assert(!setenv("AIMEE_HOME", workspace, 1) && !setenv("HOME", workspace, 1));
   log_entries = cJSON_CreateArray();
   stored_rows = cJSON_CreateArray();
   int first_case = argc == 3 && !strcmp(argv[1], "--case") ? atoi(argv[2]) : 0;
   int last_case = argc == 3 ? first_case + 1 : 38;
   assert(first_case >= 0 && last_case <= 38);
   for (int fixture_case = first_case; fixture_case < last_case; fixture_case++)
   {
      static const int modes[] = {0,  0,  1,  2,  3,  4,  5,  7,  8,  9,  10, 11, 12,
                                  13, 14, 15, 6,  17, 18, 19, 20, 21, 22, 23, 24, 25,
                                  26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37};
      inline_selection = fixture_case == 1 || fixture_case == 17 || fixture_case == 24;
      child_mode = modes[fixture_case];
      headless = child_mode == 2 || child_mode == 3;
      expected_controls = child_mode == 1;
      actual_controls = 0;
      lifetime_invalidation_seen = 0;
      int positive = child_mode == 0 || child_mode == 1 || child_mode == 3 || child_mode == 8 ||
                     child_mode == 12 || child_mode == 19 || child_mode == 21 || child_mode == 23 ||
                     child_mode == 24 || child_mode == 33 || child_mode == 35 || child_mode == 36 ||
                     child_mode == 37;
      assert(!config_set_provider(inline_selection ? "acp:inline" : "configured-acp"));
      char marker[1024];
      snprintf(marker, sizeof marker, "%s/started-%d", workspace, inline_selection);
      assert(snprintf(command, sizeof command,
                      "--acp-route-child %d %s started-%d \"quoted argument\"", child_mode,
                      workspace, inline_selection) < (int)sizeof command);
      acp_route_policy_reset(workspace, "alice", "session-one");
      acp_route_policy_fault(
          child_mode == 32
              ? 1
              : (child_mode == 33 ? 2 : (child_mode == 36 ? 3 : (child_mode == 37 ? 4 : 0))));
      if (child_mode == 37)
      {
         char target[1024];
         assert(snprintf(target, sizeof target, "%s/owned.txt", workspace) < (int)sizeof target);
         FILE *seed = fopen(target, "wb");
         assert(seed &&
                fwrite(file_content, 1, strlen(file_content), seed) == strlen(file_content));
         assert(!fclose(seed));
      }
      agent_tools_register_action_owner(policy_action_begin, policy_action_finish);
      agent_tools_register_metadata(route_tool_metadata);
      agent_tools_register_classifier(route_tool_classify);
      const char *denied[] = {"write_file", "read_file"};
      int deny_count = (child_mode == 15 || child_mode == 25)
                           ? 1
                           : (child_mode == 19 || child_mode == 20 ? 2 : 0);
      agent_tools_denied_set(deny_count ? denied : NULL, deny_count);
      atomic_store(&route_cancel, 0);
      agent_set_request_cancel(&route_cancel);
      compute_ctx_t *c = route_context(user_text);
      request_context_t owner_context = {.vault_transport = ATTEST_WEBCHAT_TRUSTED,
                                         .transport = REQ_TRANSPORT_TCP,
                                         .capabilities = c->conn_caps};
      snprintf(owner_context.vault_principal, sizeof owner_context.vault_principal, "alice");
      snprintf(owner_context.principal, sizeof owner_context.principal, "alice");
      snprintf(owner_context.request_id, sizeof owner_context.request_id, "acp-public-request");
      snprintf(owner_context.path, sizeof owner_context.path, "/v1/chat/send_stream");
      agent_set_request_session("session-one");
      agent_set_request_vault_principal(child_mode == 14 ? "foreign-actor" : "alice");
      if (child_mode == 12 || child_mode == 17)
         owner_context.capabilities = CAP_CHAT;
      if (child_mode == 18)
         owner_context.vault_transport = ATTEST_NONE;
      request_context_set(&owner_context);
      setenv("PARENT_SECRET", "must-not-reach-agent", 1);
      pthread_t canceller;
      if (child_mode == 6)
         assert(!pthread_create(&canceller, NULL, cancel_started_child, marker));
      chat_stream_worker(c);
      if (child_mode == 6)
         assert(!pthread_join(canceller, NULL));
      agent_set_request_cancel(NULL);
      if (child_mode == 21)
      {
         assert(!access(marker, F_OK) && !unlink(marker));
         snprintf(owner_context.request_id, sizeof owner_context.request_id, "acp-public-followup");
         request_context_set(&owner_context);
         chat_stream_worker(route_context(next_user_text));
      }

      int launched = child_mode != 13 && child_mode != 14 && child_mode != 17 && child_mode != 18 &&
                     child_mode != 22;
      if (launched && access(marker, F_OK))
      {
         char *diagnostic = cJSON_Print(log_entries);
         fprintf(stderr, "stored events=%s\nworker error=%s\n", diagnostic, worker_error);
         free(diagnostic);
      }
      assert((access(marker, F_OK) == 0) == launched);
      int completed = 0;
      for (int i = 0; i < cJSON_GetArraySize(log_entries); i++)
      {
         const cJSON *event = cJSON_GetArrayItem(log_entries, i);
         const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(event, "kind"));
         const char *status = cJSON_GetStringValue(
             cJSON_GetObjectItem(cJSON_GetObjectItem(event, "payload"), "status"));
         if (kind && !strcmp(kind, "execution") && status && !strcmp(status, "completed"))
            completed++;
      }
      if (completed != (positive ? (child_mode == 21 ? 2 : 1) : 0))
      {
         char *detail = cJSON_Print(log_entries);
         fprintf(stderr, "case=%d mode=%d completed=%d expected=%d dispatch=%d outcomes=%d\n%s\n",
                 fixture_case, child_mode, completed, positive, acp_route_policy_dispatches(),
                 acp_route_policy_outcomes(), detail);
         free(detail);
      }
      assert(completed == (positive ? (child_mode == 21 ? 2 : 1) : 0));
      assert(actual_controls == expected_controls);
      if (child_mode == 35)
         mcp_probe_expired();
      if (child_mode == 36 || child_mode == 37)
      {
         char path[1024], line[2048], first_digest[65];
         assert(snprintf(path, sizeof path, "%s/nested-cli-observed.jsonl", workspace) <
                (int)sizeof path);
         FILE *observed = fopen(path, "rb");
         assert(observed);
         for (int invocation = 0; invocation < 2; invocation++)
         {
            assert(fgets(line, sizeof line, observed));
            cJSON *record = cJSON_Parse(line);
            const char *tool = cJSON_GetStringValue(cJSON_GetObjectItem(record, "tool")),
                       *cwd = cJSON_GetStringValue(cJSON_GetObjectItem(record, "cwd")),
                       *digest = cJSON_GetStringValue(cJSON_GetObjectItem(record, "token_sha256"));
            assert(record && tool && cwd && digest && strlen(digest) == 64 &&
                   !strcmp(cwd, workspace) &&
                   !strcmp(tool, invocation         ? "read_file"
                                 : child_mode == 36 ? "write_file"
                                                    : "bash"));
            if (!invocation)
               snprintf(first_digest, sizeof first_digest, "%s", digest);
            else
               assert((!strcmp(first_digest, digest)) == (child_mode == 36));
            cJSON_Delete(record);
         }
         assert(!fgets(line, sizeof line, observed) && !fclose(observed));
         assert(!unlink(path));
      }
      if (child_mode == 29 || child_mode == 30)
         assert(lifetime_invalidation_seen == 1);
      assert(acp_route_policy_dispatches() ==
             acp_route_policy_outcomes() + (child_mode == 32 ? 1 : 0));
      int file_case = child_mode == 1 || child_mode == 3 || child_mode == 5 || child_mode == 23 ||
                      child_mode == 24 || child_mode == 32 || child_mode == 34 ||
                      child_mode == 35 || child_mode == 36;
      assert(acp_route_policy_writes() == file_case);
      char target_check[1024];
      snprintf(target_check, sizeof target_check, "%s/owned.txt", workspace);
      if (file_case || child_mode == 37)
      {
         FILE *saved = fopen(target_check, "rb");
         assert(saved);
         char bytes[128] = {0};
         size_t n = fread(bytes, 1, sizeof bytes, saved);
         assert(!fclose(saved));
         assert(n == strlen(file_content) && !memcmp(bytes, file_content, n));
      }
      else
         assert(access(target_check, F_OK) < 0);
      if (child_mode == 8)
      {
         int reports = 0;
         const cJSON *e;
         cJSON_ArrayForEach(e, log_entries)
         {
            const cJSON *payload = cJSON_GetObjectItem(e, "payload");
            const cJSON *count = cJSON_GetObjectItem(payload, "unverified_external_tool_reports");
            if (count)
               reports += count->valueint;
            const cJSON *blocks = cJSON_GetObjectItem(payload, "blocks");
            const cJSON *b;
            cJSON_ArrayForEach(b, blocks)
            {
               const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type"));
               assert(!t || strcmp(t, "tool_use"));
            }
         }
         assert(reports == 2);
      }

      if (child_mode == 23 || child_mode == 24 || child_mode == 33)
         assert(last_business_text && !strcmp(last_business_text, "ACP public route complete"));
      if (!positive)
         assert(last_business_rc < 0 && !last_business_text);
      if (child_mode == 23 || child_mode == 24 || child_mode == 35)
      {
         int pre = -1, post = -1, uses = 0, results = 0, last_result = -1;
         for (int row = 0; row < cJSON_GetArraySize(log_entries); row++)
         {
            const cJSON *event = cJSON_GetArrayItem(log_entries, row);
            const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(event, "kind"));
            if (!kind || strcmp(kind, "message"))
               continue;
            const cJSON *payload = cJSON_GetObjectItem(event, "payload"), *block;
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "role"));
            cJSON_ArrayForEach(block, cJSON_GetObjectItem(payload, "blocks"))
            {
               const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text")),
                          *id = cJSON_GetStringValue(cJSON_GetObjectItem(block, "tool_id")),
                          *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
               if (role && !strcmp(role, "assistant") && text &&
                   !strcmp(text, "Preparing the owned MCP call."))
               {
                  assert(pre < 0);
                  pre = row;
               }
               if (role && !strcmp(role, "assistant") && text &&
                   !strcmp(text, "ACP public route complete"))
               {
                  assert(post < 0);
                  post = row;
               }
               if (id && !strncmp(id, "acp-mcp:", 8) && type && !strcmp(type, "tool_use"))
               {
                  assert(pre >= 0 && post < 0);
                  uses++;
               }
               if (id && !strncmp(id, "acp-mcp:", 8) && type && !strcmp(type, "tool_result"))
               {
                  assert(pre >= 0 && post < 0 && uses == results + 1);
                  results++;
                  last_result = row;
               }
            }
         }
         assert(pre >= 0 && uses == 2 && results == 2 && post > last_result);
      }
      if (positive)
      {
         int exact_text = 0, users = 0;
         const cJSON *e;
         cJSON_ArrayForEach(e, log_entries)
         {
            const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(e, "kind"));
            const cJSON *payload = cJSON_GetObjectItem(e, "payload");
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "role"));
            if (kind && !strcmp(kind, "message") && role && !strcmp(role, "user"))
               users++;
            const cJSON *b;
            cJSON_ArrayForEach(b, cJSON_GetObjectItem(payload, "blocks"))
            {
               const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(b, "text"));
               if (role && !strcmp(role, "assistant") && text &&
                   !strcmp(text, "ACP public route complete"))
                  exact_text++;
            }
         }
         if (users != (child_mode == 21 ? 2 : 1) || exact_text < (child_mode == 21 ? 2 : 1))
         {
            char *detail = cJSON_Print(log_entries);
            fprintf(stderr, "case=%d users=%d text=%d\n%s\n", fixture_case, users, exact_text,
                    detail);
            free(detail);
         }
         assert(users == (child_mode == 21 ? 2 : 1) && exact_text >= (child_mode == 21 ? 2 : 1));
         if (child_mode == 21)
         {
            int lowered = 0;
            const cJSON *e;
            cJSON_ArrayForEach(e, log_entries)
            {
               const cJSON *count = cJSON_GetObjectItem(cJSON_GetObjectItem(e, "payload"),
                                                        "history_messages_as_text");
               if (count)
                  lowered += count->valueint;
            }
            assert(lowered == 2);
         }
      }
      printf("PASS public ACP case=%d mode=%d inline=%d headless=%d launched=%d completed=%d "
             "controls=%d dispatches=%d outcomes=%d writes=%d\n",
             fixture_case, child_mode, inline_selection, headless, launched, completed,
             actual_controls, acp_route_policy_dispatches(), acp_route_policy_outcomes(),
             acp_route_policy_writes());
      request_context_clear();
      unlink(marker);
      char target[1024];
      snprintf(target, sizeof target, "%s/owned.txt", workspace);
      unlink(target);
      cJSON_Delete(log_entries);
      log_entries = cJSON_CreateArray();
      cJSON_Delete(stored_rows);
      stored_rows = cJSON_CreateArray();
      writes = fail_store = disconnected = cancelled = delivered_sequence = 0;
      test_learning_observations_reset();
   }
   cJSON_Delete(log_entries);
   cJSON_Delete(stored_rows);
   free(last_business_text);
   const char *artifacts = getenv("AIMEE_TEST_ARTIFACT_DIR");
   if (artifacts && artifacts[0])
   {
      char source[1024], destination[4096];
      assert(snprintf(source, sizeof source, "%s/mcp-stage.log", workspace) < (int)sizeof source);
      assert(snprintf(destination, sizeof destination, "%s/mcp-observed-frames.jsonl", artifacts) <
             (int)sizeof destination);
      FILE *input = fopen(source, "rb");
      int output = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
      assert(input && output >= 0);
      char bytes[8192];
      size_t count;
      while ((count = fread(bytes, 1, sizeof bytes, input)))
         assert(write(output, bytes, count) == (ssize_t)count);
      assert(!ferror(input) && !fclose(input) && !close(output));
   }
   obs_bus_stop();
   audit_worm_close();
   acp_route_policy_reset(workspace, "alice", "session-one");
   platform_test_rmrf(workspace);
   assert(access(workspace, F_OK) < 0);
   puts("PASS: 38 actual public ACP route fixtures; configured/inline, control/policy/filesystem "
        "and failure boundaries");
   return 0;
}

#endif /* !AIMEE_ACP_FULL_WORKER_FIXTURE */
