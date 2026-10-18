#include "server_session_control.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int results[4];
static void *answer(void *arg)
{
   size_t n = (size_t)arg;
   cJSON *reply = cJSON_Parse("{\"decision\":\"allow\"}");
   results[n] = server_control_reply("session", "turn:permission", "alice", reply);
   cJSON_Delete(reply);
   return NULL;
}
int main(void)
{
   server_control_scope_t *scope = server_control_scope_open("session", "turn", "alice");
   assert(scope);
   assert(server_control_scope_bind(scope) == NULL && server_control_scope_current() == scope);
   cJSON *request = cJSON_Parse("{\"tool_name\":\"Read\",\"input\":{\"path\":\"source.c\"}}");
   assert(server_control_register(scope, "permission", "permission", request) == 0);
   assert(server_control_register(scope, "permission", "permission", request) < 0);
   cJSON *reply = cJSON_Parse("{\"decision\":\"allow\"}");
   assert(server_control_reply("session", "turn:permission", "mallory", reply) < 0);
   assert(server_control_reply("session", "old-turn:permission", "alice", reply) == 1);
   assert(server_control_reply("other-session", "turn:permission", "alice", reply) == 1);
   pthread_t threads[4];
   for (size_t i = 0; i < 4; i++)
      assert(!pthread_create(&threads[i], NULL, answer, (void *)i));
   int accepted = 0;
   for (int i = 0; i < 4; i++)
   {
      pthread_join(threads[i], NULL);
      assert(results[i] == 0 || results[i] == 1);
      accepted += results[i] == 0;
   }
   assert(accepted == 1);
   char *id = NULL, *kind = NULL;
   cJSON *original = NULL, *decision = NULL;
   assert(server_control_take(scope, "wrong-id", &id, &kind, &original, &decision) == 0);
   assert(server_control_take(scope, "permission", &id, &kind, &original, &decision) == 1);
   assert(!strcmp(id, "permission") && !strcmp(kind, "permission") &&
          cJSON_Compare(request, original, 1));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(decision, "decision")), "allow"));
   free(id);
   free(kind);
   cJSON_Delete(original);
   cJSON_Delete(decision);
   assert(server_control_reply("session", "turn:permission", "alice", reply) == 1);
   assert(server_control_take(scope, NULL, &id, &kind, &original, &decision) == 0);
   cJSON_Delete(reply);
   reply = cJSON_Parse("{\"decision\":\"allow\",\"native_response\":{}}");
   assert(server_control_reply("session", "turn:permission", "alice", reply) < 0);
   /* Invalid form submissions do not consume the pending native request. */
   cJSON *form =
       cJSON_Parse("{\"form\":{\"message\":\"Pick options\",\"schema\":{\"type\":\"object\","
                   "\"properties\":{\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4},"
                   "\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1},"
                   "\"enabled\":{\"type\":\"boolean\"},\"tags\":{\"type\":\"array\",\"minItems\":1,"
                   "\"maxItems\":2,\"items\":{\"anyOf\":[{\"const\":\"a\",\"title\":\"A\"},"
                   "{\"const\":\"b\",\"title\":\"B\"}]}}},\"required\":[\"count\",\"name\","
                   "\"enabled\",\"tags\"]}}}");
   assert(form && server_control_register(scope, "form", "elicitation", form) == 0);
   const char *invalid[] = {
       "{}",
       "{\"count\":1.5,\"name\":\"x\",\"enabled\":true,\"tags\":[\"a\"]}",
       "{\"count\":5,\"name\":\"x\",\"enabled\":true,\"tags\":[\"a\"]}",
       "{\"count\":2,\"name\":\"xx\",\"enabled\":true,\"tags\":[\"a\"]}",
       "{\"count\":2,\"name\":\"x\",\"enabled\":1,\"tags\":[\"a\"]}",
       "{\"count\":2,\"name\":\"x\",\"enabled\":true,\"tags\":[\"unknown\"]}",
       "{\"count\":2,\"name\":\"x\",\"enabled\":true,\"tags\":[\"a\",\"a\"]}",
       "{\"count\":2,\"name\":\"x\",\"enabled\":true,\"tags\":[\"a\"],\"extra\":true}",
   };
   for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
   {
      cJSON *bad = cJSON_CreateObject();
      cJSON_AddStringToObject(bad, "decision", "allow");
      cJSON_AddItemToObject(bad, "content", cJSON_Parse(invalid[i]));
      assert(server_control_reply("session", "turn:form", "alice", bad) < 0);
      assert(server_control_take(scope, "form", &id, &kind, &original, &decision) == 0);
      cJSON_Delete(bad);
   }
   cJSON *accepted_form = cJSON_Parse("{\"decision\":\"allow\",\"content\":{\"count\":2,"
                                      "\"name\":\"é\",\"enabled\":false,\"tags\":[\"a\",\"b\"]}}");
   assert(server_control_reply("session", "turn:form", "mallory", accepted_form) < 0);
   assert(server_control_reply("session", "turn:form", "alice", accepted_form) == 0);
   assert(server_control_take(scope, "form", &id, &kind, &original, &decision) == 1);
   assert(cJSON_Compare(accepted_form, decision, 1));
   free(id);
   free(kind);
   cJSON_Delete(original);
   cJSON_Delete(decision);
   cJSON_Delete(form);
   cJSON_Delete(accepted_form);
   server_control_scope_close(scope);
   assert(!server_control_scope_current());
   cJSON_Delete(reply);
   reply = cJSON_Parse("{\"decision\":\"deny\"}");
   assert(server_control_reply("session", "turn:permission", "alice", reply) == 1);
   cJSON_Delete(reply);
   cJSON_Delete(request);
   puts("server controls: owner/turn/session binding, concurrent reply, consume-once and teardown "
        "passed");
   return 0;
}
