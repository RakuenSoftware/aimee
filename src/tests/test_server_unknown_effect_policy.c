#include "server_unknown_effect_policy.h"
#include "server_session_capture.h"
#include "server_session_control.h"
#include "server_native_history.h"
#include "request_context.h"
#include "aimee_sha256.h"
#include <aimee/tools/module_api.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static request_context_t owner;
static int unknown, unavailable, prompts, responses, loads, emit_failure, fail_owner;
static char last_request_id[256];
static int cancelled, capture_failed, source_count = 1, large_old_input;
static char expected_receipt_hash[65];
static const char *decision = "allow";
const request_context_t *request_context_get(void)
{
   return &owner;
}
server_session_capture_t *server_session_capture_current(void)
{
   return (void *)1;
}
const char *server_session_capture_session(const server_session_capture_t *v)
{
   return v ? "session" : NULL;
}
const char *server_session_capture_turn(const server_session_capture_t *v)
{
   return v ? "turn" : NULL;
}
const char *session_id(void)
{
   return "session";
}
const char *agent_get_request_vault_principal(void)
{
   return owner.vault_principal;
}
int agent_request_cancelled(void)
{
   return cancelled;
}
int server_session_capture_accepting(const server_session_capture_t *capture)
{
   return capture && !capture_failed;
}
int server_native_history_load(const char *session, const char *actor, const char *turn,
                               int running, cJSON **messages, cJSON **receipts)
{
   (void)session;
   (void)actor;
   (void)turn;
   (void)running;
   loads++;
   *messages = NULL;
   *receipts = NULL;
   if (unavailable)
      return -1;
   *messages = cJSON_CreateArray();
   *receipts = cJSON_CreateArray();
   if (unknown)
   {
      cJSON *base = cJSON_Parse(
          "{\"event_id\":\"receipt-event\",\"receipt\":{\"source\":{\"session_id\":\"session\","
          "\"turn_id\":\"old-turn\",\"sequence\":\"2\",\"tool_id\":\"old-call\",\"name\":\"write_"
          "file\"}},\"call\":{\"input\":{\"old_secret\":\"not-display-data\"}}}");
      if (large_old_input)
      {
         char value[4097];
         memset(value, 'x', sizeof value - 1);
         value[sizeof value - 1] = 0;
         cJSON *args = cJSON_GetObjectItemCaseSensitive(
             cJSON_GetObjectItemCaseSensitive(base, "call"), "input");
         cJSON_ReplaceItemInObjectCaseSensitive(args, "old_secret", cJSON_CreateString(value));
      }
      for (int i = 0; i < source_count; i++)
      {
         cJSON *entry = cJSON_Duplicate(base, 1);
         char id[64];
         snprintf(id, sizeof id, "receipt-event:%d", i);
         cJSON_ReplaceItemInObjectCaseSensitive(entry, "event_id", cJSON_CreateString(id));
         cJSON_AddItemToArray(*receipts, entry);
      }
      cJSON_Delete(base);
   }
   cJSON *canonical = aimee_ir_json_canonical_copy(*receipts);
   char *wire = cJSON_PrintUnformatted(canonical);
   assert(wire && !aimee_sha256_hex(wire, strlen(wire), expected_receipt_hash));
   free(wire);
   cJSON_Delete(canonical);
   return 0;
}
int server_native_history_requires_permission(const cJSON *receipts)
{
   return cJSON_GetArraySize(receipts) > 0;
}
int server_native_history_admit_source(const char *session, const char *actor, const char *turn,
                                       int running, const cJSON *messages)
{
   (void)session;
   (void)actor;
   (void)turn;
   (void)running;
   (void)messages;
   return unavailable ? -1 : 0;
}
static int sink(const aimee_session_event_t *event, void *context)
{
   server_control_scope_t *scope = context;
   if (event->kind == AIMEE_SESSION_CONTROL_RESPONSE)
   {
      responses++;
      return 0;
   }
   assert(event->kind == AIMEE_SESSION_CONTROL_REQUEST);
   snprintf(last_request_id, sizeof last_request_id, "turn:%s", event->request_id);
   if (emit_failure)
      return -1;
   assert(!server_control_register(scope, event->request_id, "permission", event->control));
   if (fail_owner)
   {
      server_control_scope_fail(scope);
      return 0;
   }
   const cJSON *input = cJSON_GetObjectItemCaseSensitive(event->control, "input"),
               *summary = cJSON_GetObjectItemCaseSensitive(input, "unknown_receipts");
   assert(cJSON_IsObject(summary) &&
          cJSON_GetObjectItemCaseSensitive(summary, "count")->valueint == source_count);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(summary, "sha256")),
                  expected_receipt_hash));
   assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(summary, "sources")) <= 4);
   char *display = cJSON_PrintUnformatted(event->control);
   assert(display && strlen(display) < 16384 && !strstr(display, "old_secret"));
   free(display);
   prompts++;
   char qualified[256];
   snprintf(qualified, sizeof qualified, "turn:%s", event->request_id);
   cJSON *reply = cJSON_CreateObject();
   cJSON_AddStringToObject(reply, "decision", decision);
   assert(!server_control_reply("session", qualified, "actor", reply));
   cJSON_Delete(reply);
   return 0;
}
int main(void)
{
   owner.model_execution = 1;
   snprintf(owner.request_id, sizeof owner.request_id, "host-request");
   snprintf(owner.vault_principal, sizeof owner.vault_principal, "actor");
   server_control_scope_t *scope = server_control_scope_open("session", "turn", "actor");
   assert(scope);
   server_control_scope_bind(scope);
   aimee_ir_session_set_sink(sink, scope);
   char why[256];
   uint32_t mutation = AIMEE_TOOL_POLICY_WRITE | AIMEE_TOOL_POLICY_FILESYSTEM;
   owner.request_id[0] = 0;
   assert(!server_unknown_effect_policy("write_file", "{}", "normal", mutation, why, sizeof why));
   snprintf(owner.request_id, sizeof owner.request_id, "host-request");
   assert(!prompts);
   unknown = 1;
   assert(!server_unknown_effect_policy("read_file", "{}", "inspect",
                                        AIMEE_TOOL_POLICY_READ | AIMEE_TOOL_POLICY_FILESYSTEM, why,
                                        sizeof why));
   assert(!prompts);
   assert(!server_unknown_effect_policy_stage("read_file", "{}", "inspect-final",
                                              AIMEE_TOOL_POLICY_READ, 1, why, sizeof why));
   assert(!prompts);
   assert(!server_unknown_effect_policy("write_file", "{\"value\":1}", "one", mutation, why,
                                        sizeof why));
   assert(prompts == 1 && responses == 1);
   assert(!server_unknown_effect_policy_stage("write_file", "{\"value\":1}", "one", mutation, 1,
                                              why, sizeof why));
   assert(prompts == 1 && responses == 1);
   // An allowed response is one use, even if the model replays the same attempt.
   assert(server_unknown_effect_policy("write_file", "{\"value\":1}", "one", mutation, why,
                                       sizeof why) < 0);
   assert(prompts == 1);
   server_control_scope_bind(NULL);
   server_control_scope_close(scope);
   scope = server_control_scope_open("session", "turn", "actor");
   assert(scope);
   server_control_scope_bind(scope);
   aimee_ir_session_set_sink(sink, scope);
   decision = "deny";
   assert(server_unknown_effect_policy("bash", "{}", "two", AIMEE_TOOL_POLICY_EXECUTION, why,
                                       sizeof why) < 0);
   assert(prompts == 2 && responses == 2);
   // A different model/attempt in this same host turn cannot re-trigger denial.
   assert(server_unknown_effect_policy("write_file", "{}", "new-model", mutation, why, sizeof why) <
          0);
   assert(prompts == 2);
   unavailable = 1;
   assert(server_unknown_effect_policy("write_file", "{}", "uncertain", mutation, why, sizeof why) <
          0);
   unavailable = 0;
   for (int fault = 0; fault < 2; fault++)
   {
      server_control_scope_bind(NULL);
      server_control_scope_close(scope);
      scope = server_control_scope_open("session", "turn", "actor");
      assert(scope);
      server_control_scope_bind(scope);
      aimee_ir_session_set_sink(sink, scope);
      emit_failure = fault == 0;
      fail_owner = fault == 1;
      int before = prompts;
      assert(server_unknown_effect_policy("write_file", "{}", "failed-publication", mutation, why,
                                          sizeof why) < 0);
      cJSON *late = cJSON_CreateObject();
      cJSON_AddStringToObject(late, "decision", "allow");
      assert(server_control_reply("session", last_request_id, "actor", late) != 0);
      cJSON_Delete(late);
      emit_failure = fail_owner = 0;
      assert(server_unknown_effect_policy("write_file", "{}", "another-attempt", mutation, why,
                                          sizeof why) < 0);
      assert(prompts == before);
   }
   server_control_scope_bind(NULL);
   server_control_scope_close(scope);
   scope = server_control_scope_open("session", "turn", "actor");
   assert(scope);
   server_control_scope_bind(scope);
   aimee_ir_session_set_sink(sink, scope);
   decision = "allow";
   int before = prompts;
   assert(!server_unknown_effect_policy("write_file", "{\"path\":\"relative\",\"value\":1}",
                                        "rewrite", mutation, why, sizeof why));
   assert(prompts == before + 1);
   assert(!server_unknown_effect_policy_stage("write_file",
                                              "{\"path\":\"/workspace/target\",\"value\":2}",
                                              "rewrite", mutation, 1, why, sizeof why));
   assert(prompts == before + 2);
   assert(server_unknown_effect_policy_stage("write_file",
                                             "{\"path\":\"/workspace/target\",\"value\":2}",
                                             "rewrite", mutation, 1, why, sizeof why) < 0);
   assert(prompts == before + 2);
   for (int fault = 0; fault < 3; fault++)
   {
      server_control_scope_bind(NULL);
      server_control_scope_close(scope);
      scope = server_control_scope_open("session", "turn", "actor");
      assert(scope);
      server_control_scope_bind(scope);
      aimee_ir_session_set_sink(sink, scope);
      int before = prompts;
      assert(
          !server_unknown_effect_policy("write_file", "{}", "liveness", mutation, why, sizeof why));
      assert(prompts == before + 1);
      cancelled = fault == 0;
      owner.context_refused = fault == 1;
      capture_failed = fault == 2;
      assert(server_unknown_effect_policy_stage("write_file", "{}", "liveness", mutation, 1, why,
                                                sizeof why) < 0);
      cancelled = 0;
      owner.context_refused = 0;
      capture_failed = 0;
      assert(server_unknown_effect_policy_stage("write_file", "{}", "liveness", mutation, 1, why,
                                                sizeof why) < 0);
      assert(prompts == before + 1);
   }
   server_control_scope_bind(NULL);
   server_control_scope_close(scope);
   scope = server_control_scope_open("session", "turn", "actor");
   assert(scope);
   server_control_scope_bind(scope);
   aimee_ir_session_set_sink(sink, scope);
   source_count = 2048;
   large_old_input = 1;
   int previous = prompts;
   assert(!server_unknown_effect_policy("write_file", "{}", "large-source", mutation, why,
                                        sizeof why));
   assert(prompts == previous + 1);
   assert(!server_unknown_effect_policy_stage("write_file", "{}", "large-source", mutation, 1, why,
                                              sizeof why));
   assert(prompts == previous + 1);
   owner.model_execution = 0;
   assert(!server_unknown_effect_policy("write_file", "{}", "operator", mutation, why, sizeof why));
   aimee_ir_session_set_sink(NULL, NULL);
   server_control_scope_bind(NULL);
   server_control_scope_close(scope);
   puts("unknown-effect source gate: read inspection, one-use permission, replay and denied-turn "
        "suppression passed");
   return 0;
}
