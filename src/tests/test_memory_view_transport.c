/* Native views consume complete Go envelopes through named host discovery. */
#include "cJSON.h"
#include "json_fluent.h"
#include "session_briefing.h"
#include "request_context.h"
#include "kb_client.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char *api_memory_stats(void);
extern char *api_dashboard_reminders(void);
extern char *api_dashboard_directives(void);
static const char *expected_operation;
static const char *reply;
static int expected_limit, transport_result = 1, initialized = 1, calls;
static request_context_t provider;
static int has_provider, selected_native, accepts, refuses, accept_result;
extern char *kb_client_session_briefing_commitments(int limit);
extern char *kb_client_session_briefing_directives(int limit);
const request_context_t *request_context_get(void)
{
   return has_provider ? &provider : NULL;
}
int request_context_refuse_assembly(const char *kind)
{
   assert(!strcmp(kind, "unavailable"));
   refuses++;
   return 0;
}
int aimee_module_tool_source_mode(const char *name)
{
   assert(!strcmp(name, "prospective_memory"));
   return selected_native ? -1 : 0;
}
int ingress_preinject_accept_native_projection(const cJSON *projection)
{
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(projection, "native_context")));
   accepts++;
   return accept_result;
}
static void check_custody_argument(const cJSON *args)
{
   const cJSON *mode = cJSON_GetObjectItemCaseSensitive(args, "native_source_mode");
   if (strstr(expected_operation, "briefing") && has_provider)
      assert(!mode); /* Capture remains enabled for the provider. */
   else if (strcmp(expected_operation, "stats-dashboard"))
      assert(cJSON_IsFalse(mode));
}
char *kb_v1_action_request(const char *method, cJSON *args)
{
   assert(!strncmp(method, "session_briefing.", 17));
   check_custody_argument(args);
   cJSON_Delete(args);
   return reply ? strdup(reply) : NULL;
}

int kb_store_is_initialized(void)
{
   return initialized;
}
int aimee_module_commands_dispatch_internal_timeout(const char *method, const cJSON *args,
                                                    int timeout_ms, cJSON **result)
{
   assert(timeout_ms == 60000);
   assert(strcmp(method, "memory.runtime") == 0);
   assert(strcmp(jo_cstr(args, "operation"), expected_operation) == 0);
   if (strstr(expected_operation, "briefing"))
      assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(args, "limit")) ==
             expected_limit);
   check_custody_argument(args);
   calls++;
   *result = reply ? cJSON_Parse(reply) : NULL;
   return transport_result;
}
int aimee_module_commands_dispatch_internal_caller_timeout(const char *method, const cJSON *args,
                                                           int timeout_ms, cJSON **result)
{
   return aimee_module_commands_dispatch_internal_timeout(method, args, timeout_ms, result);
}
int main(void)
{
   kb_client_memory_scope_context_set("outer-workspace", "outer-project", 1);
   kb_client_memory_scope_context_state_t saved;
   kb_client_memory_scope_context_snapshot(&saved);
   kb_client_memory_scope_context_set("inner-workspace", "inner-project", 0);
   kb_client_memory_scope_context_restore(&saved);
   cJSON *nested = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(nested);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(nested, "include_all")));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(nested, "project")),
                  "outer-project"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(nested, "workspace")),
                  "outer-workspace"));
   cJSON_Delete(nested);
   kb_client_memory_scope_context_restore(NULL);
   nested = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(nested);
   assert(cJSON_GetArraySize(nested) == 0);
   cJSON_Delete(nested);

   char text[12001];
   memset(text, 'x', sizeof(text) - 1);
   text[sizeof(text) - 1] = 0;
   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "status", "ok");
   cJSON_AddStringToObject(body, "body", text);
   char *encoded = cJSON_PrintUnformatted(body);
   reply = encoded;
   expected_operation = "prospective-briefing";
   expected_limit = 0;
   char *value = kb_client_session_briefing_commitments(expected_limit);
   assert(value && strcmp(value, text) == 0);
   free(value);
   expected_operation = "directive-briefing";
   expected_limit = 2;
   value = kb_client_session_briefing_directives(expected_limit);
   assert(value && strcmp(value, text) == 0);
   free(value);
   free(encoded);
   cJSON_Delete(body);

   struct view
   {
      const char *operation;
      char *(*render)(void);
   } views[] = {
       {"stats-dashboard", api_memory_stats},
       {"prospective-dashboard", api_dashboard_reminders},
       {"directive-dashboard", api_dashboard_directives},
   };
   for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++)
   {
      expected_operation = views[i].operation;
      reply = "{\"status\":\"ok\",\"dashboard\":{\"count\":4}}";
      value = views[i].render();
      assert(value && strcmp(value, "{\"count\":4}") == 0);
      free(value);
      const char *failures[] = {NULL, "{}", "[]", "{\"status\":\"error\",\"dashboard\":{}}"};
      for (size_t j = 0; j < sizeof(failures) / sizeof(failures[0]); j++)
      {
         reply = failures[j];
         assert(views[i].render() == NULL);
      }
      reply = "{\"status\":\"ok\",\"dashboard\":{}}";
      transport_result = -1;
      assert(views[i].render() == NULL);
      transport_result = 1;
   }
   expected_operation = "directive-briefing";
   expected_limit = 1;
   reply = "{\"status\":\"error\",\"block\":\"must not inject\"}";
   assert(kb_client_session_briefing_directives(1) == NULL);
   reply = "{\"status\":\"ok\",\"block\":17}";
   assert(kb_client_session_briefing_directives(1) == NULL);
   /* Existing KB wrappers enroll exact native text before returning it for
    * model execution and receipt-required assembly. Human/session-start
    * presentation-only exports need no provider callback. */
   char *(*briefings[])(int) = {kb_client_session_briefing_commitments,
                                kb_client_session_briefing_directives};
   selected_native = 1;
   has_provider = 1;
   for (int context_kind = 0; context_kind < 2; context_kind++)
   {
      provider.model_execution = context_kind == 0;
      provider.memory_receipt_required = context_kind == 1;
      for (size_t i = 0; i < sizeof(briefings) / sizeof(briefings[0]); i++)
      {
         expected_operation = i % 2 ? "directive-briefing" : "prospective-briefing";
         expected_limit = 1;
         reply = "{\"status\":\"ok\",\"block\":\"exact native text\",\"body\":\"exact native "
                 "text\",\"native_context\":{\"text\":\"exact native text\"}}";
         int before_accept = accepts;
         value = briefings[i](1);
         assert(value && !strcmp(value, "exact native text"));
         free(value);
         assert(accepts == before_accept + 1);
         accept_result = -1;
         assert(briefings[i](1) == NULL);
         accept_result = 0;
         reply =
             "{\"status\":\"ok\",\"block\":\"changed\",\"body\":\"changed\",\"native_context\":{"
             "\"text\":\"original\"}}";
         before_accept = accepts;
         assert(briefings[i](1) == NULL && accepts == before_accept);
         reply = "{\"status\":\"ok\",\"block\":\"unfenced\",\"body\":\"unfenced\"}";
         assert(briefings[i](1) == NULL);
         has_provider = 0;
         value = briefings[i](1);
         assert(value && !strcmp(value, "unfenced"));
         free(value);
         has_provider = 1;
      }
   }
   assert(refuses == 12);
   has_provider = 0;
   initialized = 0;
   int before = calls;
   assert(api_dashboard_reminders() == NULL && api_dashboard_directives() == NULL);
   assert(calls == before);
   puts("native memory views generic transport: ok");
   return 0;
}
