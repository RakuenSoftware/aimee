/* Actual HTTP ingress request assembly cannot bypass canonical admission. */
#include "../server/openai_chat.c"
#include <assert.h>
void ir_seam_test_memory(int enabled);
int ir_seam_test_plan_calls(const char *phase);
static cJSON *forbidden_builder(const agent_t *agent, cJSON *messages, cJSON *tools,
                                const char *system, int tokens, double temperature)
{
   (void)agent;
   (void)messages;
   (void)tools;
   (void)system;
   (void)tokens;
   (void)temperature;
   assert(!"legacy request builder must never run");
   return NULL;
}
int main(void)
{
   agent_t agent = {0};
   snprintf(agent.model, sizeof agent.model, "fixture-model");
   cJSON *messages = cJSON_Parse("[{\"role\":\"user\",\"content\":\"hello\"}]");
   cJSON *tools = cJSON_Parse("[{\"type\":\"function\",\"function\":{\"name\":\"Read\","
                              "\"parameters\":{\"type\":\"object\"}}}]");
   const char *drivers[] = {"openai", "anthropic", "chatgpt"};
   setenv("AIMEE_IR_PATH", "0", 1);
   for (int preserve = 0; preserve <= 1; preserve++)
      for (size_t i = 0; i < sizeof drivers / sizeof drivers[0]; i++)
      {
         delegate_driver_t driver = {.name = drivers[i], .build_request = forbidden_builder};
         ir_seam_test_memory(1);
         int before = ir_seam_test_plan_calls("context");
         char *body =
             openai_build_body(&agent, &driver, messages, tools, "policy", 17, 0.2, preserve);
         assert(body);
         cJSON *request = cJSON_Parse(body);
         assert(request &&
                !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "model")), agent.model));
         assert(ir_seam_test_plan_calls("context") == before + !preserve);
         assert(cJSON_IsArray(cJSON_GetObjectItem(request, i == 2 ? "input" : "messages")));
         cJSON_Delete(request);
         free(body);
         /* An invalid tool schema is a refusal, never a driver fallback. */
         cJSON *bad = cJSON_Parse("[{\"type\":\"unsupported_tool\"}]");
         assert(!openai_build_body(&agent, &driver, messages, bad, "policy", 17, 0.2, preserve));
         cJSON_Delete(bad);
         bad = cJSON_Parse("[{\"role\":\"forged-role\",\"content\":\"hidden\"}]");
         assert(!openai_build_body(&agent, &driver, bad, tools, "policy", 17, 0.2, preserve));
         cJSON_Delete(bad);
         bad = cJSON_Parse("[{\"role\":\"user\",\"content\":[{\"type\":\"unknown-content\"}]}]");
         assert(!openai_build_body(&agent, &driver, bad, tools, "policy", 17, 0.2, preserve));
         cJSON_Delete(bad);
      }
   cJSON_Delete(messages);
   cJSON_Delete(tools);
   puts("OpenAI ingress canonical admission and independent host-edit policy passed");
   return 0;
}
