/* Full delegate-worker composition. Configuration/storage services are explicit
 * fixture peers; workspace/container acquisition, source policy, canonical
 * capture, routing and the descriptor-launched ACP worker are production code.
 * This executable requires the separately bound real container/egress fixture.
 * It never invokes a provider SDK or a model. */
#include "modules/workspace/workspace_turn.h"
#include "agent_exec.h"
#define AIMEE_ACP_FULL_WORKER_FIXTURE           1
#define compute_pool_set_job                    full_unused_compute_pool_set_job
#define test_learning_assert_source             full_unused_test_learning_assert_source
#define session_persona_get                     full_unused_session_persona_get
#define session_primary_get                     full_unused_session_primary_get
#define err_json                                full_unused_err_json
#define persona_load                            full_unused_persona_load
#define persona_free                            full_unused_persona_free
#define workspace_turn_resource_identity        unused_route_resource_identity
#define workspace_turn_bind_active              unused_route_bind_active
#define workspace_turn_unbind_active            unused_route_unbind_active
#define workspace_turn_active_cwd               unused_route_active_cwd
#define workspace_turn_drift_notice             unused_route_drift_notice
#define workspace_turn_reject_foreign_cwd       unused_route_reject_foreign_cwd
#define workspace_turn_mirror_bind              unused_route_mirror_bind
#define workspace_turn_mirror_release           unused_route_mirror_release
#define workspace_turn_container_bound          unused_route_container_bound
#define agent_run_with_tools                    unused_route_agent_run
#define ingress_preinject_set_health_turn_id    full_unused_ingress_preinject_set_health_turn_id
#define ingress_preinject_acquire_send_guard    full_unused_ingress_preinject_acquire_send_guard
#define ingress_preinject_release_send_guard    full_unused_ingress_preinject_release_send_guard
#define ingress_preinject_set_session_id        full_unused_ingress_preinject_set_session_id
#define ingress_preinject_set_task_requirements full_unused_ingress_preinject_set_task_requirements
#define presence_turn_host_own                  full_unused_presence_turn_host_own
#define presence_emit_turn_delta                full_unused_presence_emit_turn_delta
#include "test_server_acp_chat_route.c"
#undef test_learning_assert_source
#undef compute_pool_set_job
void compute_pool_set_job(pool_job_kind_t kind, const char *format, ...)
{
   (void)format;
   assert(kind == POOL_JOB_DELEGATE);
}
#undef ingress_preinject_set_health_turn_id
#undef ingress_preinject_acquire_send_guard
#undef ingress_preinject_release_send_guard
#undef ingress_preinject_set_session_id
#undef ingress_preinject_set_task_requirements
#undef presence_turn_host_own
#undef presence_emit_turn_delta

#undef workspace_turn_resource_identity
#undef workspace_turn_bind_active
#undef workspace_turn_unbind_active
#undef workspace_turn_active_cwd
#undef workspace_turn_drift_notice
#undef workspace_turn_reject_foreign_cwd
#undef workspace_turn_mirror_bind
#undef workspace_turn_mirror_release
#undef workspace_turn_container_bound
#undef agent_run_with_tools
#undef db1_server_session_create
#undef db1_server_session_get
#undef db1_session_ir_append
#undef db1_session_ir_page
#undef db1_session_ir_running_history_load
#include <sys/socket.h>
#include <dirent.h>
#undef session_persona_get
#undef session_primary_get
#undef err_json
#undef persona_load
#undef persona_free
#include "platform_ipc.h"
#include "platform_random.h"
#include "vault_principal.h"
#include "server_http_identity.h"
#include "delegate_permissions_stub.h"
#include <aimee/delegates/delegate_role.h>
#include <aimee/delegates/delegate_launch_args.h>
#include <aimee/delegates/module_api.h>
#include "support/delegate_role_seam_stub.h"
#include "modules/workspace/workspace_turn.h"
#include <aimee/delegates/delegate_backend_docker.h>

static int full_worker_read_only;
static const char *full_worker_role(void)
{
   return full_worker_read_only ? "diagnose" : "code";
}

/* Exact code/diagnose role metadata recording, pinned by the Go rolepolicy_test.go
 * TestRoleIsWrite contract. Permission grants come from the existing generated
 * ResolveRolePermissions recording; neither peer attests source or performs I/O. */
static int full_worker_role_policy(int operation, const char *role, int a, int b, int c, int *out)
{
   (void)a;
   (void)b;
   (void)c;
   if (!role || strcmp(role, full_worker_role()) || !out ||
       (operation != DELEGATE_ROLE_OP_BUILTIN && operation != DELEGATE_ROLE_OP_IS_WRITE))
      return -1;
   *out = operation == DELEGATE_ROLE_OP_BUILTIN || !full_worker_read_only;
   return 0;
}

/* Recorded Go review metadata: code is unguarded; diagnose is guarded but
 * does not snippet-check historical evidence. The exact report below makes no
 * clean-tree claim. This recording does not grade the execution. */
static int full_worker_review_policy(const char *role, const char *response, unsigned flags,
                                     unsigned *verdict, char *message, size_t capacity)
{
   if (!role || strcmp(role, full_worker_role()) || !response || !verdict)
      return -1;
   if (flags == AIMEE_DELEGATES_REVIEW_POLICY_ONLY && !response[0])
      *verdict = full_worker_read_only ? AIMEE_DELEGATES_REVIEW_GUARDED : 0;
   else if (full_worker_read_only && !(flags & ~AIMEE_DELEGATES_REVIEW_WORKTREE_DIRTY) &&
            !strcmp(response, "Full delegate worker ACP complete"))
      *verdict = AIMEE_DELEGATES_REVIEW_GUARDED;
   else
      return -1;
   if (message && capacity)
      message[0] = 0;
   return 0;
}

static void full_worker_exact_file(const char *path, const char *expected)
{
   FILE *file = fopen(path, "rb");
   char content[256];
   assert(file);
   size_t length = fread(content, 1, sizeof content, file);
   assert(!ferror(file) && !fclose(file) && length == strlen(expected) &&
          !memcmp(content, expected, length));
}

/* Exact in-memory DB1 service peer. It stores writer-owned canonical envelopes;
 * source/capture/control policy and all workspace/container I/O stay real. */
static db1_server_session_t full_sessions[32];
static int full_session_count;
int db1_server_session_get(const char *sid, db1_server_session_t *out);
int db1_server_session_create(const char *sid, const char *client, const char *actor)
{
   assert(sid && sid[0] && actor && actor[0]);
   for (int i = 0; i < full_session_count; i++)
      if (!strcmp(full_sessions[i].id, sid))
         return strcmp(full_sessions[i].principal, actor) ? -1 : 0;
   assert(full_session_count < 32);
   db1_server_session_t *row = &full_sessions[full_session_count++];
   snprintf(row->id, sizeof row->id, "%s", sid);
   snprintf(row->client_type, sizeof row->client_type, "%s", client);
   snprintf(row->principal, sizeof row->principal, "%s", actor);
   snprintf(row->created_at, sizeof row->created_at, "owned-fixture-incarnation");
   const char *bound = run_cmd_get_cwd();
   const char *source_session = agent_get_request_session();
   if (!source_session || !source_session[0])
      source_session = sid;
   db1_server_session_t declared;
   assert(!db1_server_session_get(source_session, &declared) && !strcmp(declared.principal, actor));
   /* Logical tool/action ownership remains the admitted parent; canonical
    * content is captured in the separately owned child turn/session. */
   acp_route_policy_reset(bound && bound[0] ? bound : workspace, actor, source_session);
   acp_route_policy_fault(5);
   return 0;
}
int db1_server_session_get(const char *sid, db1_server_session_t *out)
{
   for (int i = 0; i < full_session_count; i++)
      if (!strcmp(full_sessions[i].id, sid))
      {
         *out = full_sessions[i];
         return 0;
      }
   return -1;
}
/* Explicit delegation-state storage peer. Only the worker's actual record
 * creates a known active ID; an unavailable/unknown ID is a refusal. This does
 * not attest a model source, grant permission or perform container operations.
 */
static struct
{
   char id[64], parent[64], session[128], role[64];
   int depth, recorded, completed, stop_reads;
} full_spawn;
int db1_delegation_spawn_record(const char *id, const char *parent, const char *sid, int depth,
                                const char *role)
{
   db1_server_session_t session;
   const char *parent_id = parent ? parent : "";
   const request_context_t *context = request_context_get();
   if (!id || !id[0] || strlen(id) >= sizeof full_spawn.id ||
       strlen(parent_id) >= sizeof full_spawn.parent || !sid ||
       strlen(sid) >= sizeof full_spawn.session || !role || strcmp(role, full_worker_role()) ||
       depth < 0 || !context || strcmp(request_context_session_actor(context), owner) ||
       db1_server_session_get(sid, &session) || strcmp(session.principal, owner))
      return -1;
   if (full_spawn.recorded)
      return strcmp(full_spawn.id, id) || strcmp(full_spawn.parent, parent_id) ||
                     strcmp(full_spawn.session, sid) || strcmp(full_spawn.role, role) ||
                     full_spawn.depth != depth
                 ? -1
                 : 0;
   snprintf(full_spawn.id, sizeof full_spawn.id, "%s", id);
   snprintf(full_spawn.parent, sizeof full_spawn.parent, "%s", parent_id);
   snprintf(full_spawn.session, sizeof full_spawn.session, "%s", sid);
   snprintf(full_spawn.role, sizeof full_spawn.role, "%s", role);
   full_spawn.depth = depth;
   full_spawn.recorded = 1;
   return 0;
}
int db1_delegation_spawn_is_stopped(const char *id)
{
   if (!full_spawn.recorded || !id || strcmp(id, full_spawn.id))
      return -1;
   full_spawn.stop_reads++;
   return full_spawn.completed;
}
int db1_delegation_spawn_complete(const char *id)
{
   if (!full_spawn.recorded || !id || strcmp(id, full_spawn.id))
      return -1;
   full_spawn.completed = 1;
   return 0;
}

int db1_session_ir_append(const char *sid, const char *id, const char *turn, const char *actor,
                          const char *payload, char sequence[32])
{
   db1_server_session_t owner;
   if (db1_server_session_get(sid, &owner) || strcmp(owner.principal, actor))
      return -1;
   int last = 0;
   const cJSON *existing;
   cJSON_ArrayForEach(existing, stored_rows)
   {
      if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(existing, "session_id")), sid))
         continue;
      last++;
      if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(existing, "event_id")), id))
         return -1; /* This fixture never permits ambiguous duplicate commits. */
   }
   snprintf(sequence, 32, "%d", last + 1);
   cJSON *row = cJSON_CreateObject();
   assert(row && cJSON_AddNumberToObject(row, "version", 1) &&
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
   db1_server_session_t owner;
   if (db1_server_session_get(sid, &owner) || strcmp(owner.principal, actor))
      return NULL;
   cJSON *page = cJSON_CreateArray();
   assert(page);
   const cJSON *row;
   cJSON_ArrayForEach(
       row, stored_rows) if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(row, "session_id")),
                                     sid) &&
                             atoi(cJSON_GetStringValue(cJSON_GetObjectItem(row, "sequence"))) >
                                 atoi(after) &&
                             cJSON_GetArraySize(page) < limit)
       assert(cJSON_AddItemToArray(page, cJSON_Duplicate(row, 1)));
   char *wire = cJSON_PrintUnformatted(page);
   cJSON_Delete(page);
   return wire;
}
int db1_session_ir_running_history_load(const char *sid, const char *actor, const char *turn,
                                        char **out)
{
   (void)turn;
   char *wire = db1_session_ir_page(sid, "0", 4096, actor);
   if (!wire)
      return -1;
   cJSON *page = cJSON_Parse(wire), *history = cJSON_CreateArray();
   assert(page && history);
   free(wire);
   const cJSON *row;
   cJSON_ArrayForEach(row, page)
   {
      const cJSON *event = cJSON_GetObjectItem(row, "event");
      const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(event, "kind"));
      if (kind && !strcmp(kind, "message"))
         assert(cJSON_AddItemToArray(history,
                                     cJSON_Duplicate(cJSON_GetObjectItem(event, "payload"), 1)));
   }
   *out = cJSON_PrintUnformatted(history);
   cJSON_Delete(page);
   cJSON_Delete(history);
   return *out ? 0 : -1;
}

/* Fresh child history is a storage observation, not an affirmative source
 * handoff. Refuse any preexisting model or interruption content in this case. */
static cJSON *full_fresh_completed_history(const char *sid, const char *actor, const char *turn)
{
   db1_server_session_t session;
   if (db1_server_session_get(sid, &session) || strcmp(session.principal, actor))
      return NULL;
   const cJSON *row;
   cJSON_ArrayForEach(row, stored_rows)
   {
      if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(row, "session_id")), sid))
         continue;
      const cJSON *event = cJSON_GetObjectItem(row, "event");
      const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(event, "kind"));
      if (!kind || strstr(kind, "interruption"))
         return NULL;
      if (!strcmp(kind, "message"))
      {
         const char *row_turn = cJSON_GetStringValue(cJSON_GetObjectItem(row, "turn_id"));
         const cJSON *message = cJSON_GetObjectItem(event, "payload");
         const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(message, "role"));
         /* The admitted current user input is still pending, not completed
          * history. It must not be replayed as an earlier turn. */
         if (!row_turn || strcmp(row_turn, turn) || !role || strcmp(role, "user"))
            return NULL;
      }
   }
   return cJSON_CreateArray();
}
int db1_session_ir_history_load(const char *sid, const char *scope, const char *actor,
                                const char *turn, char **out)
{
   assert(scope && !strcmp(scope, "primary") && turn && turn[0] && out);
   cJSON *history = full_fresh_completed_history(sid, actor, turn);
   *out = history ? cJSON_PrintUnformatted(history) : NULL;
   cJSON_Delete(history);
   return *out ? 0 : -1;
}
int db1_session_native_history_load(const char *actor, const char *sid, const char *mode,
                                    const char *turn, char **out)
{
   assert(mode && !strcmp(mode, "completed") && turn && turn[0] && out);
   cJSON *history = full_fresh_completed_history(sid, actor, turn);
   if (!history)
      return -1;
   cJSON *value = cJSON_CreateObject();
   assert(value && cJSON_AddItemToObject(value, "messages", history) &&
          cJSON_AddArrayToObject(value, "interruptions"));
   *out = cJSON_PrintUnformatted(value);
   cJSON_Delete(value);
   return *out ? 0 : -1;
}

void test_learning_assert_source(const cJSON *record)
{
   const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(record, "session_id"));
   const char *actor = cJSON_GetStringValue(cJSON_GetObjectItem(record, "actor"));
   const char *sequence = cJSON_GetStringValue(cJSON_GetObjectItem(record, "sequence"));
   assert(sid && actor && sequence && !strcmp(actor, owner));
   const cJSON *row;
   cJSON_ArrayForEach(row, stored_rows)
   {
      if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(row, "session_id")), sid) ||
          strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(row, "sequence")), sequence))
         continue;
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(row, "actor")), actor));
      assert(cJSON_Compare(cJSON_GetObjectItem(row, "event"), cJSON_GetObjectItem(record, "event"),
                           1));
      return;
   }
   assert(!"learning observation must reference an actual committed owned row");
}

int full_route_task_outcome_record(const char *, const char *, const char *, const char *,
                                   const char *);
int db1_interaction_event_record(const char *sid, const char *type, const char *actor,
                                 const char *payload, const char *outcome)
{
   if (strcmp(type, "delegate_exit"))
      return full_route_task_outcome_record(sid, type, actor, payload, outcome);
   assert(sid && sid[0] && !strcmp(actor, "system"));
   assert(!strcmp(outcome, "ok") || !strcmp(outcome, "error"));
   cJSON *event = cJSON_Parse(payload);
   assert(cJSON_IsObject(event));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(event, "role")), full_worker_role()));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(event, "learning_owner")),
                  "canonical_session_log"));
   fprintf(stderr, "actual delegate exit observation: %s %s\n", outcome, payload);
   cJSON_Delete(event);
   return 42;
}

int main(int argc, char **argv)
{
   if (argc > 1 && !strcmp(argv[1], "--acp-route-child"))
      return child_main(argc, argv);
   if (argc == 2 && !strcmp(argv[1], "mcp-serve"))
      return cli_mcp_serve();
   if (cli_native_bridge_enabled())
      return cli_native_bridge_main(argc, argv);
   if (argc == 1)
   {
      puts("SKIP: full ACP delegate worker requires the real Docker integration fixture; "
           "run make integration-test-acp-full-delegate-worker with its owned repository, "
           "private HOME and prepared sandbox image");
      return 0;
   }
   assert(argc == 3 &&
          (!strcmp(argv[1], "--full-worker") || !strcmp(argv[1], "--full-worker-read-only")));
   full_worker_read_only = !strcmp(argv[1], "--full-worker-read-only");
   const char *fixture_home = getenv("AIMEE_FULL_WORKER_HOME");
   assert(fixture_home && fixture_home[0] == '/' && getenv("AIMEE_HOME") && getenv("HOME") &&
          !strcmp(fixture_home, getenv("AIMEE_HOME")) && !strcmp(fixture_home, getenv("HOME")));
   struct stat home_stat;
   assert(!stat(fixture_home, &home_stat) && S_ISDIR(home_stat.st_mode) &&
          home_stat.st_uid == getuid());
   /* The runner supplies a new committed, UID-owned registered repository. */
   struct stat st;
   assert(!stat(argv[2], &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid());
   char *canonical = realpath(argv[2], NULL);
   assert(canonical && strlen(canonical) < sizeof workspace);
   strcpy(workspace, canonical);
   free(canonical);
   ssize_t count = readlink("/proc/self/exe", executable, sizeof executable - 1);
   assert(count > 0 && count < (ssize_t)sizeof executable);
   executable[count] = 0;
   delegate_role_seam_install();
   const char *canonical_role = delegate_role_canonicalize(full_worker_role());
   assert(canonical_role && !strcmp(canonical_role, full_worker_role()));
   assert(!strcmp(delegate_role_canonicalize(canonical_role), full_worker_role()));
   delegate_register_role_policy_provider(full_worker_role_policy);
   delegate_register_review_evidence_provider(full_worker_review_policy);
   delegate_permissions_stub_install();
   expected_controls = 0;
   agent_tools_register_metadata(route_tool_metadata);
   agent_tools_register_classifier(route_tool_classify);
   agent_tools_register_action_owner(policy_action_begin, policy_action_finish);
   log_entries = cJSON_CreateArray();
   stored_rows = cJSON_CreateArray();
   assert(!config_set_memory_recall_enabled(0));
   assert(!platform_setenv("AIMEE_CONTEXT_NO_KB", "1"));
   assert(!config_workspace_add(workspace, "shared", "", ""));
   assert(!delegate_backend_register(delegate_backend_docker_get()));
   int pair[2];
   assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair));
   platform_peer_cred_t peer;
   assert(!platform_ipc_peer_cred(pair[0], &peer) && peer.uid == getuid());
   /* The connection owns large bounded buffers; keep this single-run fixture
    * off the process stack, exactly as a real accepted connection is heap-owned. */
   static server_conn_t connection;
   connection.fd = pair[0];
   connection.capabilities = CAPS_ALL;
   connection.peer_uid = peer.uid;
   connection.attested_transport =
       vault_principal_resolve(0, 0, peer.uid, NULL, 0, NULL, connection.vault_principal,
                               sizeof connection.vault_principal);
   assert(connection.attested_transport == ATTEST_UDS_PEERCRED);
   assert(strlen(connection.vault_principal) < sizeof owner);
   strcpy(owner, connection.vault_principal);
   const char *head = "POST /v1/delegate HTTP/1.1\r\n\r\n";
   char request_id[33];
   assert(!platform_random_hex(request_id, 32));
   server_http_identity_capture(pair[0], 0, head);
   server_http_populate_request_context(pair[0], 0, head, request_id, "POST", "/v1/delegate",
                                        connection.capabilities);
   const request_context_t *ingress = request_context_get();
   assert(ingress && ingress->peer_uid == (long)peer.uid &&
          ingress->vault_transport == ATTEST_UDS_PEERCRED &&
          !strcmp(ingress->principal, connection.vault_principal));
   static server_ctx_t server; /* Real server ownership is heap/static, not a 67 MiB stack frame. */
   cJSON *request = cJSON_CreateObject();
   assert(!db1_server_session_create("full-worker-parent", "cli", connection.vault_principal));
   cJSON_AddStringToObject(request, "session_id", "full-worker-parent");
   cJSON_AddStringToObject(request, "role", full_worker_role());
   cJSON_AddStringToObject(request, "persona", "engineer");
   cJSON_AddStringToObject(request, "cwd", workspace);
   cJSON_AddStringToObject(request, "prompt", "Complete the owned full worker protocol fixture.");
   const char *agent = getenv("AIMEE_FULL_WORKER_AGENT");
   assert(agent && agent[0] == '/' && strlen(agent) < 256);
   assert(access(agent, X_OK) != 0); /* Image-only agent: no host availability substitute. */
   cJSON_AddStringToObject(request, "acp_command", agent);
   if (full_worker_read_only)
      cJSON_AddStringToObject(request, "acp_args", "--read-only");
   cJSON_AddNumberToObject(request, "max_turns", 0);
   cJSON_AddNumberToObject(request, "timeout_ms", 60000);
   cJSON_AddBoolToObject(request, "handoff_json", 0);
   cJSON_AddBoolToObject(request, "tools", 1);
   cJSON_AddBoolToObject(request, "provided_target", 1);
   char parent_seed[4096], private_sentinel[4096], parent_attempt[4096];
   assert(snprintf(parent_seed, sizeof parent_seed, "%s/full-worker.txt", workspace) > 0);
   assert(snprintf(private_sentinel, sizeof private_sentinel, "%s/read-only-parent-private.txt",
                   fixture_home) > 0);
   assert(snprintf(parent_attempt, sizeof parent_attempt, "%s/opaque-direct-write.txt", workspace) >
          0);
   if (full_worker_read_only)
   {
      full_worker_exact_file(parent_seed, "full worker owned write\n");
      FILE *sentinel = fopen(private_sentinel, "wx");
      assert(sentinel && !fchmod(fileno(sentinel), 0600) &&
             fputs("parent private state unchanged\n", sentinel) >= 0 && !fclose(sentinel));
      assert(access(parent_attempt, F_OK) && errno == ENOENT);
   }
   compute_ctx_t *context = create_compute_ctx(&server, &connection, request);
   assert(context);
   context->compute_executor_threads = 1;
   assert(db1_delegation_spawn_is_stopped("unrecorded-fixture-spawn") == -1);
   assert(db1_delegation_spawn_complete("unrecorded-fixture-spawn") == -1);
   delegate_worker(context);
   assert(full_spawn.recorded && full_spawn.completed && full_spawn.stop_reads > 0);
   assert(db1_delegation_spawn_is_stopped(full_spawn.id) == 1);
   assert(db1_delegation_spawn_is_stopped("unrecorded-fixture-spawn") == -1);
   if (full_worker_read_only)
   {
      full_worker_exact_file(parent_seed, "full worker owned write\n");
      full_worker_exact_file(private_sentinel, "parent private state unchanged\n");
      assert(access(parent_attempt, F_OK) && errno == ENOENT);
   }
   char capture_path[4096];
   assert(snprintf(capture_path, sizeof capture_path, "%s/full-worker-canonical.private.json",
                   fixture_home) < (int)sizeof capture_path);
   FILE *capture_file = fopen(capture_path, "wx");
   char *capture_json = cJSON_PrintUnformatted(stored_rows);
   assert(capture_file && capture_json && !fchmod(fileno(capture_file), 0600));
   assert(fwrite(capture_json, 1, strlen(capture_json), capture_file) == strlen(capture_json));
   assert(!fclose(capture_file));
   free(capture_json);
   cJSON *spawn_observation = cJSON_CreateObject();
   assert(spawn_observation && cJSON_AddStringToObject(spawn_observation, "id", full_spawn.id) &&
          cJSON_AddStringToObject(spawn_observation, "parent", full_spawn.parent) &&
          cJSON_AddStringToObject(spawn_observation, "session", full_spawn.session) &&
          cJSON_AddStringToObject(spawn_observation, "role", full_spawn.role) &&
          cJSON_AddNumberToObject(spawn_observation, "depth", full_spawn.depth) &&
          cJSON_AddNumberToObject(spawn_observation, "active_stop_reads", full_spawn.stop_reads) &&
          cJSON_AddBoolToObject(spawn_observation, "completed", full_spawn.completed));
   char *spawn_json = cJSON_PrintUnformatted(spawn_observation);
   assert(snprintf(capture_path, sizeof capture_path, "%s/full-worker-spawn.private.json",
                   fixture_home) < (int)sizeof capture_path);
   capture_file = fopen(capture_path, "wx");
   assert(capture_file && spawn_json && !fchmod(fileno(capture_file), 0600) &&
          fwrite(spawn_json, 1, strlen(spawn_json), capture_file) == strlen(spawn_json) &&
          !fclose(capture_file));
   free(spawn_json);
   cJSON_Delete(spawn_observation);
   cJSON_Delete(request);
   close(pair[0]);
   char wire[65536];
   size_t used = 0;
   for (;;)
   {
      struct pollfd pending = {.fd = pair[1], .events = POLLIN};
      assert(poll(&pending, 1, 2000) > 0 && used + 1 < sizeof wire);
      ssize_t count = read(pair[1], wire + used, sizeof wire - used - 1);
      assert(count >= 0);
      if (!count)
         break;
      used += (size_t)count;
   }
   wire[used] = 0;
   close(pair[1]);
   cJSON *result = aimee_ir_json_parse_frame(wire, used);
   if (!result || !cJSON_IsString(cJSON_GetObjectItem(result, "status")) ||
       strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(result, "status")), "ok"))
      fprintf(stderr, "actual full worker response: %s\n", wire);
   const char *status = result ? cJSON_GetStringValue(cJSON_GetObjectItem(result, "status")) : NULL;
   assert(status && !strcmp(status, "ok"));
   char owners_path[4096];
   assert(snprintf(owners_path, sizeof owners_path, "%s/acp-owners", fixture_home) > 0);
   DIR *owners = opendir(owners_path);
   assert(owners);
   struct dirent *entry;
   while ((entry = readdir(owners)))
      assert(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."));
   assert(!closedir(owners));

   aimee_response_t response = {0};
   assert(!aimee_ir_response_parse(cJSON_GetObjectItem(result, "ir"), &response));
   assert(!aimee_ir_response_complete_valid(&response));
   char *text = aimee_ir_response_text_alloc(&response);
   assert(text && !strcmp(text, "Full delegate worker ACP complete"));
   assert(response.usage_unobserved && cJSON_IsFalse(cJSON_GetObjectItem(result, "cost_known")));
   assert(cJSON_GetArraySize(stored_rows) > 0);
   assert(acp_route_policy_writes() == !full_worker_read_only);
   struct
   {
      char id[256];
      int result;
   } calls[4] = {0};
   int uses = 0, results = 0, bash = 0, last_result = -1, final_text = -1;
   const cJSON *row;
   cJSON_ArrayForEach(row, stored_rows)
   {
      const cJSON *event = cJSON_GetObjectItem(row, "event");
      const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(event, "kind"));
      if (!kind || strcmp(kind, "message"))
         continue;
      const cJSON *message = cJSON_GetObjectItem(event, "payload"), *block;
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(message, "role"));
      cJSON_ArrayForEach(block, cJSON_GetObjectItem(message, "blocks"))
      {
         const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type")),
                    *id = cJSON_GetStringValue(cJSON_GetObjectItem(block, "tool_id")),
                    *name = cJSON_GetStringValue(cJSON_GetObjectItem(block, "name")),
                    *body = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text"));
         int sequence = atoi(cJSON_GetStringValue(cJSON_GetObjectItem(row, "sequence")));
         if (role && !strcmp(role, "assistant") && body && !strcmp(body, text))
            final_text = sequence;
         if (!id || (strncmp(id, "acp-rpc:", 8) && strncmp(id, "acp-mcp:", 8)))
            continue;
         if (type && !strcmp(type, "tool_use"))
         {
            assert(role && !strcmp(role, "assistant") && uses < 4 &&
                   strlen(id) < sizeof calls[0].id);
            for (int i = 0; i < uses; i++)
               assert(strcmp(calls[i].id, id));
            strcpy(calls[uses++].id, id);
            bash += name && !strcmp(name, "bash");
         }
         if (type && !strcmp(type, "tool_result"))
         {
            assert(role && !strcmp(role, "tool") &&
                   cJSON_IsFalse(cJSON_GetObjectItem(block, "is_error")));
            int found = -1;
            for (int i = 0; i < uses; i++)
               if (!strcmp(calls[i].id, id))
                  found = i;
            assert(found >= 0 && !calls[found].result);
            calls[found].result = 1;
            results++;
            last_result = sequence;
         }
      }
   }
   int expected_pairs = full_worker_read_only ? 3 : 4;
   assert(uses == expected_pairs && results == expected_pairs && bash == 1 &&
          final_text > last_result);

   cJSON *bash_arguments = cJSON_Parse("{\"command\":\"read-only nested CLI\"}");
   assert(bash_arguments && !agent_tools_action_managed("bash", bash_arguments));
   cJSON_Delete(bash_arguments);
   /* Three read operations, plus a write in the code lane, journal; Bash keeps
    * its actual policy/source and result without an invented extra receipt. */
   assert(acp_route_policy_dispatches() == expected_pairs &&
          acp_route_policy_outcomes() == expected_pairs);
   free(text);
   aimee_response_free(&response);
   cJSON_Delete(result);
   puts("full worker: actual local UDS -> inline selection -> container workspace -> canonical ACP "
        "result");
   return 0;
}
