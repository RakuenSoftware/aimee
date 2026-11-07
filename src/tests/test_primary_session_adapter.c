/* Exercise the production primary owner across model switches and restart. */
#include "primary_session_adapter.h"
#include "agent_runtime_messages.h"
#include "headers/server_session_capture.h"
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
#include <pthread.h>

static char *stored, *canonical_stored;
static cJSON *model_override, *capture_history;
static int capture_present, capture_bad, capture_refs, save32, save55, save57;
static int preserving_failure, cancel_checkpoint;
static const char *capture_session = "stable", *capture_turn = "current-turn";
static int capture_object;
int server_session_capture_retain_current(const char *session, const char *actor,
                                          server_session_capture_t **out)
{
   *out = NULL;
   if (!capture_present)
      return 0;
   if (capture_bad || strcmp(session, "stable") || strcmp(actor, "alice"))
      return -1;
   *out = (server_session_capture_t *)&capture_object;
   ++capture_refs;
   return 0;
}
const char *server_session_capture_session(const server_session_capture_t *owner)
{
   assert(owner == (void *)&capture_object);
   return capture_session;
}
const char *server_session_capture_turn(const server_session_capture_t *owner)
{
   assert(owner == (void *)&capture_object);
   return capture_turn;
}
void server_session_capture_release(server_session_capture_t *owner)
{
   if (owner)
   {
      assert(owner == (void *)&capture_object);
      assert(capture_refs == 1);
      --capture_refs;
   }
}
int server_session_capture_history(server_session_capture_t *owner, cJSON **out)
{
   assert(owner == (void *)&capture_object);
   *out = cJSON_Duplicate(capture_history, 1);
   return *out ? 0 : -1;
}

static int fail_checkpoint, compact_enabled, compact_keep;
static int fail_recovery, recovered_before_history, recovery_reset;
static int standalone_active, standalone_unavailable;
static int host_history_calls, host_history_failure;
static int browse_open, browse_store_failure;
static int browse_recovery_calls, browse_recovery_failure, browse_recovery_reset;
static int legacy_reads, require_browse_recovery;
static int mock_native_browse_reconcile(const char *session, const char *actor, char error[512])
{
   assert(!strcmp(session, "stable") && !strcmp(actor, "alice"));
   browse_recovery_calls++;
   if (browse_recovery_reset)
   {
      primary_session_adapter_set_native_browse_reconcile(NULL);
      primary_session_adapter_reset(); /* Callback runs outside the adapter lock. */
   }
   if (browse_recovery_failure)
   {
      snprintf(error, 512, "abandoned native custody unresolved");
      return -1;
   }
   return 0;
}
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
   if (!browse_open)
   {
      assert(recovered_before_history);
      recovered_before_history = 0;
   }
   if (browse_store_failure)
      return -1;
   *out = canonical_stored ? strdup(canonical_stored) : NULL;
   return *out ? 0 : 1;
}
int db1_session_ir_history_save(const char *id, const char *checkpoint, const char *scope,
                                const char *actor, const char *json)
{
   assert(!strcmp(id, "stable") && strlen(checkpoint) == 32 && !strcmp(scope, "primary") &&
          !strcmp(actor, "alice"));
   ++save32;
   /* Actual ordinary opcode32 rejects both families before any append. */
   if (fail_checkpoint || strstr(json, "\"input_attachment\"") || strstr(json, "\"tool_source\""))
      return -1;
   free(canonical_stored);
   canonical_stored = strdup(json);
   return 0;
}
static int preserving_save(const char *id, const char *checkpoint, const char *scope,
                           const char *actor, const char *active, const char *json)
{
   assert(!strcmp(id, "stable") && strlen(checkpoint) == 32 && !strcmp(scope, "primary") &&
          !strcmp(actor, "alice") && !strcmp(active, "current-turn") && capture_refs == 1);
   if (cancel_checkpoint)
   {
      int prior;
      assert(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &prior) == 0);
      assert(prior == PTHREAD_CANCEL_DISABLE);
      assert(pthread_cancel(pthread_self()) == 0);
   }
   if (preserving_failure)
      return -1;
   free(canonical_stored);
   canonical_stored = strdup(json);
   return 0;
}
int db1_session_tool_source_history_save(const char *id, const char *checkpoint, const char *scope,
                                         const char *actor, const char *active, const char *json)
{
   ++save55;
   return preserving_save(id, checkpoint, scope, actor, active, json);
}
int db1_session_input_attachment_history_save(const char *id, const char *checkpoint,
                                              const char *scope, const char *actor,
                                              const char *active, const char *json)
{
   ++save57;
   return preserving_save(id, checkpoint, scope, actor, active, json);
}
static int import_reports, import_failure;
int db1_session_ir_history_import(const char *id, const char *scope, const char *actor,
                                  const char *source, const char *agent, const char *digest,
                                  const char *messages, const char *report)
{
   assert(!strcmp(id, "stable") && !strcmp(source, "stable") && !strcmp(scope, "primary") &&
          !strcmp(actor, "alice") && !strcmp(agent, "legacy-agent") && strlen(digest) == 64);
   cJSON *metadata = cJSON_Parse(report);
   assert(metadata);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(metadata, "source_sha256")), digest));
   import_reports++;
   const char *outcome = cJSON_GetStringValue(cJSON_GetObjectItem(metadata, "outcome"));
   if (!import_failure && strcmp(outcome, "archived") && strcmp(outcome, "refused"))
   {
      free(canonical_stored);
      canonical_stored = strdup(messages);
   }
   cJSON_Delete(metadata);
   return import_failure ? -1 : 0;
}
static int browse_skipped;
int db1_session_ir_history_import_browse(const char *id, const char *actor, const char *agent,
                                         const char *digest, const char *messages,
                                         const char *report)
{
   if (browse_store_failure)
      return -1;
   if (browse_skipped)
      return 1;
   return db1_session_ir_history_import(id, "primary", actor, id, agent, digest, messages, report);
}
static char stored_provider[64];
static int expected_count, executions;

int db1_primary_session_get_latest(const char *id, db1_primary_session_row_t *row)
{
   assert(!strcmp(id, "stable"));
   legacy_reads++;
   if (require_browse_recovery)
      assert(browse_recovery_calls == require_browse_recovery);
   memset(row, 0, sizeof(*row));
   if (!stored)
      return 1;
   row->messages_json = strdup(stored);
   snprintf(row->agent_name, sizeof(row->agent_name), "%s", "legacy-agent");
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
   if (compact_keep)
   {
      *result = (session_compact_result_t){.messages_before = before, .messages_after = before};
      return 0;
   }
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
   *updated = model_override ? cJSON_Duplicate(model_override, 1)
              : initial      ? cJSON_Duplicate(initial, 1)
                             : cJSON_CreateArray();
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
static void test_large_legacy_import_precedes_compaction(void)
{
   primary_session_adapter_reset();
   free(canonical_stored);
   canonical_stored = NULL;
   free(stored);
   cJSON *many = cJSON_CreateArray();
   for (int i = 0; i < 4097; i++)
      assert(cJSON_AddItemToArray(
          many,
          cJSON_Parse("{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"legacy\"}]}")));
   stored = cJSON_PrintUnformatted(many);
   cJSON_Delete(many);
   strcpy(stored_provider, "anthropic");
   agent_t native = {0};
   strcpy(native.name, "native-claude");
   strcpy(native.provider, "anthropic");
   strcpy(native.backend, AGENT_BACKEND_PROVIDER_CLI);
   strcpy(native.cli_kind, "claude");
   primary_session_request_t req = {.agent = &native, .aimee_session_id = "stable"};
   session_compact_result_t result;
   char id[128], error[256];
   compact_enabled = 1;
   int before = executions;
   assert(primary_session_adapter_compact(&req, &result, id, sizeof id, error, sizeof error) == 0);
   assert(result.messages_before == 4097 && result.compacted && executions == before);
   assert(strstr(canonical_stored, "canonical compacted history"));
   primary_session_adapter_reset();
   free(canonical_stored);
   canonical_stored = NULL;
   free(stored);
   cJSON *paired = cJSON_Parse(
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":\"large-result\","
       "\"name\":\"read\",\"input\":{}}]},{\"role\":\"user\",\"blocks\":[{\"type\":\"tool_result\","
       "\"tool_id\":\"large-result\",\"content\":\"\",\"is_error\":false}]}]");
   assert(paired);
   char *large = malloc(1200001);
   assert(large);
   memset(large, 'x', 1200000);
   large[1200000] = 0;
   cJSON *output =
       cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(paired, 1), "blocks"), 0);
   assert(cJSON_SetValuestring(cJSON_GetObjectItem(output, "content"), large));
   free(large);
   stored = cJSON_PrintUnformatted(paired);
   cJSON_Delete(paired);
   assert(primary_session_adapter_compact(&req, &result, id, sizeof id, error, sizeof error) == 0);
   assert(result.messages_before == 2 && result.compacted && executions == before);
   assert(strstr(canonical_stored, "canonical compacted history"));
   compact_enabled = 0;
   primary_session_adapter_reset();
   free(stored);
   stored = NULL;
}

static void test_browser_native_recovery(void)
{
   int reads = legacy_reads, reports = import_reports, before = executions;
   primary_session_adapter_set_native_browse_reconcile(NULL);
   standalone_active = 1;
   assert(primary_session_history_open("stable", "alice") == -1);
   standalone_active = 0;
   standalone_unavailable = 1;
   assert(primary_session_history_open("stable", "alice") == -1);
   standalone_unavailable = 0;
   assert(legacy_reads == reads && import_reports == reports);
   primary_session_adapter_set_native_browse_reconcile(mock_native_browse_reconcile);
   assert(primary_session_history_open("stable", "mallory") == -1);
   owner = "mallory";
   assert(primary_session_history_open("stable", "alice") == -1);
   owner = "alice";
   assert(!browse_recovery_calls && legacy_reads == reads);
   browse_recovery_failure = 1;
   assert(primary_session_history_open("stable", "alice") == -1);
   assert(browse_recovery_calls == 1 && legacy_reads == reads && import_reports == reports);
   browse_recovery_failure = 0;
   stored = strdup("[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"Recovered "
                   "browsing input\"}]}]");
   strcpy(stored_provider, "anthropic");
   require_browse_recovery = 2;
   browse_recovery_reset = 1;
   assert(primary_session_history_open("stable", "alice") == 0);
   assert(browse_recovery_calls == 2 && legacy_reads == reads + 1);
   assert(import_reports == reports + 1 && canonical_stored &&
          strstr(canonical_stored, "Recovered browsing input"));
   browse_recovery_reset = 0;
   primary_session_adapter_set_native_browse_reconcile(mock_native_browse_reconcile);
   /* A healthy live-owner success must continue to import/replay normally. */
   require_browse_recovery = 3;
   assert(primary_session_history_open("stable", "alice") == 0);
   assert(browse_recovery_calls == 3 && legacy_reads == reads + 2);
   require_browse_recovery = 0;
   primary_session_adapter_set_native_browse_reconcile(NULL);
   assert(executions == before && import_reports == reports + 2);
   free(stored);
   stored = NULL;
   free(canonical_stored);
   canonical_stored = NULL;
}

static void test_browser_history_open(void)
{
   browse_open = 1;
   int before = executions;
   int reports = import_reports;
   free(stored);
   stored = strdup(
       "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"Old browsing input\"}]}]");
   free(canonical_stored);
   canonical_stored = NULL;
   strcpy(stored_provider, "anthropic");
   assert(primary_session_history_open("stable", "mallory") == -1);
   owner = "mallory";
   assert(primary_session_history_open("stable", "alice") == -1);
   owner = "alice";
   browse_store_failure = 1;
   assert(primary_session_history_open("stable", "alice") == -1);
   browse_store_failure = 0;
   import_failure = 1;
   assert(primary_session_history_open("stable", "alice") == -1);
   assert(!canonical_stored);
   import_failure = 0;
   assert(primary_session_history_open("stable", "alice") == 0);
   assert(canonical_stored && strstr(canonical_stored, "Old browsing input"));
   int acknowledged = import_reports;
   browse_skipped = 1;
   assert(primary_session_history_open("stable", "alice") == 0 && import_reports == acknowledged);
   browse_skipped = 0;
   free(canonical_stored);
   canonical_stored = NULL;
   free(stored);
   stored = strdup("Legacy terminal transcript must be browseable before sending");
   strcpy(stored_provider, "tmux");
   assert(primary_session_history_open("stable", "alice") == 0 && !canonical_stored);
   assert(import_reports > reports && executions == before);
   free(stored);
   stored = NULL;
   assert(primary_session_history_open("stable", "alice") == 0);
   browse_open = 0;
}

static cJSON *source_message(const char *text, int annotated)
{
   cJSON *message = cJSON_CreateObject(), *blocks = cJSON_CreateArray(),
         *block = cJSON_CreateObject();
   cJSON_AddStringToObject(message, "role", "user");
   cJSON_AddItemToObject(message, "blocks", blocks);
   cJSON_AddItemToArray(blocks, block);
   cJSON_AddStringToObject(block, "type", "text");
   cJSON_AddStringToObject(block, "text", text);
   if (annotated)
      cJSON_AddItemToObject(
          block, "input_attachment",
          cJSON_Parse("{\"schema_version\":1,\"kind\":\"host.original-input.v1\","
                      "\"operation_uuid\":\"00000000-0000-4000-8000-000000000001\","
                      "\"text_sha256\":"
                      "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                      "\"opaque\":\"{ \\\"wide\\\":18446744073709551615 }\"}"));
   return message;
}
static cJSON *tool_history(size_t receipt_size)
{
   cJSON *messages = cJSON_Parse(
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\","
       "\"tool_id\":\"call-1\",\"name\":\"fixture\",\"input\":{}}]},"
       "{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\",\"tool_id\":\"call-1\","
       "\"is_error\":false,\"content\":\"wide result 18446744073709551615\"}]}]");
   assert(messages);
   cJSON *block =
       cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 1), "blocks"), 0);
   cJSON *source = cJSON_Parse(
       "{\"schema_version\":1,\"owner\":\"00000000-0000-4000-8000-000000000002\","
       "\"visible_sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}");
   char *receipt = malloc(receipt_size + 32);
   assert(receipt);
   strcpy(receipt, "{\"opaque\":\"");
   memset(receipt + strlen(receipt), 'x', receipt_size);
   strcpy(receipt + 11 + receipt_size, "\"}");
   cJSON_AddStringToObject(source, "receipt", receipt);
   free(receipt);
   cJSON_AddItemToObject(block, "tool_source", source);
   return messages;
}
static int preserving_turn(agent_t *agent)
{
   cJSON *initial = cJSON_Parse(canonical_stored);
   expected_count = initial ? cJSON_GetArraySize(initial) : 0;
   cJSON_Delete(initial);
   primary_session_request_t req = {.agent = agent,
                                    .aimee_session_id = "stable",
                                    .user_prompt = "continue",
                                    .active_turn_id = "current-turn"};
   agent_result_t out = {0};
   int rc = primary_session_adapter_turn(&req, &out, NULL, 0);
   assert(capture_refs == 0);
   return rc;
}
static void preserving_reset(void)
{
   primary_session_adapter_reset();
   free(canonical_stored);
   canonical_stored = strdup("[]");
   cJSON_Delete(capture_history);
   capture_history = cJSON_CreateArray();
   cJSON_Delete(model_override);
   model_override = cJSON_CreateArray();
   capture_present = 1;
   capture_bad = 0;
   capture_session = "stable";
   capture_turn = "current-turn";
   preserving_failure = 0;
   save32 = save55 = save57 = 0;
}
static void *cancel_preserving_turn(void *opaque)
{
   (void)preserving_turn(opaque);
   pthread_testcancel();
   assert(0);
   return NULL;
}
static void test_preserving_checkpoint(void)
{
   agent_t agent = {0};
   strcpy(agent.name, "codex");
   strcpy(agent.provider, "chatgpt");
   primary_session_adapter_set_native_reconcile(mock_native_reconcile);
   preserving_reset();
   cJSON_Delete(model_override);
   model_override = tool_history(70000);
   assert(preserving_turn(&agent) == 0 && save55 == 1 && !save32 && !save57);
   assert(strlen(canonical_stored) > 65536);
   cJSON *saved = cJSON_Parse(canonical_stored);
   cJSON *left = cJSON_GetObjectItem(
       cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(model_override, 1), "blocks"), 0),
       "tool_source");
   cJSON *right = cJSON_GetObjectItem(
       cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(saved, 1), "blocks"), 0),
       "tool_source");
   assert(cJSON_Compare(left, right, 1));
   cJSON_Delete(saved);

   preserving_reset();
   cJSON_AddItemToArray(capture_history, source_message("first", 1));
   cJSON_AddItemToArray(model_override, source_message("first", 0));
   assert(preserving_turn(&agent) == 0 && save57 == 1 && !save32 && !save55);
   assert(strstr(canonical_stored, "input_attachment"));
   /* Exact same advisory bytes: a real ordinary User remains a User and must
    * not acquire an inferred attachment or disappear by copied provenance. */
   for (int evidence = 0; evidence < 2; evidence++)
   {
      preserving_reset();
      cJSON_AddItemToArray(capture_history, source_message("first", 1));
      cJSON_AddItemToArray(model_override, source_message("first", 0));
      cJSON *hint = evidence ? agent_session_tool_evidence_message("same advisory text")
                             : agent_session_control_message("same advisory text");
      assert(hint && cJSON_AddItemToArray(model_override, hint));
      assert(preserving_turn(&agent) == 0 && save57 == 1 && !save32 && !save55);
      preserving_reset();
      cJSON_AddItemToArray(capture_history, source_message("first", 1));
      cJSON_AddItemToArray(model_override, source_message("first", 0));
      hint = evidence ? agent_session_tool_evidence_message("same advisory text")
                      : agent_session_control_message("same advisory text");
      assert(hint &&
             cJSON_ReplaceItemInObjectCaseSensitive(hint, "role", cJSON_CreateString("user")) &&
             cJSON_AddItemToArray(model_override, hint));
      assert(preserving_turn(&agent) != 0 && !save57 && !save32 && !save55);
   }
   preserving_reset();
   cJSON_AddItemToArray(capture_history, source_message("first", 1));
   cJSON_AddItemToArray(model_override, source_message("first", 0));
   assert(preserving_turn(&agent) == 0 && save57 == 1);
   /* Restart and second active turn preserve the first exact pair and add a new one. */
   primary_session_adapter_reset();
   cJSON_Delete(capture_history);
   capture_history = cJSON_Parse(canonical_stored);
   cJSON_AddItemToArray(capture_history, source_message("second", 1));
   cJSON_Delete(model_override);
   model_override = cJSON_Parse(canonical_stored);
   cJSON_AddItemToArray(model_override, source_message("second", 0));
   assert(preserving_turn(&agent) == 0 && save57 == 2 && !save32);

   /* Explicitly retained identical pairs are not guessed and remain legitimate. */
   preserving_reset();
   cJSON_AddItemToArray(capture_history, source_message("same", 1));
   cJSON_AddItemToArray(capture_history, source_message("same", 1));
   cJSON_Delete(model_override);
   model_override = cJSON_Duplicate(capture_history, 1);
   assert(preserving_turn(&agent) == 0 && save57 == 1);

   for (int fault = 0; fault < 13; fault++)
   {
      preserving_reset();
      cJSON_AddItemToArray(capture_history, source_message("first", 1));
      cJSON_AddItemToArray(model_override, source_message("first", 0));
      if (fault == 0)
         capture_present = 0;
      if (fault == 1)
         capture_bad = 1;
      if (fault == 2)
         capture_session = "foreign";
      if (fault == 3)
         capture_turn = "old-turn";
      if (fault == 4)
      {
         cJSON_Delete(model_override);
         model_override = cJSON_CreateArray();
      }
      if (fault == 5)
         cJSON_ReplaceItemInArray(model_override, 0, source_message("changed", 0));
      if (fault == 6)
      {
         cJSON_AddItemToArray(capture_history, source_message("first", 0));
         cJSON_AddItemToArray(model_override, source_message("first", 0));
      }
      if (fault == 7)
         cJSON_ReplaceItemInArray(model_override, 0, source_message("first", 1)),
             cJSON_SetValuestring(
                 cJSON_GetObjectItem(
                     cJSON_GetObjectItem(
                         cJSON_GetArrayItem(
                             cJSON_GetObjectItem(cJSON_GetArrayItem(model_override, 0), "blocks"),
                             0),
                         "input_attachment"),
                     "opaque"),
                 "{\"foreign\":true}");
      if (fault == 8)
      {
         cJSON_AddItemToArray(capture_history, source_message("second", 0));
         cJSON_InsertItemInArray(model_override, 0, source_message("second", 0));
      }
      if (fault == 9)
         preserving_failure = 1;
      if (fault == 10) /* Same text moved to another User block is not the same input. */
         cJSON_InsertItemInArray(
             cJSON_GetObjectItem(cJSON_GetArrayItem(model_override, 0), "blocks"), 0,
             cJSON_Parse("{\"type\":\"text\",\"text\":\"extra\"}"));
      if (fault == 11) /* No current canonical input can authorize a supplied attachment. */
      {
         cJSON_ReplaceItemInArray(capture_history, 0, source_message("first", 0));
         cJSON_ReplaceItemInArray(model_override, 0, source_message("first", 1));
      }
      if (fault == 12) /* Matching words moved across User message boundaries refuse. */
      {
         cJSON_AddItemToArray(capture_history, source_message("second", 0));
         cJSON *extra = source_message("second", 0);
         cJSON *block = cJSON_DetachItemFromArray(cJSON_GetObjectItem(extra, "blocks"), 0);
         cJSON_AddItemToArray(cJSON_GetObjectItem(cJSON_GetArrayItem(model_override, 0), "blocks"),
                              block);
         cJSON_Delete(extra);
      }
      assert(preserving_turn(&agent) != 0 && !save32 && !save55);
      assert(save57 == (fault == 9));
   }
   preserving_reset();
   cJSON_AddItemToArray(capture_history, source_message("both", 1));
   cJSON_Delete(model_override);
   model_override = tool_history(80000);
   cJSON_InsertItemInArray(model_override, 0, source_message("both", 0));
   assert(preserving_turn(&agent) == 0 && save57 == 1 && !save55 && !save32);
   assert(strstr(canonical_stored, "tool_source") && strstr(canonical_stored, "input_attachment"));
   preserving_reset();
   cJSON_AddItemToArray(capture_history, source_message("compact source", 1));
   free(canonical_stored);
   canonical_stored = cJSON_PrintUnformatted(capture_history);
   primary_session_request_t compact_req = {.agent = &agent, .aimee_session_id = "stable"};
   session_compact_result_t compact_result;
   char compact_error[256];
   compact_enabled = compact_keep = 1;
   assert(primary_session_adapter_compact(&compact_req, &compact_result, NULL, 0, compact_error,
                                          sizeof compact_error) != 0 &&
          !save32 && !save57);
   compact_req.active_turn_id = "current-turn";
   compact_keep = 0; /* A live owner does not authorize dropping/changing User TEXT. */
   assert(primary_session_adapter_compact(&compact_req, &compact_result, NULL, 0, compact_error,
                                          sizeof compact_error) != 0 &&
          !save32 && !save57);
   compact_keep = 1;
   assert(primary_session_adapter_compact(&compact_req, &compact_result, NULL, 0, compact_error,
                                          sizeof compact_error) == 0 &&
          save57 == 1 && !save32);
   compact_enabled = compact_keep = 0;
   preserving_reset();
   cJSON_AddItemToArray(capture_history, source_message("cancel", 1));
   cJSON_AddItemToArray(model_override, source_message("cancel", 0));
   pthread_t thread;
   void *outcome = NULL;
   cancel_checkpoint = 1;
   assert(pthread_create(&thread, NULL, cancel_preserving_turn, &agent) == 0);
   assert(pthread_join(thread, &outcome) == 0 && outcome == PTHREAD_CANCELED);
   assert(capture_refs == 0 && save57 == 1);
   cancel_checkpoint = 0;
   /* Same owner can checkpoint again: cancellation left no busy lock/ref. */
   assert(preserving_turn(&agent) == 0 && save57 == 2);
   cJSON_Delete(model_override);
   model_override = NULL;
   cJSON_Delete(capture_history);
   capture_history = NULL;
   capture_present = 0;
   primary_session_adapter_reset();
   free(canonical_stored);
   canonical_stored = NULL;
   puts("preserving checkpoint: tool-only, input, both, >64KiB, second turn 13 refusals and "
        "cancellation PASS");
}

int main(void)
{
   test_preserving_checkpoint();
   test_browser_native_recovery();
   test_browser_history_open();
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
   import_failure = 1;
   int before_import_executions = executions;
   turn(&a, 0, 0); /* No effects/model call before durable migration acknowledgement. */
   assert(executions == before_import_executions && !canonical_stored);
   primary_session_adapter_reset();
   import_failure = 0;
   turn(&a, 1, 1); /* Import uses stored source format, not selected model. */
   assert(import_reports >= 2);
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
   test_large_legacy_import_precedes_compaction();
   free(stored);
   free(canonical_stored);
   puts("primary canonical history: switch, switch back, reload, legacy import and refusal passed");
   return 0;
}
