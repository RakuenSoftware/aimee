#include "aimee_sha256.h"
/* Factory lifecycle fixture. Peer owners are explicit mocks; actual private HOME,
 * executable descriptors, launch configuration and cleanup use production code.
 * Actual SDK/config protocol fidelity is covered by test_codex_native_adoption.py. */
#include "server_native_execution.h"
#include "server_native_sdk_profile.h"
#include "cli_claude_native.h"
#include "claude_headless.h"
#include "server_native_runtime.h"
#include "server_native_delegate_source.h"
#include "server_native_cache.h"
#include "server_native_cache_lease.h"
#include "server_native_containment.h"
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
#include <sys/wait.h>
#include <unistd.h>
#include <stdatomic.h>

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
static int refuse_runtime;
static int expected_turn_limit;
static char parent_role[64], private_home[4096], host_workspace[4096], observed_sdk_identity[65];
static int child_executable, child_directory;
static const char *active_sdk = "codex";
static aimee_wire_t expected_wire = AIMEE_WIRE_RESPONSES;
static const char *expected_provider, *expected_endpoint, *expected_auth;
static char observed_claude_cwd[4096];
static int bound_credential_home;
static const char digest[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
struct server_native_containment
{
   int old;
};
static struct server_native_containment boundary;
static int qualified_mode, reserved, quiesced, published_claim;
static atomic_int finished_reservation, disposed;
static int recovered, lookup_failure, preclaim_drop;
static atomic_int quiescence_failure, runtime_closed;
static atomic_int finish_failure;
static char observed_epoch[33] = "0";
static cJSON *durable_reservation;
static const char *active_override;
static int lookup_changed, lookup_missing, old_completed;
static const char *local_live_session, *local_live_actor, *local_live_turn;
static int local_owner_queries;
int server_session_capture_has_owner(const char *session, const char *actor, const char *turn)
{
   local_owner_queries++;
   return local_live_session && !strcmp(session, local_live_session) && local_live_actor &&
          !strcmp(actor, local_live_actor) && local_live_turn && !strcmp(turn, local_live_turn);
}
int db1_session_native_active_owned(const char *actor, const char *session, char **out)
{
   *out = NULL;
   if (!strcmp(session, "destination") && strlen(actor) == VAULT_PRINCIPAL_MAX - 1 &&
       strspn(actor, "L") == strlen(actor))
   {
      *out = strdup("[]");
      return *out ? 0 : -1;
   }
   if (strcmp(actor, "actor") || strcmp(session, "destination"))
      return -1;
   if (active_override)
   {
      *out = strdup(active_override);
      return 0;
   }
   cJSON *records = cJSON_CreateArray();
   if (durable_reservation)
      cJSON_AddItemToArray(records, cJSON_Duplicate(durable_reservation, 1));
   *out = cJSON_PrintUnformatted(records);
   cJSON_Delete(records);
   return *out ? 0 : -1;
}
int db1_session_native_epoch(const char *actor, const char *wire, char out[33])
{
   assert(!strcmp(actor, "actor"));
   cJSON *v = cJSON_Parse(wire);
   assert(v);
   cJSON_Delete(v);
   snprintf(out, 33, "%s", observed_epoch);
   return 0;
}
int db1_session_native_reservation_lookup_checked(const char *actor, const char *wire, char **out)
{
   assert(!strcmp(actor, "actor") && wire);
   *out = NULL;
   if (lookup_failure)
      return -1;
   if (lookup_missing)
      return 1;
   if (durable_reservation)
   {
      cJSON *copy = cJSON_Duplicate(durable_reservation, 1);
      if (lookup_changed)
         cJSON_ReplaceItemInObjectCaseSensitive(
             copy, "epoch", cJSON_CreateString("99999999999999999999999999999999"));
      *out = cJSON_PrintUnformatted(copy);
      cJSON_Delete(copy);
      return 0;
   }
   return 1;
}
int db1_session_native_reserve(const char *actor, const char *wire, char out[33])
{
   assert(!strcmp(actor, "actor") && !private_home[0] && !sdk_calls);
   assert(!durable_reservation);
   durable_reservation = cJSON_Parse(wire);
   assert(durable_reservation);
   cJSON_AddStringToObject(durable_reservation, "actor", actor);
   cJSON_AddStringToObject(durable_reservation, "state", "active");
   /* Real Go omitempty omits preclaim expected_token and claim_id. */
   reserved++;
   snprintf(out, 33, "%s", observed_epoch);
   return 0;
}
int db1_session_native_reservation_finish(const char *actor, const char *wire, char sequence[32],
                                          char epoch[33])
{
   if (preclaim_drop && !terminal_committed)
      return -1;
   assert(!strcmp(actor, "actor") && terminal_committed && quiesced && (completed || aborted));
   assert(durable_reservation && wire);
   if (finish_failure)
      return -1;
   finished_reservation++;
   snprintf(sequence, 32, "17");
   snprintf(epoch, 33, "0");
   cJSON_Delete(durable_reservation);
   durable_reservation = NULL;
   return 0;
}
int db1_session_native_reservation_finish_checked(const char *actor, const char *wire,
                                                  char sequence[32], char epoch[33], int *absent)
{
   *absent = 0;
   if (old_completed)
   {
      assert(quiesced && durable_reservation && !sdk_calls && !private_home[0]);
      cJSON *v = cJSON_Parse(wire);
      assert(v);
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(v, "expected_token")),
                     "complete:fixture:old-turn"));
      cJSON_Delete(v);
      cJSON_Delete(durable_reservation);
      durable_reservation = NULL;
      finished_reservation++;
      snprintf(sequence, 32, "17");
      snprintf(epoch, 33, "%s", observed_epoch);
      return 0;
   }
   return db1_session_native_reservation_finish(actor, wire, sequence, epoch);
}
int db1_session_native_reservation_reconcile(const char *actor, const char *wire, char sequence[32],
                                             char epoch[33])
{
   assert(!strcmp(actor, "actor") && wire && quiesced && !sdk_calls && !private_home[0]);
   cJSON *v = cJSON_Parse(wire);
   assert(v);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(v, "turn_id")),
                  preclaim_drop ? "turn" : "old-turn"));
   assert(!cJSON_GetObjectItem(v, "containment_json"));
   cJSON_Delete(v);
   cJSON_Delete(durable_reservation);
   durable_reservation = NULL;
   recovered++;
   snprintf(sequence, 32, "18");
   snprintf(epoch, 33, "11111111111111111111111111111111");
   snprintf(observed_epoch, sizeof observed_epoch, "%s", epoch);
   return 0;
}
int server_native_containment_probe(const char *root, const uint16_t *ports, size_t n,
                                    char error[512])
{
   assert(root && n == 1 && ports[0] == 45678 && !sdk_calls && !private_home[0]);
   (void)error;
   return qualified_mode ? 0 : -1;
}
server_native_containment_t *server_native_containment_open(const char *root, const uint16_t *ports,
                                                            size_t n, char error[512])
{
   assert(root && ports && n == 1 && !reserved);
   (void)error;
   return &boundary;
}
const cJSON *server_native_containment_manifest(const server_native_containment_t *owner)
{
   assert(owner == &boundary);
   static cJSON *v;
   if (!v)
      v = cJSON_Parse("{\"fixture\":\"owned-boundary\"}");
   return v;
}
int server_native_containment_sha256(const server_native_containment_t *owner, char out[65])
{
   assert(owner == &boundary);
   snprintf(out, 65, "%s", digest);
   return 0;
}
int server_native_containment_bind_home(server_native_containment_t *owner, const char *home)
{
   assert(owner == &boundary && !strcmp(active_sdk, "claude"));
   assert(home && strstr(home, "/n.") && !access(home, F_OK));
   assert(!bound_credential_home++);
   return 0;
}
const cli_native_spawn_guard_t *server_native_containment_guard(server_native_containment_t *owner)
{
   static cli_native_spawn_guard_t g;
   assert(!owner || owner == &boundary);
   return owner ? &g : NULL;
}
server_native_containment_t *server_native_containment_reopen(const cJSON *manifest,
                                                              char error[512])
{
   (void)error;
   assert(cJSON_IsObject(manifest) && durable_reservation);
   return &boundary;
}
int server_native_containment_quiesce(server_native_containment_t *owner, int timeout)
{
   assert(owner == &boundary && (timeout == 10000 || timeout == 1000));
   quiesced++;
   return quiescence_failure ? -1 : 0;
}
void server_native_containment_close(server_native_containment_t *owner, int remove_empty)
{
   if (owner)
   {
      assert(owner == &boundary);
      if (remove_empty)
         assert(finished_reservation || recovered);
      if (remove_empty && (finished_reservation || recovered))
         atomic_store(&disposed, 1);
   }
}
int server_native_cache_epoch_binding(const char *binding, const char *epoch, char out[65])
{
   assert(!strcmp(epoch, "0") || !strcmp(epoch, "11111111111111111111111111111111"));
   snprintf(out, 65, "%s",
            !strcmp(epoch, "0")
                ? binding
                : "1111111111111111111111111111111111111111111111111111111111111111");
   return 0;
}
cJSON *server_native_cache_claim_json(const server_native_cache_t *cache)
{
   assert(cache);
   return cJSON_Parse("{\"expected_token\":\"complete:fixture:turn\"}");
}
server_native_cache_t *server_native_cache_begin_epoch(const char *session, const char *actor,
                                                       const char *turn, const char *harness,
                                                       const char *scope,
                                                       const aimee_request_t *request,
                                                       const char *epoch,
                                                       const server_native_cache_owner_t *owner)
{
   assert(!strcmp(epoch, observed_epoch));
   if (owner)
   {
      assert(reserved && (!quiesced || recovered) && owner->publish && owner->owner);
      cJSON *claim = cJSON_Parse("{\"expected_token\":\"pending:fixture:turn\"}");
      assert(!owner->publish(claim, owner->context));
      cJSON_Delete(claim);
      published_claim++;
   }
   return server_native_cache_begin(session, actor, turn, harness, scope, request);
}
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
   assert(held == &capture && joined && (child_closed || !sdk_calls) && !terminal);
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
int agent_tools_catalog(aimee_wire_t wire, aimee_request_t *out)
{
   assert(wire == expected_wire);
   out->n_tools = 1;
   out->tools = calloc(1, sizeof *out->tools);
   assert(out->tools);
   out->tools[0].name = strdup("read_file");
   out->tools[0].schema = cJSON_Parse("{\"type\":\"object\"}");
   return 0;
}
int agent_tools_catalog_filter(aimee_request_t *tools, const char *role)
{
   assert(tools && tools->n_tools >= 0 && tools->n_tools <= 1);
   assert(!strcmp(parent_role, role ? role : "primary"));
   return 0;
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
static int expected_no_tools;
int agent_prepare_request_ir(const agent_t *agent, const cJSON *messages,
                             const aimee_request_t *tools, const char *system, int max_tokens,
                             double temperature, aimee_wire_t wire, int delegate,
                             aimee_request_t *out)
{
   assert(max_tokens == 0 && temperature == -1);
   (void)delegate;
   assert(cJSON_GetArraySize(messages) == 0 && tools);
   assert(tools->n_tools == (expected_no_tools ? 0 : 1));
   assert(!strcmp(system, "host environment") && wire == expected_wire);
   if (expected_provider)
      assert(!strcmp(agent->provider, expected_provider));
   if (expected_endpoint)
      assert(!strcmp(agent->endpoint, expected_endpoint));
   if (expected_auth)
      assert(!strcmp(agent->auth_type, expected_auth));
   out->model = strdup(agent->model);
   if (expected_no_tools)
      return 0;
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
   if (durable_reservation)
      assert(reserved);
   *out = mode == 1 ? NULL
                    : strdup(mode == 10 ? "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\","
                                          "\"text\":\"prior task\"}]},"
                                          "{\"role\":\"assistant\",\"blocks\":[{\"type\":\"text\","
                                          "\"text\":\"prior original\"}]}]"
                                        : "[]");
   return mode == 1 ? -1 : 0;
}
int server_native_history_load(const char *session, const char *actor, const char *active,
                               int running, cJSON **messages, cJSON **receipts)
{
   assert(!running);
   char *wire = NULL;
   int rc = db1_session_ir_history_load(session, "primary", actor, active, &wire);
   *messages = !rc && wire ? cJSON_Parse(wire) : NULL;
   *receipts = cJSON_CreateArray();
   free(wire);
   return rc || !*messages || aimee_ir_history_admit(*messages) ? -1 : 0;
}
int server_native_history_admit_source(const char *session, const char *actor, const char *active,
                                       int running, const cJSON *messages)
{
   assert(!strcmp(session, "destination") && !strcmp(actor, "actor") && !strcmp(active, "turn") &&
          running);
   return aimee_ir_history_admit(messages);
}
int server_native_runtime_set_native_owner(server_native_runtime_t *value, const cJSON *reservation)
{
   assert(value == &runtime && reserved && durable_reservation && !private_home[0] && !sdk_calls);
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(reservation, "owner_id")));
   assert(!cJSON_GetObjectItemCaseSensitive(reservation, "containment_json"));
   return 0;
}
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
   assert(!"normal attested factory fixture requested a service capability");
   return -1;
}
server_native_runtime_t *server_native_runtime_open_bound_request(
    const agent_t *selected, const request_context_t *request_owner, const char *source,
    const aimee_request_t *profile, const server_native_tool_binding_t *bindings, int count)
{
   assert(request_owner == &owner && !strcmp(source, "transport"));
   assert(!strcmp(selected->model, "selected-model") && profile->n_messages == 0);
   if (!strcmp(active_sdk, "claude"))
      assert(profile->n_system > 0 &&
             !strcmp(profile->system[0].text, AIMEE_NATIVE_CLAUDE_IDENTITY));
   else
      assert(!profile->n_system || strcmp(profile->system[0].text, AIMEE_NATIVE_CLAUDE_IDENTITY));
   assert(!strcmp(selected->provider, expected_provider ? expected_provider : "chatgpt"));
   if (expected_endpoint)
      assert(!strcmp(selected->endpoint, expected_endpoint));
   if (expected_auth)
      assert(!strcmp(selected->auth_type, expected_auth));
   assert(!durable_reservation); /* Session recovery precedes current profile. */
   if (refuse_runtime)
      return NULL;
   if (expected_no_tools)
   {
      assert(!profile->n_tools && !count && !bindings);
      opened++;
      return &runtime;
   }
   assert(profile->n_tools == 1 && profile->tools[0].kind == AIMEE_TOOL_FUNCTION);
   const char *logical = !strcmp(active_sdk, "claude") ? "mcp__aimee__read_file" : "read_file";
   assert(!strcmp(profile->tools[0].name, logical));
   assert(count == 1 && bindings && !bindings[0].space);
   assert(!strcmp(bindings[0].name, logical) && !strcmp(bindings[0].registry_name, "read_file"));
   assert(!strcmp(active_sdk, "claude")
              ? bindings[0].mcp_name && !strcmp(bindings[0].mcp_name, "read_file")
              : !bindings[0].mcp_name);
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
int server_native_runtime_set_turn_limit(server_native_runtime_t *r, int limit)
{
   assert(r == &runtime && limit == expected_turn_limit);
   return 0;
}
int server_native_runtime_turn_limit_reached(const server_native_runtime_t *r)
{
   assert(r == &runtime);
   return 0;
}
int server_native_runtime_model_turns(const server_native_runtime_t *r)
{
   assert(r == &runtime);
   return 0;
}
int server_native_runtime_attest_launch(server_native_runtime_t *r, const char *proof)
{
   assert(r == &runtime && strlen(proof) == 64 && !child_closed);
   if (!strcmp(active_sdk, "codex"))
      assert(!strcmp(proof, digest));
   return 0;
}
int server_native_runtime_cache_scope(const server_native_runtime_t *r,
                                      const server_native_execution_profile_t *profile,
                                      char scope[65])
{
   assert(r == &runtime && !strcmp(profile->sdk, active_sdk));
   assert(strlen(profile->build_digest) == 64 && strcmp(profile->build_digest, digest));
   assert(strlen(profile->launch_digest) == 64 && !strcmp(profile->workspace_digest, digest));
   if (!strcmp(active_sdk, "codex"))
      assert(!strcmp(profile->launch_digest, digest));
   snprintf(scope, 65, "%s", digest);
   return 0;
}
void server_native_runtime_deactivate_hold_port(server_native_runtime_t *r)
{
   if (r)
   {
      assert(r == &runtime);
      joined = 1;
   }
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
      runtime_closed++;
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
   out->n_content = mode == 19 ? 4 : mode == 11 ? 2 : 1;
   out->content = calloc((size_t)out->n_content, sizeof *out->content);
   out->content[0].type = AIMEE_BLK_TEXT;
   out->content[0].text = strdup("ORIGINAL canonical output");
   if (mode == 11)
      out->content[1] = (aimee_block_t){.type = AIMEE_BLK_THINKING,
                                        .text = strdup("original reasoning"),
                                        .thinking_id = strdup("original-reasoning-id"),
                                        .thinking_encrypted_content = strdup("opaque-original")};
   if (mode == 19)
   {
      out->content[1] = (aimee_block_t){.type = AIMEE_BLK_IMAGE,
                                        .media_type = strdup("image/png"),
                                        .media_ref = strdup("data:image/png;base64,cGl4ZWw=")};
      out->content[1].raw = cJSON_Parse("{\"native_media\":{\"id\":9007199254740993}}");
      out->content[2] = (aimee_block_t){.type = AIMEE_BLK_DOCUMENT,
                                        .media_type = strdup("application/pdf"),
                                        .media_ref = strdup("https://owned.invalid/document.pdf")};
      out->content[3] =
          (aimee_block_t){.type = AIMEE_BLK_THINKING,
                          .text = strdup("retained thought"),
                          .thinking_encrypted_content = strdup("opaque-encrypted-thought")};
   }
   if (mode == 20)
   {
      free(out->content[0].text);
      out->content[0] = (aimee_block_t){.type = AIMEE_BLK_IMAGE,
                                        .media_type = strdup("image/png"),
                                        .media_ref = strdup("data:image/png;base64,cGl4ZWw=")};
   }
   out->usage_in = 73;
   out->usage_out = 29;
   out->usage_cache_read = 11;
   out->usage_cache_write = 7;
   out->stop_reason = mode == 4                  ? AIMEE_STOP_TOOL_USE
                      : mode == 12 || mode == 15 ? AIMEE_STOP_CONTENT_FILTER
                      : mode == 13               ? AIMEE_STOP_MAX_TOKENS
                      : mode == 14               ? AIMEE_STOP_UNKNOWN
                      : mode == 16               ? AIMEE_STOP_ERROR
                                                 : AIMEE_STOP_END_TURN;
   if (mode == 12 || mode == 15)
   {
      out->raw_stop_reason = strdup("refusal");
      out->refusal = calloc(1, sizeof *out->refusal);
      out->refusal->category = strdup("future_provider_category");
      out->refusal->explanation = strdup("retained provider detail");
   }
   if (mode == 16)
      out->error_message = strdup("retained upstream diagnostic");
   if (mode == 17)
      out->content[0].type = AIMEE_BLK_THINKING;
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
   assert(!strcmp(harness, active_sdk) && !strcmp(scope, digest));
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
   return "native-result";
}
int server_native_cache_complete(server_native_cache_t *c, const char *id)
{
   assert(c == &cache && terminal_committed && joined && child_closed);
   assert(!strcmp(id, "native-result"));
   completed++;
   if (durable_reservation && !cJSON_GetObjectItem(durable_reservation, "expected_token"))
      cJSON_AddStringToObject(durable_reservation, "expected_token", "");
   if (durable_reservation)
      cJSON_ReplaceItemInObjectCaseSensitive(durable_reservation, "expected_token",
                                             cJSON_CreateString("complete:fixture:turn"));
   return mode == 7 ? -1 : 0;
}
int server_native_cache_abort(server_native_cache_t *c)
{
   assert(c == &cache && joined && child_closed);
   aborted++;
   if (durable_reservation && !cJSON_GetObjectItem(durable_reservation, "expected_token"))
      cJSON_AddStringToObject(durable_reservation, "expected_token", "");
   if (durable_reservation)
      cJSON_ReplaceItemInObjectCaseSensitive(durable_reservation, "expected_token",
                                             cJSON_CreateString("invalid:fixture:turn"));
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
   assert((launch->spawn_guard != NULL) == (reserved != 0));
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
          !strcmp(replay.resume_id, "native-result"));
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
   return mode == 15 || mode == 16 || mode == 18 ? -1 : rc;
}
const provider_cli_adapter_t codex_provider_cli_adapter = {.cli_kind = "codex",
                                                           .execute = sdk_execute};
typedef struct
{
   struct stat program, directory;
} stdio_launch_identity_t;
static int stdio_guard_child(void *context, int executable, int cwd)
{
   stdio_launch_identity_t *expected = context;
   struct stat program, directory;
   assert(executable >= 0 && cwd >= 0 && executable != cwd);
   assert(!fstat(executable, &program) && !fstat(cwd, &directory));
   assert(program.st_dev == expected->program.st_dev && program.st_ino == expected->program.st_ino);
   assert(directory.st_dev == expected->directory.st_dev &&
          directory.st_ino == expected->directory.st_ino);
   return 0;
}
static int stdio_guard_parent(void *context, pid_t child)
{
   assert(context && child > 0);
   return 0;
}
int claude_headless_run(const provider_cli_cfg_t *cfg, claude_headless_session_fn bind,
                        void *context, agent_result_t *out)
{
   assert(!strcmp(active_sdk, "claude") && cfg->claude_host && !cfg->codex_host && !cfg->home);
   assert(!strcmp(cfg->user_prompt, "Continue the server-owned canonical turn.") &&
          !cfg->system_prompt);
   assert(!cfg->history && !strcmp(cfg->resume_id, "native-result") && began == 1);
   const cli_claude_host_launch_t *host = cfg->claude_host;
   assert(host->sdk_identity && strlen(host->sdk_identity) == 64 &&
          strspn(host->sdk_identity, "0123456789abcdef") == 64);
   snprintf(observed_sdk_identity, sizeof observed_sdk_identity, "%s", host->sdk_identity);

   assert(bound_credential_home == (host->spawn_guard ? 1 : 0));
   snprintf(observed_claude_cwd, sizeof observed_claude_cwd, "%s", host->cwd);
   char proof[65];
   assert(!cli_claude_native_launch_digest(cfg, proof) && !strcmp(proof, host->expected_profile));
   /* Exact deterministic pre-stable-cwd launch contract for this fixture.
    * Old complete handles must not be admitted under the new cwd policy. */
   assert(strcmp(proof, "a0d85eb248a7007b6c4b2865a72206439a5e29febd33e61bc2c59c590112ff46"));
   /* Exact prior stable-cwd/no-device-identity contract measured from c829.
    * Its complete handles predate the nonsecret identity binding. */
   assert(strcmp(proof, "6ad2d900433affaaf554d1213ac88efe2949d1d4a149b674d60bd25e42c27218"));
   /* The exact fixed gateway setting is covered by the launch proof, not
    * inherited from the operator or inferred from SDK initialization. */
   int attribution_index = -1, readiness_index = -1;
   char *changed_environment[17] = {0};
   for (int i = 0; host->environment[i]; i++)
   {
      assert(i < 16);
      changed_environment[i] = host->environment[i];
      if (!strncmp(host->environment[i], "MCP_CONNECTION_NONBLOCKING=", 27))
      {
         assert(!strcmp(host->environment[i], "MCP_CONNECTION_NONBLOCKING=0"));
         assert(readiness_index == -1);
         readiness_index = i;
      }
      if (!strncmp(host->environment[i], "CLAUDE_CODE_ATTRIBUTION_HEADER=", 31))
      {
         assert(!strcmp(host->environment[i], "CLAUDE_CODE_ATTRIBUTION_HEADER=0"));
         assert(attribution_index == -1);
         attribution_index = i;
      }
   }
   assert(attribution_index >= 0 && readiness_index >= 0);
   cli_claude_host_launch_t changed_host = *host;
   provider_cli_cfg_t changed_cfg = *cfg;
   changed_host.environment = changed_environment;
   changed_cfg.claude_host = &changed_host;
   changed_host.sdk_identity = NULL;
   assert(cli_claude_native_launch_digest(&changed_cfg, proof));
   changed_host.sdk_identity = "bad";
   assert(cli_claude_native_launch_digest(&changed_cfg, proof));
   changed_host.sdk_identity = host->sdk_identity;
   /* Valid replacement correlation selects a different cache profile; mismatch,
    * unsafe config custody and missing identity never adopt the old profile. */
   char config_path[8192], original_config[2048], replacement[65];
   assert(snprintf(config_path, sizeof config_path, "%s/.claude/.claude.json", host->home) <
          (int)sizeof config_path);
   FILE *config_file = fopen(config_path, "r");
   assert(config_file);
   size_t config_size = fread(original_config, 1, sizeof original_config - 1, config_file);
   assert(!ferror(config_file) && feof(config_file) && !fclose(config_file));
   original_config[config_size] = 0;
   cJSON *config_value = cJSON_Parse(original_config);
   assert(config_value);
   memset(replacement, host->sdk_identity[0] == 'e' ? 'f' : 'e', 64);
   replacement[64] = 0;
   assert(
       cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(config_value, "userID"), replacement));
   char *changed_config = cJSON_PrintUnformatted(config_value);
   cJSON_Delete(config_value);
   assert(changed_config);
   config_file = fopen(config_path, "w");
   assert(config_file &&
          fwrite(changed_config, 1, strlen(changed_config), config_file) ==
              strlen(changed_config) &&
          !fclose(config_file));
   free(changed_config);
   assert(cli_claude_native_launch_digest(cfg, proof));
   changed_host.sdk_identity = replacement;
   assert(!cli_claude_native_launch_digest(&changed_cfg, proof) &&
          strcmp(proof, host->expected_profile));
   assert(!chmod(config_path, 0644) && cli_claude_native_launch_digest(&changed_cfg, proof));
   assert(!chmod(config_path, 0600));
   config_file = fopen(config_path, "w");
   assert(config_file && fwrite(original_config, 1, config_size, config_file) == config_size &&
          !fclose(config_file));
   changed_host.sdk_identity = host->sdk_identity;
   assert(!cli_claude_native_launch_digest(&changed_cfg, proof) &&
          !strcmp(proof, host->expected_profile));

   changed_environment[attribution_index] = "CLAUDE_CODE_ATTRIBUTION_HEADER=1";
   assert(cli_claude_native_launch_digest(&changed_cfg, proof));
   changed_environment[attribution_index] = NULL;
   assert(cli_claude_native_launch_digest(&changed_cfg, proof));
   changed_environment[attribution_index] = host->environment[attribution_index];
   int environment_count = 0;
   while (host->environment[environment_count])
      environment_count++;
   changed_environment[environment_count] = host->environment[attribution_index];
   assert(cli_claude_native_launch_digest(&changed_cfg, proof));
   changed_environment[environment_count] = NULL;
   changed_environment[readiness_index] = "MCP_CONNECTION_NONBLOCKING=1";
   assert(cli_claude_native_launch_digest(&changed_cfg, proof));
   changed_environment[readiness_index] = NULL;
   assert(cli_claude_native_launch_digest(&changed_cfg, proof));
   changed_environment[readiness_index] = host->environment[readiness_index];
   changed_environment[environment_count] = host->environment[readiness_index];
   assert(cli_claude_native_launch_digest(&changed_cfg, proof));
#if defined(__linux__)
   /* Run the real descriptor-based spawn and inspect its actual child env.
    * This is a probe executable, not a simulated provider response. */
   cli_claude_host_launch_t probe_host = *host;
   provider_cli_cfg_t probe_cfg = *cfg;
   probe_host.executable_fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
   assert(probe_host.executable_fd >= 0);
   probe_host.spawn_guard = NULL;
   probe_cfg.claude_host = &probe_host;
   for (int holes = 0; holes < 8; holes++)
   {
      pid_t verifier = fork();
      assert(verifier >= 0);
      if (!verifier)
      {
         for (int fd = 0; fd < 3; fd++)
            if (holes & (1 << fd))
               close(fd);
         cli_claude_host_launch_t launched = probe_host;
         launched.executable_fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
         launched.cwd_fd = open(host->cwd, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
         assert(launched.executable_fd >= 0 && launched.cwd_fd >= 0);
         stdio_launch_identity_t identity;
         assert(!fstat(launched.executable_fd, &identity.program) &&
                !fstat(launched.cwd_fd, &identity.directory));
         cli_native_spawn_guard_t guard = {stdio_guard_child, stdio_guard_parent, &identity};
         launched.spawn_guard = &guard;
         provider_cli_cfg_t launch = probe_cfg;
         launch.claude_host = &launched;
         int input = -1, output = -1, status = -1;
         pid_t probe;
         assert(!cli_claude_native_spawn(&launch, &input, &output, &probe));
         close(input);
         char observed[128] = {0};
         ssize_t bytes = read(output, observed, sizeof observed - 1);
         assert(bytes > 0 && !strcmp(observed, "native Claude gateway environment verified\n"));
         close(output);
         assert(waitpid(probe, &status, 0) == probe && WIFEXITED(status) && !WEXITSTATUS(status));
         close(launched.executable_fd);
         close(launched.cwd_fd);
         _exit(0);
      }
      int status;
      assert(waitpid(verifier, &status, 0) == verifier && WIFEXITED(status) &&
             !WEXITSTATUS(status));
   }
   close(probe_host.executable_fd);
#endif
   snprintf(private_home, sizeof private_home, "%s", host->home);
   child_executable = host->executable_fd;
   child_directory = host->cwd_fd;
   char path[4200];
   snprintf(path, sizeof path, "%s/.claude/.credentials.json", host->home);
   assert(access(path, F_OK));
   cJSON *frame = cJSON_CreateObject();
   cJSON_AddStringToObject(frame, "type", "system");
   cJSON_AddStringToObject(frame, "subtype", "init");
   cJSON_AddStringToObject(frame, "cwd", host->cwd);
   cJSON_AddStringToObject(frame, "session_id", "native-result");
   cJSON_AddStringToObject(frame, "apiKeySource", "ANTHROPIC_API_KEY");
   cJSON_AddStringToObject(frame, "permissionMode", "default");
   cJSON *tools = cJSON_AddArrayToObject(frame, "tools");
   cJSON_AddItemToArray(tools, cJSON_CreateString("mcp__aimee__read_file"));
   cJSON *servers = cJSON_AddArrayToObject(frame, "mcp_servers"), *server = cJSON_CreateObject();
   cJSON_AddStringToObject(server, "name", "aimee");
   cJSON_AddStringToObject(server, "status", "connected");
   cJSON_AddItemToArray(servers, server);
   cJSON_AddArrayToObject(frame, "plugins");
   assert(!cli_claude_native_verify(host, frame));
   const char *bad_keys[] = {"cwd", "apiKeySource", "permissionMode", "subtype"};
   const char *bad_reasons[] = {"workspace", "API source", "permission mode", "init frame"};
   for (unsigned i = 0; i < sizeof bad_keys / sizeof bad_keys[0]; i++)
   {
      cJSON *bad = cJSON_Duplicate(frame, 1);
      cJSON_ReplaceItemInObjectCaseSensitive(bad, bad_keys[i], cJSON_CreateString("foreign"));
      assert(cli_claude_native_verify(host, bad));
      assert(!strcmp(cli_claude_native_profile_error(host, bad), bad_reasons[i]));
      cJSON_Delete(bad);
   }
   const char *shape_reasons[] = {"tool count", "tool count", "plugins", "MCP connection"};
   for (int i = 0; i < 4; i++)
   {
      cJSON *bad = cJSON_Duplicate(frame, 1);
      if (i == 0)
         cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(bad, "tools"),
                              cJSON_CreateString("Bash"));
      else if (i == 1)
         cJSON_DeleteItemFromArray(cJSON_GetObjectItemCaseSensitive(bad, "tools"), 0);
      else if (i == 2)
         cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(bad, "plugins"),
                              cJSON_CreateString("unowned-plugin"));
      else
         cJSON_ReplaceItemInObjectCaseSensitive(
             cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(bad, "mcp_servers"), 0), "status",
             cJSON_CreateString("failed"));
      assert(cli_claude_native_verify(host, bad));
      assert(!strcmp(cli_claude_native_profile_error(host, bad), shape_reasons[i]));
      cJSON_Delete(bad);
   }
   cJSON *bad_name = cJSON_Duplicate(frame, 1);
   cJSON_ReplaceItemInArray(cJSON_GetObjectItemCaseSensitive(bad_name, "tools"), 0,
                            cJSON_CreateString("unowned-value-must-not-be-logged"));
   assert(!strcmp(cli_claude_native_profile_error(host, bad_name), "tool names"));
   cli_claude_profile_summary_t summary;
   cli_claude_native_profile_summary(host, bad_name, &summary);
   assert(summary.expected_tools == 1 && summary.reported_tools == 1 && summary.mcp_servers == 1 &&
          summary.aimee_connected == 1 && summary.plugins == 0);
   char expected_hash[65];
   assert(
       !aimee_sha256_hex("mcp__aimee__read_file", strlen("mcp__aimee__read_file"), expected_hash));
   assert(!strcmp(summary.missing_owned_sha256, expected_hash));
   const char *unexpected = "unowned-value-must-not-be-logged";
   assert(!aimee_sha256_hex(unexpected, strlen(unexpected), expected_hash));
   assert(!strcmp(summary.unexpected_reported_sha256, expected_hash));
   assert(!strcmp(cJSON_GetStringValue(
                      cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(bad_name, "tools"), 0)),
                  unexpected));
   cJSON_Delete(bad_name);
   /* SDK status strings are untrusted. Only the closed enum can reach logs,
    * including when a future SDK emits private diagnostic text as status. */
   const char *statuses[] = {"pending",    "failed",
                             "needs-auth", "disabled",
                             "connected",  "PRIVATE_SDK_STATUS=https://secret.invalid/token"};
   const char *safe_statuses[] = {"pending",  "failed",    "needs-auth",
                                  "disabled", "connected", "unknown"};
   for (unsigned i = 0; i < sizeof statuses / sizeof statuses[0]; i++)
   {
      cJSON *status_frame = cJSON_Duplicate(frame, 1);
      cJSON *status_server =
          cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(status_frame, "mcp_servers"), 0);
      assert(cJSON_ReplaceItemInObjectCaseSensitive(status_server, "status",
                                                    cJSON_CreateString(statuses[i])));
      cli_claude_native_profile_summary(host, status_frame, &summary);
      assert(!strcmp(cli_claude_mcp_status_name(summary.aimee_status), safe_statuses[i]));
      assert(!strstr(cli_claude_mcp_status_name(summary.aimee_status), "PRIVATE_SDK_STATUS"));
      cJSON_Delete(status_frame);
   }
   assert(!strcmp(cli_claude_mcp_status_name((cli_claude_mcp_status_t)-1), "unknown"));
   cli_claude_native_profile_summary(host, frame, &summary);
   assert(!strcmp(summary.missing_owned_sha256, "-") &&
          !strcmp(summary.unexpected_reported_sha256, "-"));
   cJSON *bad_servers = cJSON_Duplicate(frame, 1);
   cJSON_DeleteItemFromArray(cJSON_GetObjectItemCaseSensitive(bad_servers, "mcp_servers"), 0);
   assert(!strcmp(cli_claude_native_profile_error(host, bad_servers), "MCP server count"));
   cJSON_Delete(bad_servers);
   assert(!cli_claude_native_profile_error(host, frame));
   char moved_cwd[8192];
   assert(snprintf(moved_cwd, sizeof moved_cwd, "%s-replaced", host->cwd) < (int)sizeof moved_cwd);
   assert(!rename(host->cwd, moved_cwd) && !mkdir(host->cwd, 0700));
   assert(
       host->init(frame, host->context)); // Retained fd cannot attest a different pathname inode.
   assert(!rmdir(host->cwd) && !rename(moved_cwd, host->cwd));
   assert(!host->init(frame, host->context));
   assert(!bind("native-result", context));
   cJSON_Delete(frame);
   sdk_calls++;
   child_closed = 1;
   out->success = 1;
   out->response = strdup("SDK REWRITE MUST NOT BECOME CANONICAL");
   return 0;
}

static void seed_abandoned(const char *actor)
{
   assert(!durable_reservation);
   durable_reservation = cJSON_CreateObject();
   cJSON_AddNumberToObject(durable_reservation, "version", 1);
   cJSON_AddStringToObject(durable_reservation, "session_id", "destination");
   cJSON_AddStringToObject(durable_reservation, "turn_id", "old-turn");
   cJSON_AddStringToObject(durable_reservation, "harness", "codex");
   cJSON_AddStringToObject(durable_reservation, "binding_sha256", digest);
   cJSON_AddStringToObject(durable_reservation, "owner_id", "22222222222222222222222222222222");
   cJSON_AddStringToObject(durable_reservation, "containment_sha256", digest);
   cJSON_AddStringToObject(durable_reservation, "containment_json",
                           "{\"fixture\":\"owned-boundary\"}");
   cJSON_AddStringToObject(durable_reservation, "epoch", "0");
   cJSON_AddStringToObject(durable_reservation, "state", "active");
   cJSON_AddStringToObject(durable_reservation, "actor", actor);
}
static void reset(int next)
{
   assert(!terminal && !retained);
   mode = next;
   owner.vault_transport = ATTEST_WEBCHAT_TRUSTED;
   expected_wire = AIMEE_WIRE_RESPONSES;
   expected_provider = expected_endpoint = expected_auth = NULL;
   reserved = quiesced = finished_reservation = published_claim = 0;
   disposed = 0;
   runtime_closed = 0;
   active_override = NULL;
   lookup_changed = lookup_missing = old_completed = 0;
   local_live_session = local_live_actor = local_live_turn = NULL;
   local_owner_queries = 0;
   recovered = lookup_failure = quiescence_failure = finish_failure = preclaim_drop = 0;
   snprintf(observed_epoch, sizeof observed_epoch, "0");
   opened = joined = child_closed = began = completed = aborted = closed = 0;
   terminal_committed = sdk_calls = restored = ready_calls = rejected_terminal = 0;
   child_executable = child_directory = -1;
   private_home[0] = 0;
   bound_credential_home = 0;
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
/* This fixture exercises the real strong router and native owner. External ACP
 * has its own real-process/public-route fixture and must not enter this one. */
int server_acp_execute_owned(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   (void)cfg;
   (void)out;
   assert(!"unexpected ACP route in native factory fixture");
   return AGENT_RC_CONTEXT_REFUSED;
}

int main(int argc, char **argv)
{
   if (argc > 1 && !strcmp(argv[1], "--bare"))
   {
      struct stat channel;
      for (int fd = 0; fd < 3; fd++)
         assert(!fstat(fd, &channel) && S_ISFIFO(channel.st_mode));
      const char *setting = getenv("CLAUDE_CODE_ATTRIBUTION_HEADER");
      assert(setting && !strcmp(setting, "0"));
      setting = getenv("MCP_CONNECTION_NONBLOCKING");
      assert(setting && !strcmp(setting, "0"));
      assert(!getenv("PARENT_SECRET") && !getenv("ANTHROPIC_AUTH_TOKEN"));
      assert(getenv("ANTHROPIC_API_KEY") &&
             !strcmp(getenv("ANTHROPIC_API_KEY"), "local-capability-fixture"));
      /* The real factory's descriptor spawn has already fchdir'd this probe.
       * Read the exact path selected by the SDK environment, not a duplicate
       * test-generated config or the original operator/tool workspace. */
      char actual_cwd[4096], config_path[8192], config_bytes[16385];
      const char *config_dir = getenv("CLAUDE_CONFIG_DIR");
      assert(config_dir && getcwd(actual_cwd, sizeof actual_cwd));
      assert(snprintf(config_path, sizeof config_path, "%s/.claude.json", config_dir) <
             (int)sizeof config_path);
      FILE *config = fopen(config_path, "r");
      assert(config);
      size_t count = fread(config_bytes, 1, sizeof config_bytes - 1, config);
      assert(!ferror(config) && feof(config) && !fclose(config));
      config_bytes[count] = 0;
      cJSON *value = cJSON_Parse(config_bytes);
      assert(cJSON_IsObject(value));
      cJSON *projects = cJSON_GetObjectItemCaseSensitive(value, "projects");
      assert(cJSON_IsObject(projects) && cJSON_GetArraySize(projects) == 1);
      cJSON *project = cJSON_GetObjectItemCaseSensitive(projects, actual_cwd);
      assert(project && "factory config must key trust to actual SDK getcwd");
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(project, "hasTrustDialogAccepted")));
      cJSON_Delete(value);
      puts("native Claude gateway environment verified");
      return 0;
   }
   char temporary[4096];
   assert(snprintf(temporary, sizeof temporary, "%s/native-factory-XXXXXX", platform_tmpdir()) <
          (int)sizeof temporary);
   assert(mkdtemp(temporary));
   snprintf(host_workspace, sizeof host_workspace, "%s", temporary);
   run_cmd_set_cwd(host_workspace);
   setenv("AIMEE_OAUTH_RUNTIME_DIR", temporary, 1);
   setenv("AIMEE_HOME", temporary, 1);
   setenv("PARENT_SECRET", "do-not-inherit", 1);
   setenv("CLAUDE_CODE_ATTRIBUTION_HEADER", "1", 1);
   setenv("MCP_CONNECTION_NONBLOCKING", "1", 1);
   agent_t selected = {0};
   snprintf(selected.cli_kind, sizeof selected.cli_kind, "codex");
   snprintf(selected.cli_cmd, sizeof selected.cli_cmd, "/bin/true");
   snprintf(selected.provider, sizeof selected.provider, "chatgpt");
   snprintf(selected.model, sizeof selected.model, "selected-model");
   provider_cli_cfg_t cfg = {.agent = &selected, .role = "worker", .user_prompt = "current task"};
#if defined(__linux__)
   /* Runtime preparation also resolves credential/source lineage. A refusal
    * must not misdiagnose that boundary as a tool-catalog defect or model reply. */
   reset(0);
   refuse_runtime = 1;
   agent_result_t preparation_refused = {0};
   assert(provider_cli_adapter_host_execute(&cfg, &preparation_refused) < 0);
   assert(!preparation_refused.success && !preparation_refused.canonical_response.content &&
          !preparation_refused.canonical_response.n_content);
   assert(!strcmp(preparation_refused.error,
                  "native SDK launch refused during native runtime preparation"));
   assert(!sdk_calls && !opened && !began && !reserved && !private_home[0] && !retained);
   assert(restored == 1);
   agent_result_clear_response(&preparation_refused);
   refuse_runtime = 0;
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
         char lease_error[512];
         assert(!server_native_cache_lease_open(getenv("AIMEE_HOME"), active_sdk, "destination",
                                                digest, "actor", "concurrent-turn", lease_error));
         assert(strstr(lease_error, "busy"));
         finish(scenario != 6);
         assert(completed == (scenario == 6 ? 0 : 1));
         assert(aborted == ((scenario == 6 || scenario == 7) ? 1 : 0) && closed == 1);
      }
      else
      {
         assert(!terminal && !completed);
         assert(!began || (aborted == 1 && closed == 1));
      }
      agent_result_clear_response(&out);
   }
   /* Provider declines/partials and failed SDK framing retain original IR,
    * never the SDK display, and cannot publish a successful native cache. */
   for (int diagnostic = 12; diagnostic <= 18; diagnostic++)
   {
      reset(diagnostic);
      aimee_response_t original = {0};
      cfg.canonical_response = &original;
      agent_result_t failed = {0};
      assert(provider_cli_adapter_host_execute(&cfg, &failed) < 0);
      assert(!failed.success && !terminal && !completed && aborted == 1 && closed == 1);
      assert(original.n_content == 1 && original.usage_in == 73 && original.usage_out == 29);
      assert(failed.canonical_response.n_content == 1);
      assert(!failed.response || !strstr(failed.response, "SDK REWRITE"));
      if (diagnostic == 12 || diagnostic == 15)
      {
         assert(original.stop_reason == AIMEE_STOP_CONTENT_FILTER && original.refusal &&
                !strcmp(original.refusal->category, "future_provider_category") &&
                !strcmp(original.refusal->explanation, "retained provider detail") &&
                !strcmp(original.raw_stop_reason, "refusal"));
      }
      if (diagnostic == 13)
         assert(original.stop_reason == AIMEE_STOP_MAX_TOKENS);
      if (diagnostic == 14)
         assert(original.stop_reason == AIMEE_STOP_UNKNOWN);
      if (diagnostic == 16)
         assert(original.stop_reason == AIMEE_STOP_ERROR && original.error_message &&
                !strcmp(original.error_message, "retained upstream diagnostic"));
      if (diagnostic == 17)
         assert(original.content[0].type == AIMEE_BLK_THINKING);
      if (diagnostic == 18)
         assert(original.stop_reason == AIMEE_STOP_END_TURN && !failed.success);
      aimee_response_free(&original);
      agent_result_clear_response(&failed);
      cfg.canonical_response = NULL;
   }
   /* General native/primary completion preserves all admitted visible media,
    * even with no text. Business TEXT-only projection is a different boundary. */
   for (int media = 19; media <= 20; media++)
   {
      reset(media);
      aimee_response_t original = {0};
      cfg.canonical_response = &original;
      agent_result_t rendered = {0};
      assert(!provider_cli_adapter_host_execute(&cfg, &rendered));
      assert(rendered.success && terminal && !completed && !aborted);
      assert(original.n_content == (media == 19 ? 4 : 1) && original.usage_in == 73);
      if (media == 19)
      {
         assert(original.content[1].type == AIMEE_BLK_IMAGE &&
                original.content[2].type == AIMEE_BLK_DOCUMENT);
         assert(original.content[1].raw && !strcmp(original.content[3].thinking_encrypted_content,
                                                   "opaque-encrypted-thought"));
         char *raw = cJSON_PrintUnformatted(original.content[1].raw);
         assert(raw && strstr(raw, "9007199254740993"));
         cJSON_free(raw);
      }
      else
         assert(original.content[0].type == AIMEE_BLK_IMAGE && rendered.response &&
                !rendered.response[0]);
      assert(aimee_ir_response_complete_text_valid(&original)); /* not a business artifact */
      aimee_response_free(&original);
      agent_result_clear_response(&rendered);
      cfg.canonical_response = NULL;
      finish(1);
      assert(completed == 1 && !aborted);
   }
   /* The actual factory must forward the adapter's admitted cap unchanged to
    * the runtime owner, including zero's existing unlimited/default meaning. */
   const int turn_limits[] = {0, 1, 3};
   for (size_t i = 0; i < sizeof turn_limits / sizeof turn_limits[0]; i++)
   {
      reset(0);
      cfg.native_turn_limit = expected_turn_limit = turn_limits[i];
      agent_result_t limited = {0};
      assert(!provider_cli_adapter_host_execute(&cfg, &limited));
      assert(limited.success && opened == 1);
      finish(1);
      agent_result_clear_response(&limited);
   }
   cfg.native_turn_limit = expected_turn_limit = 0;
   /* The source-bound no-tools selection must reach the actual native profile
    * and runtime tool owner, not just hide tools in a display/Go request. */
   reset(0);
   expected_no_tools = 1;
   cfg.no_tools = 1;
   agent_result_t tool_less = {0};
   assert(!provider_cli_adapter_host_execute(&cfg, &tool_less));
   assert(tool_less.success && opened == 1);
   finish(1);
   agent_result_clear_response(&tool_less);
   cfg.no_tools = 0;
   expected_no_tools = 0;
   /* DB40 acknowledgement can precede the marker at a crash: recover by
    * authenticated directory lookup, never by adopting a marker into DB40. */
   reset(0);
   seed_abandoned("actor");
   agent_result_t recovered_result = {0};
   assert(!provider_cli_adapter_host_execute(&cfg, &recovered_result));
   assert(recovered == 1 && quiesced == 1 && !reserved && !durable_reservation);
   assert(!strcmp(observed_epoch, "11111111111111111111111111111111"));
   finish(1);
   agent_result_clear_response(&recovered_result);
   /* Failed lookup and foreign authority refuse before HOME, SDK or kill. */
   reset(0);
   lookup_failure = 1;
   assert(provider_cli_adapter_host_execute(&cfg, &recovered_result) < 0);
   assert(!sdk_calls && !private_home[0] && !quiesced && !terminal);
   agent_result_clear_response(&recovered_result);
   reset(0);
   seed_abandoned("foreign-actor");
   assert(provider_cli_adapter_host_execute(&cfg, &recovered_result) < 0);
   assert(!sdk_calls && !private_home[0] && !quiesced && !terminal);
   agent_result_clear_response(&recovered_result);
   cJSON_Delete(durable_reservation);
   durable_reservation = NULL;
   reset(0);
   seed_abandoned("actor");
   cJSON_ReplaceItemInObjectCaseSensitive(
       durable_reservation, "containment_sha256",
       cJSON_CreateString("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));
   assert(provider_cli_adapter_host_execute(&cfg, &recovered_result) < 0);
   assert(!sdk_calls && !private_home[0] && !quiesced && !terminal);
   agent_result_clear_response(&recovered_result);
   cJSON_Delete(durable_reservation);
   durable_reservation = NULL;
   reset(0);
   seed_abandoned("actor");
   quiescence_failure = 1;
   assert(provider_cli_adapter_host_execute(&cfg, &recovered_result) < 0);
   assert(!sdk_calls && !private_home[0] && quiesced == 1 && !recovered && durable_reservation);
   agent_result_clear_response(&recovered_result);
   cJSON_Delete(durable_reservation);
   durable_reservation = NULL;
   /* A new Codex model/account binding can recover an old Claude directory;
    * discovery and exact43 custody are independent of the current registry. */
   reset(0);
   seed_abandoned("actor");
   cJSON_ReplaceItemInObjectCaseSensitive(durable_reservation, "harness",
                                          cJSON_CreateString("claude"));
   cJSON_ReplaceItemInObjectCaseSensitive(
       durable_reservation, "binding_sha256",
       cJSON_CreateString("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));
   assert(!provider_cli_adapter_host_execute(&cfg, &recovered_result));
   assert(recovered == 1 && !durable_reservation && opened == 1);
   finish(1);
   agent_result_clear_response(&recovered_result);
   /* Changed DB revision, live lease and current-turn ownership cannot be
    * converted into authority by the session discovery response. */
   char recovery_error[512];
   reset(0);
   seed_abandoned("actor");
   lookup_changed = 1;
   assert(server_native_session_reconcile("destination", "actor", "turn", recovery_error) < 0);
   assert(!quiesced && !recovered && durable_reservation);
   lookup_changed = 0;
   lookup_missing = 1;
   assert(server_native_session_reconcile("destination", "actor", "turn", recovery_error) < 0);
   assert(!quiesced && !recovered && durable_reservation);
   lookup_missing = 0;
   server_native_cache_lease_t *live = server_native_cache_lease_open(
       getenv("AIMEE_HOME"), "codex", "destination", digest, "actor", "old-turn", recovery_error);
   assert(live);
   assert(server_native_session_reconcile("destination", "actor", "turn", recovery_error) < 0);
   assert(!quiesced && !recovered);
   /* Browsing a live writer is observational: the exact actor/session/turn
    * remains registered until its retained terminal resources are released. */
   local_live_session = "destination";
   local_live_actor = "actor";
   local_live_turn = "old-turn";
   assert(!server_native_session_reconcile_browse("destination", "actor", recovery_error));
   assert(local_owner_queries == 1 && !quiesced && !recovered && durable_reservation);
   /* Strict prehistory retains its refusal even for a process-local writer. */
   assert(server_native_session_reconcile("destination", "actor", "turn", recovery_error) < 0);
   assert(local_owner_queries == 1 && !quiesced && !recovered);
   local_live_turn = "different-turn";
   assert(server_native_session_reconcile_browse("destination", "actor", recovery_error) < 0);
   assert(!quiesced && !recovered);
   local_live_turn = "old-turn";
   local_live_actor = "different-actor";
   assert(server_native_session_reconcile_browse("destination", "actor", recovery_error) < 0);
   local_live_actor = "actor";
   local_live_session = "different-session";
   assert(server_native_session_reconcile_browse("destination", "actor", recovery_error) < 0);
   local_live_session = "destination";
   /* Validate all records before any liveness query: a valid live first entry
    * cannot mask a malformed or foreign second entry. */
   cJSON *invalid_records = cJSON_CreateArray();
   cJSON_AddItemToArray(invalid_records, cJSON_Duplicate(durable_reservation, 1));
   cJSON_AddItemToArray(invalid_records, cJSON_CreateObject());
   char *invalid_wire = cJSON_PrintUnformatted(invalid_records);
   cJSON_Delete(invalid_records);
   active_override = invalid_wire;
   local_owner_queries = 0;
   assert(server_native_session_reconcile_browse("destination", "actor", recovery_error) < 0);
   assert(!local_owner_queries && !quiesced && !recovered);
   free(invalid_wire);
   active_override = NULL;
   assert(server_native_session_reconcile_browse("destination", "foreign-actor", recovery_error) <
          0);
   assert(server_native_session_reconcile_browse("foreign-session", "actor", recovery_error) < 0);
   assert(!local_owner_queries && !quiesced && !recovered);
   local_live_session = local_live_actor = local_live_turn = NULL;
   assert(!server_native_cache_lease_close(live, 1));
   /* With no process-local writer or live physical lease, browse follows the
    * unchanged source-bound orphan recovery path and starts no SDK/HOME. */
   assert(!server_native_session_reconcile_browse("destination", "actor", recovery_error));
   assert(recovered == 1 && quiesced == 1 && !durable_reservation && !sdk_calls &&
          !private_home[0]);
   reset(0);
   seed_abandoned("actor");
   cJSON_ReplaceItemInObjectCaseSensitive(durable_reservation, "turn_id",
                                          cJSON_CreateString("turn"));
   assert(server_native_session_reconcile("destination", "actor", "turn", recovery_error) < 0);
   assert(!quiesced && !recovered);
   cJSON_Delete(durable_reservation);
   durable_reservation = NULL;
   reset(0);
   assert(server_native_session_reconcile("destination", "foreign-actor", "turn", recovery_error) <
          0);
   assert(server_native_session_reconcile("foreign-session", "actor", "turn", recovery_error) < 0);
   char long_actor[VAULT_PRINCIPAL_MAX + 1];
   memset(long_actor, 'L', sizeof long_actor);
   long_actor[VAULT_PRINCIPAL_MAX - 1] = 0;
   assert(!server_native_session_reconcile_browse("destination", long_actor, recovery_error));
   /* Ordinary browsing adds no narrower actor precondition, while the existing
    * strict native profile's supported bound remains unchanged. */
   assert(server_native_session_reconcile("destination", long_actor, "turn", recovery_error) < 0);
   long_actor[VAULT_PRINCIPAL_MAX - 1] = 'L';
   long_actor[VAULT_PRINCIPAL_MAX] = 0;
   assert(server_native_session_reconcile_browse("destination", long_actor, recovery_error) < 0);
   assert(!local_owner_queries && !quiesced);
   active_override = "[{}]";
   assert(server_native_session_reconcile("destination", "actor", "turn", recovery_error) < 0 &&
          !quiesced);
   active_override = NULL;
   cJSON *overflow = cJSON_CreateArray();
   for (int i = 0; i < 33; i++)
      cJSON_AddItemToArray(overflow, cJSON_CreateObject());
   char *overflow_wire = cJSON_PrintUnformatted(overflow);
   cJSON_Delete(overflow);
   active_override = overflow_wire;
   assert(server_native_session_reconcile("destination", "actor", "turn", recovery_error) < 0 &&
          !quiesced);
   free(overflow_wire);
   active_override = NULL;
   /* A completed cache with only a missing custody receipt uses44 and retains
    * its epoch. DB1 PG tests qualify the actual terminal/token requirements. */
   reset(0);
   seed_abandoned("actor");
   old_completed = 1;
   cJSON_AddStringToObject(durable_reservation, "claim_id", "33333333333333333333333333333333");
   cJSON_AddStringToObject(durable_reservation, "expected_token", "complete:fixture:old-turn");
   assert(!server_native_session_reconcile("destination", "actor", "turn", recovery_error));
   assert(finished_reservation == 1 && !recovered && !strcmp(observed_epoch, "0"));
   old_completed = 0;
   /* The optional qualified path must reserve before HOME and finish only
    * after the real canonical terminal and exact boundary quiescence. */
   reset(0);
   qualified_mode = 1;
   setenv("AIMEE_NATIVE_CONTAINMENT_ROOT", "/fixture-owned-root", 1);
   agent_result_t qualified = {0};
   assert(!provider_cli_adapter_host_execute(&cfg, &qualified));
   assert(reserved == 1 && published_claim == 1 && !finished_reservation);
   finish(1);
   assert(quiesced >= 1 && finished_reservation == 1 && !durable_reservation);
   agent_result_clear_response(&qualified);
   reset(2);
   agent_result_t failed_child = {0};
   assert(provider_cli_adapter_host_execute(&cfg, &failed_child) < 0);
   assert(terminal && reserved && !finished_reservation && !closed);
   finish(0);
   assert(quiesced && aborted && finished_reservation && !durable_reservation);
   agent_result_clear_response(&failed_child);
   reset(0);
   quiescence_failure = 1;
   agent_result_t retained_boundary = {0};
   assert(provider_cli_adapter_host_execute(&cfg, &retained_boundary) < 0);
   assert(terminal && reserved && !runtime_closed);
   finish(0);
   assert(!runtime_closed && !completed && !aborted && !finished_reservation);
   atomic_store(&quiescence_failure, 0);
   for (int retry = 0; retry < 700 && !atomic_load(&disposed); retry++)
      usleep(10000);
   assert(atomic_load(&disposed) && runtime_closed == 1 && aborted == 1 &&
          finished_reservation == 1);
   agent_result_clear_response(&retained_boundary);
   /* A preclaim source whose terminal callback is dropped must use the exact
    * authoritative owner reconciliation, not retry finish forever. The DB1
    * contract fixture covers the actual cancellation/source-fence transaction. */
   reset(1);
   preclaim_drop = 1;
   rejected_terminal = 1;
   agent_result_t dropped_capture = {0};
   assert(provider_cli_adapter_host_execute(&cfg, &dropped_capture) < 0);
   for (int retry = 0; retry < 700 && !atomic_load(&disposed); retry++)
      usleep(10000);
   assert(atomic_load(&disposed) && recovered == 1 && !sdk_calls && !began && !terminal);
   assert(!private_home[0] && !completed && !aborted && !durable_reservation);
   agent_result_clear_response(&dropped_capture);
   /* An unsupported optional policy preserves the established direct path. */
   reset(0);
   qualified_mode = 0;
   assert(!provider_cli_adapter_host_execute(&cfg, &qualified));
   finish(1);
   assert(!reserved && !quiesced && !finished_reservation);
   agent_result_clear_response(&qualified);
   unsetenv("AIMEE_NATIVE_CONTAINMENT_ROOT");
   reset(0);
   snprintf(selected.provider, sizeof selected.provider, "codex-cli");
   snprintf(selected.auth_type, sizeof selected.auth_type, "codex-oauth");
   agent_result_t alias = {0};
   assert(!provider_cli_adapter_host_execute(&cfg, &alias));
   assert(!strcmp(selected.provider, "codex-cli"));
   agent_result_clear_response(&alias);
   finish(1);
   snprintf(selected.provider, sizeof selected.provider, "chatgpt");
   selected.auth_type[0] = 0;
   reset(0);
   snprintf(owner.vault_principal, sizeof owner.vault_principal, "wrong-actor");
   agent_result_t denied = {0};
   assert(provider_cli_adapter_host_execute(&cfg, &denied) < 0);
   assert(!opened && !retained && !sdk_calls && !restored);
   reset(11);
   aimee_block_t input = {.type = AIMEE_BLK_TEXT, .text = "seeded canonical task"};
   aimee_message_t message = {.role = "user", .blocks = &input, .n_blocks = 1};
   aimee_request_t seed = {.model = "selected-model", .messages = &message, .n_messages = 1};
   aimee_response_t canonical = {0};
   cfg.canonical_request = &seed;
   cfg.canonical_response = &canonical;
   agent_result_t seeded = {0};
   int seeded_rc = provider_cli_adapter_host_execute(&cfg, &seeded);
   assert(!seeded_rc && seeded.success && canonical.n_content == 2);
   assert(canonical.usage_in == 73 && canonical.usage_cache_read == 11);
   assert(!strcmp(canonical.content[0].text, "ORIGINAL canonical output"));
   assert(!strcmp(canonical.content[1].thinking_encrypted_content, "opaque-original"));
   assert(!strcmp(seeded.response, "ORIGINAL canonical output"));
   aimee_response_free(&canonical);
   agent_result_clear_response(&seeded);
   finish(1);
   cfg.canonical_request = NULL;
   cfg.canonical_response = NULL;
   reset(0);
   active_sdk = "claude";
   snprintf(selected.cli_kind, sizeof selected.cli_kind, "%s", "claude");
   agent_result_t claude = {0};
   assert(!provider_cli_adapter_host_execute(&cfg, &claude));
   assert(!strcmp(claude.response, "ORIGINAL canonical output") && !retained && joined);
   assert(access(private_home, F_OK));
   agent_result_clear_response(&claude);
   finish(1);
   assert(completed == 1 && closed == 1);
   /* The credential HOME remains per-launch; Claude project lookup must use
    * the same private scheduler cwd across successful continuation turns. */
   char first_claude_cwd[4096], first_claude_home[4096], first_sdk_identity[65];
   snprintf(first_sdk_identity, sizeof first_sdk_identity, "%s", observed_sdk_identity);
   snprintf(first_claude_cwd, sizeof first_claude_cwd, "%s", observed_claude_cwd);
   snprintf(first_claude_home, sizeof first_claude_home, "%s", private_home);
   struct stat first_cwd, next_cwd;
   assert(!stat(first_claude_cwd, &first_cwd));
   reset(0);
   agent_result_t continuation = {0};
   assert(!provider_cli_adapter_host_execute(&cfg, &continuation));
   assert(!strcmp(first_claude_cwd, observed_claude_cwd));
   assert(!strcmp(first_sdk_identity, observed_sdk_identity));
   assert(strcmp(first_claude_home, private_home) && access(first_claude_home, F_OK) &&
          access(private_home, F_OK));
   assert(!stat(observed_claude_cwd, &next_cwd) && first_cwd.st_dev == next_cwd.st_dev &&
          first_cwd.st_ino == next_cwd.st_ino);
   assert(strcmp(observed_claude_cwd, host_workspace));
   agent_result_clear_response(&continuation);
   finish(1);
   reset(0);
   qualified_mode = 1;
   setenv("AIMEE_NATIVE_CONTAINMENT_ROOT", "/fixture-owned-root", 1);
   agent_result_t qualified_claude = {0};
   assert(!provider_cli_adapter_host_execute(&cfg, &qualified_claude));
   assert(bound_credential_home == 1 && reserved);
   agent_result_clear_response(&qualified_claude);
   finish(1);
   assert(finished_reservation && !durable_reservation);
   unsetenv("AIMEE_NATIVE_CONTAINMENT_ROOT");
   qualified_mode = 0;
   /* Normal Models setup stores SDK aliases, while the host profile owns the
    * actual upstream wire and endpoint. Explicit cross-wire profiles above
    * remain unchanged; aliases must not become empty OpenAI profiles. */
   const char *claude_profiles[] = {"claude", "claude-code", "anthropic", "openai"};
   for (size_t profile = 0; profile < sizeof claude_profiles / sizeof *claude_profiles; profile++)
   {
      for (int custom = profile < 2 ? 0 : 1; custom < 2; custom++)
      {
         reset(0);
         expected_wire = profile == 3 ? AIMEE_WIRE_OPENAI_CHAT : AIMEE_WIRE_ANTHROPIC;
         expected_provider = profile < 2 ? "anthropic" : claude_profiles[profile];
         expected_auth = profile < 2 ? "claude-oauth" : profile == 2 ? "x-api-key" : "bearer";
         snprintf(selected.auth_type, sizeof selected.auth_type, "%s", expected_auth);
         expected_endpoint =
             custom ? "https://owned.example.invalid/v1" : "https://api.anthropic.com/v1";
         snprintf(selected.provider, sizeof selected.provider, "%s", claude_profiles[profile]);
         snprintf(selected.endpoint, sizeof selected.endpoint, "%s",
                  custom ? expected_endpoint : "");
         agent_result_t alias_result = {0};
         assert(!provider_cli_adapter_host_execute(&cfg, &alias_result));
         assert(alias_result.success && sdk_calls == 1 && joined);
         assert(!strcmp(selected.provider, claude_profiles[profile]));
         assert(!strcmp(selected.endpoint, custom ? expected_endpoint : ""));
         assert(!strcmp(selected.auth_type, expected_auth));
         agent_result_clear_response(&alias_result);
         finish(1);
         assert(completed == 1 && closed == 1);
      }
   }
   snprintf(selected.auth_type, sizeof selected.auth_type, "%s", "codex-oauth");
   snprintf(selected.provider, sizeof selected.provider, "%s", "chatgpt");
   selected.endpoint[0] = 0;
   active_sdk = "codex";
   reset(0);
   snprintf(selected.cli_kind, sizeof selected.cli_kind, "codex");
   qualified_mode = 1;
   setenv("AIMEE_NATIVE_CONTAINMENT_ROOT", "/fixture-owned-root", 1);
   agent_result_t uncertain_ack = {0};
   assert(!provider_cli_adapter_host_execute(&cfg, &uncertain_ack));
   finish_failure = 1;
   finish(1);
   assert(completed == 1 && !finished_reservation && durable_reservation);
   char retained_error[512];
   assert(!server_native_cache_lease_open(getenv("AIMEE_HOME"), active_sdk, "destination", digest,
                                          "actor", "after-uncertain-ack", retained_error));
   assert(strstr(retained_error, "unresolved") || strstr(retained_error, "busy"));
   agent_result_clear_response(&uncertain_ack);
   atomic_store(&finish_failure, 0);
   for (int retry = 0; retry < 700 && !atomic_load(&disposed); retry++)
      usleep(10000);
   assert(atomic_load(&disposed) && finished_reservation == 1 && !durable_reservation);
   assert(completed == 1 && !aborted); /* Retry never invalidates completed cache. */
   unsetenv("AIMEE_NATIVE_CONTAINMENT_ROOT");
   qualified_mode = 0;
   active_sdk = "codex";
   reset(0);
   snprintf(selected.cli_kind, sizeof selected.cli_kind, "%s", "unsupported");
   agent_result_t unsupported = {0};
   assert(provider_cli_adapter_host_execute(&cfg, &unsupported) == AGENT_RC_CONTEXT_REFUSED);
   assert(!unsupported.success && unsupported.error[0]);
   assert(!opened && !sdk_calls && !retained && !restored);
   agent_result_clear_response(&unsupported);
   cfg.agent = NULL;
   assert(provider_cli_adapter_host_execute(&cfg, &denied) == 1);
   /* Shutdown joins retries and parks late reserved terminal ownership without
    * issuing any new DB/cache effects or dropping its physical lease. */
   reset(0);
   active_sdk = "codex";
   qualified_mode = 1;
   cfg.agent = &selected;
   snprintf(selected.cli_kind, sizeof selected.cli_kind, "codex");
   setenv("AIMEE_NATIVE_CONTAINMENT_ROOT", "/fixture-owned-root", 1);
   agent_result_t late = {0};
   assert(!provider_cli_adapter_host_execute(&cfg, &late));
   assert(reserved == 1 && terminal && !completed && !closed);
   server_native_execution_shutdown();
   finish(1);
   assert(!completed && !aborted && !closed && !finished_reservation && durable_reservation);
   agent_result_clear_response(&late);
   unsetenv("AIMEE_NATIVE_CONTAINMENT_ROOT");

#else
   reset(0);
   agent_result_t refused = {0};
   assert(provider_cli_adapter_host_execute(&cfg, &refused) < 0);
   assert(!sdk_calls && !retained && restored == 1 && !terminal);
   agent_result_clear_response(&refused);
#endif
   server_native_execution_shutdown();
   run_cmd_set_cwd(NULL);
   cli_session_t cleanup = {.iso_home = strdup(temporary)};
   cli_session_destroy(&cleanup);
   assert(access(temporary, F_OK));
   puts("native factory canonical replay, original result, isolation and terminal ordering passed");
   return 0;
}
