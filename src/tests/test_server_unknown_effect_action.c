/* Reuse the existing governed resource/store metadata surrogate, but exercise
 * the real action_begin and source-owned permission gate together. This fixture
 * proves admission ordering, not actual filesystem execution or test grading. */
#define main                               governed_action_baseline_main
#define policy_recheck_action_tool_attempt fixture_baseline_recheck
#define policy_unknown_effect_clear        fixture_clear_noop
#define GOVERNED_ACTION_CAPTURE_EXTERNAL   1
#define aimee_module_command_call_context  baseline_resource_call
#include "test_server_governed_action.c"
#undef GOVERNED_ACTION_CAPTURE_EXTERNAL
#undef aimee_module_command_call_context
#undef policy_recheck_action_tool_attempt
#undef main
#undef policy_unknown_effect_clear
#include "server_unknown_effect_policy.h"
#include "server_native_history.h"
#include "server_session_capture.h"
#include "server_session_control.h"
#include <aimee/tools/module_api.h>
#include <aimee/tools/agent_tools.h>

static const char *active_turn;
static int prompts, deny_final, fail_publication, capture_live = 1, history_fail,
                                                  receipt_generation;
static const char *canonical_content = "expected";
static char displayed_content[64], displayed_receipt[64];
cJSON *aimee_module_command_call_context(uint32_t event, uint32_t stage, const char *verb,
                                         const cJSON *args, const cJSON *caller)
{
   cJSON *resource = baseline_resource_call(event, stage, verb, args, caller);
   if (resource && !strcmp(verb, "describe"))
      cJSON_ReplaceItemInObject(cJSON_GetObjectItem(resource, "effective_arguments"), "content",
                                cJSON_CreateString(canonical_content));
   return resource;
}
server_session_capture_t *server_session_capture_current(void)
{
   return (void *)1;
}
const char *server_session_capture_session(const server_session_capture_t *capture)
{
   return capture ? "session" : NULL;
}
const char *server_session_capture_turn(const server_session_capture_t *capture)
{
   return capture ? active_turn : NULL;
}
int server_session_capture_accepting(const server_session_capture_t *capture)
{
   return capture != NULL && capture_live;
}
const char *session_id(void)
{
   return "session";
}
const char *agent_get_request_vault_principal(void)
{
   return "alice";
}
int agent_request_cancelled(void)
{
   return 0;
}
int server_native_history_load(const char *session, const char *actor, const char *turn,
                               int running, cJSON **messages, cJSON **receipts)
{
   (void)session;
   (void)actor;
   (void)turn;
   (void)running;
   if (history_fail)
      return -1;
   *messages = cJSON_CreateArray();
   *receipts = cJSON_CreateArray();
   cJSON_AddItemToArray(
       *receipts,
       cJSON_Parse(
           "{\"event_id\":\"receipt-event\",\"receipt\":{\"source\":{\"session_id\":\"session\","
           "\"turn_id\":\"old-turn\",\"sequence\":\"2\",\"tool_id\":\"old-call\",\"name\":\"write_"
           "file\"}},\"call\":{\"input\":{\"value\":\"metadata-fixture-not-execution-proof\"}}}"));
   if (receipt_generation)
      cJSON_ReplaceItemInObject(cJSON_GetArrayItem(*receipts, 0), "event_id",
                                cJSON_CreateString("receipt-event-new"));
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
   return 0;
}
void policy_unknown_effect_clear(void)
{
   (void)server_unknown_effect_policy_stage(NULL, NULL, NULL, 0, 2, NULL, 0);
}
int policy_recheck_action_tool_attempt(const char *name, const char *effect, const char *args,
                                       const char *attempt, uint32_t resource_categories,
                                       char *reason, size_t cap)
{
   if (fixture_baseline_recheck(name, effect, args, attempt, resource_categories, reason, cap))
      return -1;
   return server_unknown_effect_policy_stage(name, args, attempt, resource_categories, 1, reason,
                                             cap);
}
static int permission(const aimee_session_event_t *event, void *opaque)
{
   if (event->kind == AIMEE_SESSION_CONTROL_RESPONSE)
      return 0;
   assert(event->kind == AIMEE_SESSION_CONTROL_REQUEST);
   server_control_scope_t *scope = opaque;
   prompts++;
   if (fail_publication)
      return -1;
   assert(!server_control_register(scope, event->request_id, "permission", event->control));
   const cJSON *action = cJSON_GetObjectItemCaseSensitive(event->control, "input");
   const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(action, "arguments");
   const char *path = text(arguments, "path");
   assert(!strcmp(path, "/workspace/target"));
   snprintf(displayed_content, sizeof displayed_content, "%s", text(arguments, "content"));
   const cJSON *unknown = cJSON_GetObjectItem(action, "unknown_receipts");
   snprintf(displayed_receipt, sizeof displayed_receipt, "%s",
            text(cJSON_GetArrayItem(cJSON_GetObjectItem(unknown, "sources"), 0), "event_id"));
   char id[256];
   snprintf(id, sizeof id, "%s:%s", active_turn, event->request_id);
   cJSON *reply = cJSON_CreateObject();
   cJSON_AddStringToObject(reply, "decision",
                           deny_final && !strcmp(path, "/workspace/target") ? "deny" : "allow");
   assert(!server_control_reply("session", id, "alice", reply));
   cJSON_Delete(reply);
   return 0;
}
static void owner_contract(void)
{
   cJSON *args = input();
   agent_tools_register_action_owner(NULL, NULL);
   assert(!agent_tools_action_managed("write_file", args));
   agent_tools_register_action_owner(policy_action_begin, policy_action_finish);
   assert(agent_tools_action_managed("write_file", args));
   assert(agent_tools_action_managed("read_file", args));
   assert(agent_tools_action_managed("edit_file", args));
   assert(agent_tools_action_managed("edit_symbol", args));
   assert(!agent_tools_action_managed("bash", args));
   assert(!agent_tools_action_managed("unregistered-tool", args));
   cJSON_AddBoolToObject(args, "dry_run", 1);
   assert(!agent_tools_action_managed("edit_file", args));
   cJSON_DeleteItemFromObject(args, "path");
   assert(!agent_tools_action_managed("edit_symbol", args));
   cJSON_Delete(args);
}
static void initial_gate_contract(void)
{
   for (int managed_owner = 0; managed_owner < 2; managed_owner++)
   {
      active_turn = managed_owner ? "unmanaged-bash" : "missing-action-owner";
      snprintf(context.request_id, sizeof context.request_id, "%s", active_turn);
      server_control_scope_t *scope = server_control_scope_open("session", active_turn, "alice");
      assert(scope);
      server_control_scope_bind(scope);
      aimee_ir_session_set_sink(permission, scope);
      agent_tools_register_action_owner(managed_owner ? policy_action_begin : NULL,
                                        managed_owner ? policy_action_finish : NULL);
      prompts = deny_final = fail_publication = 0;
      cJSON *args = cJSON_Parse("{\"path\":\"/workspace/target\",\"content\":\"expected\"}");
      char *wire = cJSON_PrintUnformatted(args), reason[256];
      // Without a registered final owner, or for unmanaged Bash, the first
      // admission gate must still require the exact one-use response.
      assert(!server_unknown_effect_policy(managed_owner ? "bash" : "write_file", wire, "attempt",
                                           AIMEE_TOOL_POLICY_WRITE, reason, sizeof reason));
      assert(prompts == 1 && !calls && !dispatched);
      free(wire);
      cJSON_Delete(args);
      aimee_ir_session_set_sink(NULL, NULL);
      server_control_scope_bind(NULL);
      server_control_scope_close(scope);
   }
}
int main(void)
{
   owner_contract();
   snprintf(context.principal, sizeof context.principal, "alice");
   snprintf(context.vault_principal, sizeof context.vault_principal, "alice");
   context.model_execution = 1;
   initial_gate_contract();
   for (int variant = 0; variant < 8; variant++)
   {
      char turn[32];
      snprintf(turn, sizeof turn, "turn-%d", variant);
      active_turn = turn;
      snprintf(context.request_id, sizeof context.request_id, "request-%d", variant);
      server_control_scope_t *scope = server_control_scope_open("session", turn, "alice");
      assert(scope);
      server_control_scope_bind(scope);
      aimee_ir_session_set_sink(permission, scope);
      prompts = 0;
      deny_final = variant == 2;
      fail_publication = variant == 5;
      capture_live = 1;
      history_fail = receipt_generation = 0;
      canonical_content = "expected";
      agent_tools_register_action_owner(policy_action_begin, policy_action_finish);
      calls = dispatched = 0;
      policy_action_attempt("attempt");
      cJSON *args = variant
                        ? input()
                        : cJSON_Parse("{\"path\":\"/workspace/target\",\"content\":\"expected\"}");
      char *wire = cJSON_PrintUnformatted(args), reason[256];
      if (variant == 6)
         capture_live = 0;
      if (variant == 7)
         history_fail = 1;
      int initial = server_unknown_effect_policy("write_file", wire, "attempt",
                                                 AIMEE_TOOL_POLICY_WRITE, reason, sizeof reason);
      free(wire);
      if (variant >= 6)
      {
         assert(initial < 0 && !prompts && !calls && !dispatched);
         goto variant_done;
      }
      // No user approval may be requested for the pre-normalized path. The
      // actual registered action owner must supply the exact final arguments.
      assert(!initial && prompts == 0 && calls == 0 && dispatched == 0);
      if (variant == 4)
         receipt_generation = 1;
      int result = policy_action_begin("write_file", &args, "/workspace", "session", 1);
      assert(!strcmp(text(args, "path"), "/workspace/target"));
      assert(prompts == 1);
      if (variant == 2 || variant == 5)
      {
         assert(result < 0 && calls == 0 && dispatched == 0);
         wire = cJSON_PrintUnformatted(args);
         assert(server_unknown_effect_policy("write_file", wire, "new-attempt",
                                             AIMEE_TOOL_POLICY_WRITE, reason, sizeof reason) < 0);
         assert(prompts == 1 && !calls && !dispatched);
         free(wire);
      }
      else
      {
         assert(!result && dispatched == 1);
         assert(!strcmp(displayed_content, "expected"));
         assert(!strcmp(displayed_receipt, variant == 4 ? "receipt-event-new" : "receipt-event"));
         int error = 0;
         char *receipt = policy_action_finish("ok", strdup("saved"), &error);
         assert(receipt && !error);
         free(receipt);
         if (variant == 3)
         {
            // A changed payload cannot borrow the first Allow; its exact new
            // canonical action requires another response, denied here.
            canonical_content = "changed";
            deny_final = 1;
            policy_action_attempt("attempt");
            assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) < 0);
            assert(prompts == 2 && dispatched == 1 && !strcmp(displayed_content, "changed"));
         }
         else
         {
            // Even unchanged arguments cannot consume the final approval twice.
            policy_action_attempt("attempt");
            assert(policy_action_begin("write_file", &args, "/workspace", "session", 1) < 0);
            assert(prompts == 1 && dispatched == 1);
         }
      }
   variant_done:
      cJSON_Delete(args);
      aimee_ir_session_set_sink(NULL, NULL);
      server_control_scope_bind(NULL);
      server_control_scope_close(scope);
   }
   cJSON_Delete(stored_intent);
   agent_tools_register_action_owner(NULL, NULL);
   puts("actual governed admission: normalized action one permission/dispatch, changed payload "
        "requires approval, deny/replay/publication/liveness fences passed");
   return 0;
}
