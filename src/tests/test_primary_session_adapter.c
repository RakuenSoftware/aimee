/* Exercise the production primary owner across model switches and restart. */
#include "primary_session_adapter.h"
#include <aimee/ir/session_event.h>
#include "agent_exec.h"
#include "headers/context_engine.h"
#include "db1_client/primary_sessions.h"
#include "db1_client/server_sessions.h"
#include "cJSON.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char *stored, *canonical_stored;
static int fail_checkpoint, compact_enabled;
static int fail_recovery, recovered_before_history, recovery_reset;
static int standalone_active, standalone_unavailable;
static int host_history_calls, host_history_failure;
static int mock_native_history_load(const char *session, const char *actor, const char *active,
                                    int running, cJSON **messages, cJSON **receipts)
{
   assert(!strcmp(session, "stable") && !strcmp(actor, "alice") && !running);
   (void)active;
   host_history_calls++;
   *messages = cJSON_Parse("[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\","
                           "\"text\":\"authenticated host projection\"}]}]");
   *receipts = cJSON_CreateArray();
   return host_history_failure ? -1 : 0;
}
static const char *owner = "alice";
const char *agent_get_request_vault_principal(void)
{
   return "alice";
}
int db1_server_session_get(const char *id, db1_server_session_t *row)
{
   assert(!strcmp(id, "stable"));
   memset(row, 0, sizeof *row);
   snprintf(row->principal, sizeof row->principal, "%s", owner);
   return 0;
}
int db1_server_session_create(const char *id, const char *client, const char *principal)
{
   (void)id;
   (void)client;
   (void)principal;
   assert(0);
   return -1;
}
static int mock_native_reconcile(const char *session, const char *actor, const char *active,
                                 char error[512])
{
   assert(!strcmp(session, "stable") && !strcmp(actor, "alice"));
   (void)active;
   if (fail_recovery)
   {
      snprintf(error, 512, "owned native source unresolved");
      return -1;
   }
   if (recovery_reset)
      primary_session_adapter_reset(); /* Must not deadlock/free busy state. */
   recovered_before_history = 1;
   return 0;
}
int db1_session_native_active_owned(const char *actor, const char *session, char **out)
{
   assert(!strcmp(actor, "alice") && !strcmp(session, "stable"));
   *out = NULL;
   if (standalone_unavailable)
      return -1;
   *out = strdup(standalone_active ? "[{}]" : "[]");
   if (!standalone_active)
      recovered_before_history = 1;
   return 0;
}
int db1_session_ir_history_load(const char *id, const char *scope, const char *actor,
                                const char *active_turn, char **out)
{
   assert(!strcmp(id, "stable") && !strcmp(scope, "primary") && !strcmp(actor, "alice"));
   assert(recovered_before_history);
   recovered_before_history = 0;
   *out = canonical_stored ? strdup(canonical_stored) : NULL;
   return *out ? 0 : 1;
}
int db1_session_ir_history_save(const char *id, const char *checkpoint, const char *scope,
                                const char *actor, const char *json)
{
   assert(!strcmp(id, "stable") && strlen(checkpoint) == 32 && !strcmp(scope, "primary") &&
          !strcmp(actor, "alice"));
   if (fail_checkpoint)
      return -1;
   free(canonical_stored);
   canonical_stored = strdup(json);
   return 0;
}
static char stored_provider[64];
static int expected_count, executions;

int db1_primary_session_get_latest(const char *id, db1_primary_session_row_t *row)
{
   assert(!strcmp(id, "stable"));
   memset(row, 0, sizeof(*row));
   if (!stored)
      return 1;
   row->messages_json = strdup(stored);
   snprintf(row->provider, sizeof(row->provider), "%s", stored_provider);
   return 0;
}
void db1_primary_session_row_clear(db1_primary_session_row_t *row)
{
   free(row->messages_json);
}
char *db1_primary_session_load(const char *id, const char *agent, const char *provider)
{
   (void)id;
   (void)agent;
   (void)provider;
   return NULL;
}
const context_engine_t *context_engine_get_active(void)
{
   return NULL;
}
const context_engine_t *context_engine_get_default(void)
{
   return NULL;
}
int session_compact(cJSON *messages, const session_compact_config_t *cfg,
                    session_compact_result_t *result)
{
   (void)cfg;
   if (!compact_enabled)
      return -1;
   int before = cJSON_GetArraySize(messages);
   while (cJSON_GetArraySize(messages))
      cJSON_DeleteItemFromArray(messages, 0);
   cJSON_AddItemToArray(messages, cJSON_Parse("{\"role\":\"user\",\"blocks\":[{\"type\":\"text\","
                                              "\"text\":\"canonical compacted history\"}]}"));
   *result = (session_compact_result_t){.compacted = 1,
                                        .messages_before = before,
                                        .messages_after = 1,
                                        .messages_removed = before - 1};
   return 0;
}
int model_context_window(const char *model)
{
   (void)model;
   return 0;
}
void ingress_preinject_set_session_id(const char *id)
{
   (void)id;
}
void ingress_preinject_set_task_requirements(const cJSON *req)
{
   (void)req;
}
void session_id_set_override(const char *id)
{
   (void)id;
}
void session_id_clear_override(void)
{
}
int session_compact_estimate_tokens(cJSON *messages)
{
   (void)messages;
   return 0;
}
int session_compact_pressure(int a, int b, int c, const session_compact_config_t *cfg)
{
   (void)a;
   (void)b;
   (void)c;
   (void)cfg;
   return SESSION_PRESSURE_OK;
}
int agent_execute_session_with_tools(const agent_t *agent, const agent_network_t *network,
                                     const char *system, const char *prompt, int max_tokens,
                                     double temperature, cJSON *initial, cJSON **updated,
                                     agent_result_t *out)
{
   (void)agent;
   (void)network;
   (void)system;
   (void)prompt;
   (void)max_tokens;
   (void)temperature;
   (void)out;
   executions++;
   agent_t other = {0};
   strcpy(other.name, "mistral");
   strcpy(other.provider, "mistral");
   primary_session_request_t busy = {
       .agent = &other, .aimee_session_id = "stable", .user_prompt = "overlap"};
   agent_result_t overlap = {0};
   assert(primary_session_adapter_turn(&busy, &overlap, NULL, 0) != 0);

   assert(cJSON_GetArraySize(initial) == expected_count);
   cJSON *message;
   cJSON_ArrayForEach(message, initial)
   {
      assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(message, "blocks")));
      assert(!cJSON_GetObjectItemCaseSensitive(message, "content"));
   }
   *updated = initial ? cJSON_Duplicate(initial, 1) : cJSON_CreateArray();
   cJSON_AddItemToArray(
       *updated,
       cJSON_Parse("{\"role\":\"assistant\",\"blocks\":[{\"type\":\"text\",\"text\":\"reply\"}]}"));
   return 0;
}
static void turn(agent_t *agent, int count, int ok)
{
   expected_count = count;
   primary_session_request_t req = {
       .agent = agent, .aimee_session_id = "stable", .user_prompt = "continue"};
   agent_result_t out = {0};
   int before = executions;
   assert((primary_session_adapter_turn(&req, &out, NULL, 0) == 0) == ok);
   assert(executions == before + ok);
}
static void test_native_compaction_owns_canonical_session(void)
{
   primary_session_adapter_reset();
   free(canonical_stored);
   canonical_stored =
       strdup("[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"original task\"}]}]");
   agent_t native = {0};
   strcpy(native.name, "native-claude");
   strcpy(native.provider, "anthropic");
   strcpy(native.backend, AGENT_BACKEND_PROVIDER_CLI);
   strcpy(native.cli_kind, "claude");
   primary_session_request_t req = {
       .agent = &native, .aimee_session_id = "stable", .provider_session_id = "foreign-sdk-handle"};
   session_compact_result_t result;
   char id[128], error[256];
   int before = executions;
   compact_enabled = 1;
   int rc = primary_session_adapter_compact(&req, &result, id, sizeof id, error, sizeof error);
   assert(!rc && result.compacted && !strcmp(id, "stable"));
   assert(strstr(canonical_stored, "canonical compacted history") && executions == before);
   char *saved = strdup(canonical_stored);
   fail_checkpoint = 1;
   rc = primary_session_adapter_compact(&req, &result, id, sizeof id, error, sizeof error);
   assert(rc && !strcmp(saved, canonical_stored) && executions == before);
   fail_checkpoint = 0;
   owner = "mallory";
   rc = primary_session_adapter_compact(&req, &result, id, sizeof id, error, sizeof error);
   assert(rc && !strcmp(saved, canonical_stored) && executions == before);
   owner = "alice";
   compact_enabled = 0;
   free(saved);
   primary_session_adapter_reset();
}

static void test_authenticated_history_loader(void)
{
   primary_session_adapter_reset();
   free(canonical_stored);
   canonical_stored = strdup("invalid old projection must not be imported");
   primary_session_adapter_set_native_history_load(mock_native_history_load);
   agent_t agent = {0};
   strcpy(agent.name, "codex");
   strcpy(agent.provider, "chatgpt");
   turn(&agent, 1, 1);
   assert(host_history_calls == 1 && strstr(canonical_stored, "authenticated host projection"));
   host_history_failure = 1;
   turn(&agent, 0, 0); /* A failed host source read never falls back to old28. */
   assert(host_history_calls == 2);
   host_history_failure = 0;
   primary_session_adapter_set_native_history_load(NULL);
   primary_session_adapter_reset();
   free(canonical_stored);
   canonical_stored = NULL;
}
int main(void)
{
   test_authenticated_history_loader();
   test_native_compaction_owns_canonical_session();
   free(canonical_stored);
   canonical_stored = NULL;
   primary_session_adapter_set_native_reconcile(mock_native_reconcile);
   agent_t a = {0}, b = {0};
   strcpy(a.name, "codex");
   strcpy(a.provider, "chatgpt");
   strcpy(b.name, "mistral");
   strcpy(b.provider, "mistral");
   recovery_reset = 1;
   turn(&a, 0, 1);
   recovery_reset = 0;
   primary_session_adapter_set_native_reconcile(NULL);
   standalone_active = 1;
   turn(&a, 0, 0);
   standalone_active = 0;
   standalone_unavailable = 1;
   turn(&a, 0, 0);
   standalone_unavailable = 0;
   primary_session_adapter_set_native_reconcile(mock_native_reconcile);
   turn(&b, 1, 1);
   turn(&a, 2, 1); /* Returning to A must include B, not A's stale cache. */
   primary_session_adapter_reset();
   turn(&b, 3, 1); /* Restart reload is canonical even for a different provider. */
   fail_recovery = 1;
   turn(&a, 0, 0); /* Unresolved native ownership blocks an API model before history/provider. */
   fail_recovery = 0;
   free(canonical_stored);
   canonical_stored = strdup("[{\"role\":\"user\",\"blocks\":[{\"type\":\"bogus\"}]}]");
   turn(&a, 0, 0); /* A broken durable transcript cannot start an empty turn. */
   primary_session_adapter_reset();
   cJSON *unresolved =
       cJSON_Parse("[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":"
                   "\"pending-write\",\"name\":\"write_file\",\"input\":{}}]}]");
   assert(unresolved && aimee_ir_history_repair(unresolved) == 1);
   free(canonical_stored);
   canonical_stored = cJSON_PrintUnformatted(unresolved);
   cJSON_Delete(unresolved);
   char *before_unresolved = strdup(canonical_stored);
   turn(&a, 0, 0); /* A paired diagnostic is still not a recorded execution outcome. */
   assert(!strcmp(canonical_stored, before_unresolved));
   free(before_unresolved);
   primary_session_adapter_reset();
   free(stored);
   free(canonical_stored);
   canonical_stored = NULL;
   stored = strdup("[{\"role\":\"user\",\"content\":\"legacy\"}]");
   strcpy(stored_provider, "anthropic");
   turn(&a, 1, 1); /* Import at load uses the stored provider, not the selected one. */
   primary_session_adapter_reset();
   owner = "mallory";
   turn(&a, 0, 0);
   owner = "alice";
   fail_checkpoint = 1;
   primary_session_request_t req = {
       .agent = &a, .aimee_session_id = "stable", .user_prompt = "continue"};
   agent_result_t out = {0};
   expected_count = 2;
   int before = executions;
   assert(primary_session_adapter_turn(&req, &out, NULL, 0) == AGENT_RC_CAPTURE_FAILED);
   assert(executions == before + 1);
   fail_checkpoint = 0;
   turn(&a, 0, 0); /* A failed checkpoint cannot replay a turn after tools ran. */
   primary_session_adapter_reset();
   free(stored);
   free(canonical_stored);
   puts("primary canonical history: switch, switch back, reload, legacy import and refusal passed");
   return 0;
}
