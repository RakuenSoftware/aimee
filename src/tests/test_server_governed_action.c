#include "aimee.h"
#include "server.h"
#include "agent_exec.h"
#include "request_context.h"
#include "module_commands.h"
#include "ingress_preinject.h"
#include "db1_client/session_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static request_context_t context;
int request_context_refuse_assembly(const char *kind)
{
   (void)kind;
   context.context_refused = 1;
   return 0;
}

static cJSON *stored_intent;
static int calls, dispatched, outcomes, guard_held, guard_released;
static int fail_resource, fail_verify, fail_store, memory_required, stale;
static const char *text(const cJSON *j, const char *key)
{
   const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, key));
   return s ? s : "";
}
const request_context_t *request_context_get(void)
{
   return &context;
}
const char *request_context_principal(void)
{
   return context.principal;
}
char *safe_strdup(const char *s)
{
   return strdup(s);
}
cJSON *server_error_kind_json(const char *kind, const char *message, const char *id)
{
   (void)message;
   (void)id;
   cJSON *j = cJSON_CreateObject();
   cJSON_AddStringToObject(j, "kind", kind);
   return j;
}
int send_and_free(server_conn_t *conn, cJSON *j)
{
   (void)conn;
   cJSON_Delete(j);
   return 0;
}
int policy_recheck_action_tool(const char *name, const char *effect, const char *args, char *reason,
                               size_t cap)
{
   (void)name;
   (void)effect;
   (void)reason;
   (void)cap;
   cJSON *j = cJSON_Parse(args);
   assert(!strcmp(text(j, "path"), "/workspace/target"));
   cJSON_Delete(j);
   return 0;
}
int ingress_preinject_acquire_send_guard(void **state)
{
   guard_held = 1;
   *state = &guard_held;
   return 0;
}
void ingress_preinject_release_send_guard(void *state)
{
   if (state)
   {
      assert(guard_held);
      guard_held = 0;
      guard_released++;
   }
}
int aimee_module_commands_dispatch_internal_timeout(const char *method, const cJSON *args,
                                                    int timeout, cJSON **reply)
{
   (void)timeout;
   assert(!strcmp(method, "memory.runtime") && !strcmp(text(args, "operation"), "action-evidence"));
   assert(guard_held);
   *reply = cJSON_Parse(stale ? "{\"status\":\"unavailable\"}"
                              : "{\"status\":\"ok\",\"evidence_sha256\":\"evidence\",\"context_"
                                "receipt\":\"receipt\",\"revocation_generation\":\"revision\","
                                "\"memory_guard\":\"guard\",\"memory_required\":true}");
   return 1;
}
cJSON *aimee_module_command_call_context(uint32_t event, uint32_t stage, const char *verb,
                                         const cJSON *args, const cJSON *caller)
{
   assert(event == 6914 && stage == 2);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(caller, "authenticated")));
   if (!strcmp(verb, "describe"))
   {
      if (fail_resource)
         return NULL;
      return cJSON_Parse(
          "{\"class\":\"file_write\",\"destination\":\"file:/workspace/"
          "target\",\"payload_sha256\":\"payload\",\"request_bytes\":\"44\",\"effective_"
          "arguments\":{\"path\":\"/workspace/target\",\"content\":\"expected\"}}");
   }
   assert(!strcmp(verb, "verify"));
   assert(!strcmp(text(args, "destination"), "file:/workspace/target"));
   if (fail_verify)
      return NULL;
   return cJSON_Parse(
       "{\"state\":\"effect_confirmed\",\"destination\":\"file:/workspace/"
       "target\",\"payload_sha256\":\"payload\",\"object_version\":\"verified-object\"}");
}
int db1_governed_action_apply(const char *principal, const char *sid, const char *wire, char *reply,
                              size_t cap)
{
   assert(!strcmp(principal, "alice") && !strcmp(sid, "session"));
   calls++;
   if (fail_store)
      return -1;
   cJSON *j = cJSON_Parse(wire), *r = cJSON_CreateObject();
   const char *op = text(j, "operation");
   cJSON_AddBoolToObject(r, "allowed", 1);
   if (!strcmp(op, "issue"))
   {
      cJSON_Delete(stored_intent);
      stored_intent = cJSON_Duplicate(cJSON_GetObjectItem(j, "intent"), 1);
      assert(!strcmp(text(stored_intent, "attempt_id"), "attempt"));
      cJSON_AddStringToObject(stored_intent, "action_id", "action");
      cJSON_AddStringToObject(stored_intent, "policy_generation", "policy");
   }
   if (!strcmp(op, "issue") || !strcmp(op, "inspect"))
   {
      cJSON *receipt = cJSON_AddObjectToObject(r, "receipt");
      cJSON_AddItemToObject(receipt, "intent", cJSON_Duplicate(stored_intent, 1));
      cJSON_AddStringToObject(receipt, "state", "outcome_unknown");
   }
   if (!strcmp(op, "admit") || !strcmp(op, "dispatch"))
   {
      if (memory_required)
         assert(guard_held && !guard_released);
      if (!strcmp(op, "dispatch"))
      {
         dispatched++;
         cJSON_AddBoolToObject(r, "dispatch_allowed", 1);
      }
   }
   if (!strcmp(op, "outcome") || !strcmp(op, "reconcile"))
   {
      assert(!guard_held);
      outcomes++;
      const cJSON *o = cJSON_GetObjectItem(j, "outcome");
      assert(!strcmp(text(o, "state"), fail_verify ? "outcome_unknown" : "effect_confirmed"));
      assert(strcmp(text(o, "object_version"), "forged"));
   }
   char *raw = cJSON_PrintUnformatted(r);
   snprintf(reply, cap, "%s", raw);
   free(raw);
   cJSON_Delete(j);
   cJSON_Delete(r);
   return 0;
}
static cJSON *input(void)
{
   return cJSON_Parse("{\"path\":\"target\",\"content\":\"expected\"}");
}
int main(void)
{
   snprintf(context.principal, sizeof(context.principal), "alice");
   snprintf(context.request_id, sizeof(context.request_id), "request");
   policy_action_attempt("attempt");
   cJSON *args = input();
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 0) == -1 && calls == 0);
   fail_resource = 1;
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) == -1 && calls == 0);
   fail_resource = 0;
   context.memory_receipt_required = 1;
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) == -1 && calls == 0);
   snprintf(context.memory_source_release, sizeof(context.memory_source_release), "ticket");
   memory_required = 1;
   stale = 1;
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) == -1 && calls == 0);
   assert(!guard_held && guard_released == 1);
   stale = 0;
   guard_released = 0;
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) == 0);
   assert(dispatched == 1 && guard_released == 1 && !guard_held);
   char *result = policy_action_finish("ok", strdup("saved"));
   cJSON *envelope = cJSON_Parse(result);
   assert(!strcmp(text(envelope, "completion_claim"), "effect_confirmed"));
   assert(outcomes == 1);
   free(result);
   cJSON_Delete(envelope);
   policy_action_attempt("attempt");
   guard_released = 0;
   fail_verify = 1;
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) == 0);
   result = policy_action_finish("ok", strdup("saved"));
   envelope = cJSON_Parse(result);
   assert(!strcmp(text(envelope, "completion_claim"), "outcome_unknown"));
   free(result);
   cJSON_Delete(envelope);
   cJSON *req = cJSON_Parse("{\"operation\":\"reconcile\",\"session_id\":\"session\",\"action_id\":"
                            "\"action\",\"directory\":\"/"
                            "workspace\",\"arguments_json\":\"{}\",\"outcome\":{\"state\":\"effect_"
                            "confirmed\",\"object_version\":\"forged\"}}");
   int before = outcomes;
   cJSON *reconciled = action_receipt_command(req);
   assert(outcomes == before && cJSON_HasObjectItem(reconciled, "reconciliation"));
   cJSON_Delete(reconciled);
   fail_verify = 0;
   reconciled = action_receipt_command(req);
   assert(outcomes == before + 1 &&
          !strcmp(text(reconciled, "completion_claim"), "effect_confirmed"));
   cJSON_Delete(req);
   cJSON_Delete(reconciled);
   cJSON_Delete(args);
   cJSON_Delete(stored_intent);
   puts("governed action host transport: ok");
   return 0;
}
