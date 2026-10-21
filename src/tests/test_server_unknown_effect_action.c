/* Reuse the existing governed resource/store metadata surrogate, but exercise
 * the real action_begin and source-owned permission gate together. This fixture
 * proves admission ordering, not actual filesystem execution or test grading. */
#define main                               governed_action_baseline_main
#define policy_recheck_action_tool_attempt fixture_baseline_recheck
#define policy_unknown_effect_clear        fixture_clear_noop
#define GOVERNED_ACTION_CAPTURE_EXTERNAL   1
#include "test_server_governed_action.c"
#undef GOVERNED_ACTION_CAPTURE_EXTERNAL
#undef policy_recheck_action_tool_attempt
#undef main
#undef policy_unknown_effect_clear
#include "server_unknown_effect_policy.h"
#include "server_native_history.h"
#include "server_session_capture.h"
#include "server_session_control.h"
#include <aimee/tools/module_api.h>

static const char *active_turn;
static int prompts, deny_final;
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
   return capture != NULL;
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
   *messages = cJSON_CreateArray();
   *receipts = cJSON_CreateArray();
   cJSON_AddItemToArray(
       *receipts,
       cJSON_Parse(
           "{\"event_id\":\"receipt-event\",\"receipt\":{\"source\":{\"session_id\":\"session\","
           "\"turn_id\":\"old-turn\",\"sequence\":\"2\",\"tool_id\":\"old-call\",\"name\":\"write_"
           "file\"}},\"call\":{\"input\":{\"value\":\"metadata-fixture-not-execution-proof\"}}}"));
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
   assert(!server_control_register(scope, event->request_id, "permission", event->control));
   prompts++;
   const cJSON *action = cJSON_GetObjectItemCaseSensitive(event->control, "input");
   const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(action, "arguments");
   const char *path = text(arguments, "path");
   char id[256];
   snprintf(id, sizeof id, "%s:%s", active_turn, event->request_id);
   cJSON *reply = cJSON_CreateObject();
   cJSON_AddStringToObject(reply, "decision",
                           deny_final && !strcmp(path, "/workspace/target") ? "deny" : "allow");
   assert(!server_control_reply("session", id, "alice", reply));
   cJSON_Delete(reply);
   return 0;
}
int main(void)
{
   snprintf(context.principal, sizeof context.principal, "alice");
   snprintf(context.vault_principal, sizeof context.vault_principal, "alice");
   context.model_execution = 1;
   for (int variant = 0; variant < 3; variant++)
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
      calls = dispatched = 0;
      policy_action_attempt("attempt");
      cJSON *args = variant
                        ? input()
                        : cJSON_Parse("{\"path\":\"/workspace/target\",\"content\":\"expected\"}");
      char *wire = cJSON_PrintUnformatted(args), reason[256];
      assert(!server_unknown_effect_policy("write_file", wire, "attempt", AIMEE_TOOL_POLICY_WRITE,
                                           reason, sizeof reason));
      free(wire);
      assert(prompts == 1 && calls == 0 && dispatched == 0);
      int result = policy_action_begin("write_file", &args, "/workspace", "session", 1);
      assert(!strcmp(text(args, "path"), "/workspace/target"));
      if (variant == 2)
         assert(result < 0 && calls == 0 && dispatched == 0 && prompts == 2);
      else
      {
         assert(!result && dispatched == 1 && prompts == (variant ? 2 : 1));
         int error = 0;
         char *receipt = policy_action_finish("ok", strdup("saved"), &error);
         assert(receipt && !error);
         free(receipt);
      }
      cJSON_Delete(args);
      aimee_ir_session_set_sink(NULL, NULL);
      server_control_scope_bind(NULL);
      server_control_scope_close(scope);
   }
   cJSON_Delete(stored_intent);
   puts("actual governed admission: unchanged permission once, rewritten action separately "
        "approved, deny before durable dispatch passed");
   return 0;
}
