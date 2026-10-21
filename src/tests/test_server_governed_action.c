#include "aimee.h"
#include "server.h"
#include "agent_exec.h"
#include "request_context.h"
#include "server_session_capture.h"
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
static const char *replay_state;
static const char *expected_actor = "alice";
static const char *capture_source = "session";
#ifndef GOVERNED_ACTION_CAPTURE_EXTERNAL
static int capture_present, capture_ready = 1;
static const char *capture_destination = "session", *capture_turn = "host-turn";
server_session_capture_t *server_session_capture_current(void)
{
   return capture_present ? (void *)1 : NULL;
}
const char *server_session_capture_session(const server_session_capture_t *capture)
{
   return capture ? capture_destination : NULL;
}
const char *server_session_capture_turn(const server_session_capture_t *capture)
{
   return capture ? capture_turn : NULL;
}
int server_session_capture_accepting(const server_session_capture_t *capture)
{
   return capture && capture_ready;
}
#endif
int server_session_capture_retain_current(const char *source, const char *actor,
                                          server_session_capture_t **out)
{
   *out = NULL;
   server_session_capture_t *current = server_session_capture_current();
   if (!current)
      return 0;
   if (strcmp(source, capture_source) || strcmp(actor, expected_actor) ||
       !server_session_capture_accepting(current))
      return -1;
   *out = current;
   return 0;
}
void server_session_capture_release(server_session_capture_t *capture)
{
   assert(!capture || capture == (void *)1);
}

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
void policy_unknown_effect_clear(void)
{
}
int policy_recheck_action_tool_attempt(const char *name, const char *effect, const char *args,
                                       const char *attempt, uint32_t resource_categories,
                                       char *reason, size_t cap)
{
   (void)name;
   (void)resource_categories;
   assert(attempt && attempt[0]);
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
   assert(!strcmp(text(args, "principal"), "alice"));
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
   assert(!strcmp(text(caller, "principal"), "alice"));
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
   assert(!strcmp(principal, expected_actor) && !strcmp(sid, "session"));
   calls++;
   if (fail_store)
      return -1;
   cJSON *j = cJSON_Parse(wire), *r = cJSON_CreateObject();
   const char *op = text(j, "operation");
   const char *host_turn = text(j, "turn_id"), *host_session = text(j, "turn_session_id");
   assert((host_turn[0] != 0) == (host_session[0] != 0));
   if (strcmp(op, "issue") && (host_turn[0] || !strcmp(op, "admit") || !strcmp(op, "dispatch")))
   {
      assert(!strcmp(host_turn, text(stored_intent, "turn_id")));
      assert(!strcmp(host_session, text(stored_intent, "turn_session_id")));
   }
   cJSON_AddBoolToObject(r, "allowed", 1);
   if (!strcmp(op, "issue"))
   {
      cJSON_Delete(stored_intent);
      stored_intent = cJSON_Duplicate(cJSON_GetObjectItem(j, "intent"), 1);
      assert(!strcmp(text(stored_intent, "attempt_id"), "attempt"));
      if (host_turn[0])
      {
         /* The real store binds these host-only fields into its immutable
          * intent. Model arguments cannot provide either tuple component. */
         cJSON_AddStringToObject(stored_intent, "turn_session_id", host_session);
         cJSON_AddStringToObject(stored_intent, "turn_id", host_turn);
      }
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
         if (replay_state)
         {
            cJSON *receipt = cJSON_AddObjectToObject(r, "receipt");
            cJSON_AddStringToObject(receipt, "state", replay_state);
            cJSON_AddBoolToObject(r, "dispatch_allowed", 0);
         }
         else
         {
            dispatched++;
            cJSON_AddBoolToObject(r, "dispatch_allowed", 1);
         }
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
#ifndef GOVERNED_ACTION_CAPTURE_EXTERNAL
static void test_canonical_capture_binding(void)
{
   memset(&context, 0, sizeof context);
   snprintf(context.principal, sizeof context.principal, "alice");
   snprintf(context.request_id, sizeof context.request_id, "canonical-request");
   context.model_execution = 1;
   expected_actor = "alice";
   fail_verify = fail_store = fail_resource = memory_required = stale = 0;
   cJSON *args = cJSON_Parse("{\"path\":\"target\",\"content\":\"expected\","
                             "\"turn_session_id\":\"forged\",\"turn_id\":\"forged\"}");
   int before = calls;
   policy_action_attempt("attempt");
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) < 0);
   assert(calls == before); /* A model cannot omit its canonical capture. */
   capture_present = 1;
   capture_source = "foreign-source";
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) < 0);
   assert(calls == before);
   capture_source = "session";
   capture_ready = 0;
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) < 0);
   assert(calls == before);
   capture_ready = 1;
   capture_destination = "canonical-child";
   capture_turn = "canonical-turn";
   policy_action_attempt("attempt");
   assert(!policy_action_begin("write_file", &args, "/workspace", "session", 1));
   assert(!strcmp(text(stored_intent, "turn_session_id"), "canonical-child"));
   assert(!strcmp(text(stored_intent, "turn_id"), "canonical-turn"));
   /* Completion retains the original child tuple even when callback context
    * has moved to another turn; the source journal session stays unchanged. */
   capture_destination = "another-child";
   capture_turn = "another-turn";
   int error = 0;
   char *result = policy_action_finish("ok", strdup("saved"), &error);
   assert(!error && result);
   free(result);
   cJSON_Delete(args);
   capture_present = 0;
   context.model_execution = 0;
}
#endif

int main(void)
{
   snprintf(context.principal, sizeof(context.principal), "alice");
   snprintf(context.request_id, sizeof(context.request_id), "request");
   policy_action_attempt("attempt");
   cJSON *args = input();
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 0) == -1 && calls == 0);
   fail_resource = 1;
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) == -1 && calls == 0);
   int refused = 0;
   char *refusal = policy_action_finish("refused", strdup("error: not executed"), &refused);
   assert(refused && strstr(refusal, "exact_resource_adapter_unavailable"));
   free(refusal);
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
   int is_error = 1;
   char *result = policy_action_finish("ok", strdup("saved"), &is_error);
   assert(!is_error);
   cJSON *envelope = cJSON_Parse(result);
   assert(!strcmp(text(envelope, "completion_claim"), "effect_confirmed"));
   assert(outcomes == 1);
   free(result);
   cJSON_Delete(envelope);
   policy_action_attempt("attempt");
   guard_released = 0;
   fail_verify = 1;
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) == 0);
   result = policy_action_finish("ok", strdup("saved"), &is_error);
   assert(is_error);
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
   for (int unknown = 0; unknown < 2; unknown++)
   {
      policy_action_attempt("attempt");
      guard_released = 0;
      replay_state = unknown ? "outcome_unknown" : "acknowledged";
      assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) == -1);
      int failed = !unknown;
      char *prior = policy_action_finish("refused", strdup("not repeated"), &failed);
      assert(failed == unknown && prior && strstr(prior, replay_state));
      free(prior);
   }
   replay_state = NULL;
   /* Real native/browser requests retain an account principal distinct from
    * the canonical session actor. All durable phases, including reconciliation
    * and fork, must use the latter; resource and memory callers remain alice. */
   context.vault_transport = ATTEST_WEBCHAT_TRUSTED;
   snprintf(context.vault_principal, sizeof(context.vault_principal), "webuser:alice");
   expected_actor = context.vault_principal;
   policy_action_attempt("attempt");
   guard_released = 0;
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) == 0);
   result = policy_action_finish("ok", strdup("saved"), &is_error);
   assert(!is_error);
   free(result);
   req = cJSON_Parse(
       "{\"operation\":\"reconcile\",\"session_id\":\"session\",\"action_id\":\"action\"}");
   reconciled = action_receipt_command(req);
   assert(!strcmp(text(reconciled, "completion_claim"), "effect_confirmed"));
   cJSON_Delete(req);
   cJSON_Delete(reconciled);
   snprintf(context.exploration_binding, sizeof(context.exploration_binding),
            "{\"principal\":\"webuser:alice\",\"session\":\"parent\"}");
   assert(policy_action_inherit("session") == 0);
   snprintf(context.exploration_binding, sizeof(context.exploration_binding),
            "{\"principal\":\"alice\",\"session\":\"parent\"}");
   before = calls;
   assert(policy_action_inherit("session") == -1 && calls == before);
   context.context_refused = 0;
   context.exploration_binding[0] = 0;
   /* An attested-but-empty actor fails closed rather than using alice. */
   context.vault_principal[0] = 0;
   policy_action_attempt("attempt");
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) == -1);
   assert(calls == before);
   /* Preserve the complete canonical actor across the finish callback. */
   memset(context.vault_principal, 'x', sizeof(context.vault_principal) - 1);
   context.vault_principal[sizeof(context.vault_principal) - 1] = 0;
   char long_actor[VAULT_PRINCIPAL_MAX];
   snprintf(long_actor, sizeof(long_actor), "%s", context.vault_principal);
   expected_actor = long_actor;
   policy_action_attempt("attempt");
   guard_released = 0;
   assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) == 0);
   snprintf(context.vault_principal, sizeof(context.vault_principal), "webuser:other-request");
   result = policy_action_finish("ok", strdup("saved"), &is_error);
   assert(!is_error);
   free(result);
   cJSON_Delete(args);
#ifndef GOVERNED_ACTION_CAPTURE_EXTERNAL
   test_canonical_capture_binding();
#endif
   cJSON_Delete(stored_intent);
   puts("governed action host transport: ok");
   return 0;
}
