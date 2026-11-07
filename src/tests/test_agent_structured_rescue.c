#include "aimee.h"
#include "agent_protocol.h"
#include <aimee/ir/session_event.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "support/rescue_fixture_provider.h"

int main(void)
{
   delegate_register_rescue_provider(rescue_fixture_provider);
   const char *texts[] = {
       "{\"name\":\"bash\",\"arguments\":{\"command\":\"safe\"}}",
       "<tool_call><name>bash</name><arguments>{\"command\":\"safe\"}</arguments></tool_call>"};
   for (int wire = 0; wire < 3; wire++)
      for (int shape = 0; shape < 2; shape++)
         for (int enabled = 0; enabled < 2; enabled++)
         {
            cJSON *root = cJSON_Parse(
                wire == 0 ? "{\"choices\":[{\"message\":{\"role\":\"assistant\"},\"finish_reason\":"
                            "\"stop\"}]}"
                : wire == 1 ? "{\"id\":\"r\",\"object\":\"response\",\"status\":\"completed\","
                              "\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
                              "\"content\":[{\"type\":\"output_text\"}]}]}"
                            : "{\"id\":\"m\",\"type\":\"message\",\"role\":\"assistant\","
                              "\"content\":[{\"type\":\"text\"}],\"stop_reason\":\"end_turn\","
                              "\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}");
            assert(root);
            cJSON *dest =
                wire == 0
                    ? cJSON_GetObjectItemCaseSensitive(
                          cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "choices"), 0),
                          "message")
                : wire == 1
                    ? cJSON_GetArrayItem(
                          cJSON_GetObjectItemCaseSensitive(
                              cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "output"),
                                                 0),
                              "content"),
                          0)
                    : cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "content"), 0);
            assert(cJSON_AddStringToObject(dest, wire == 0 ? "content" : "text", texts[shape]));
            aimee_response_t response = {0};
            int rescued = -1, rc;
            aimee_ir_session_set_sink(NULL, NULL);
            if (wire == 1)
            {
               char *raw = cJSON_PrintUnformatted(root);
               assert(raw);
               rc = agent_ir_decode_responses(raw, enabled ? 1 : -1, &rescued, &response);
               free(raw);
            }
            else
               rc = agent_ir_decode_json_response_bound(root, wire == 2, enabled ? 1 : -1, &rescued,
                                                        NULL, &response);
            assert(!rc);
            /* XML fixture is an existing caller-boundary seam, not a second JSON parser. */
            if (shape && enabled)
            {
               assert(rescued == 1 && aimee_ir_response_has_tool_use(&response));
            }
            else
            {
               assert(!rescued && !aimee_ir_response_has_tool_use(&response));
               assert(response.n_content == 1 && response.content[0].type == AIMEE_BLK_TEXT &&
                      !strcmp(response.content[0].text, texts[shape]));
            }
            aimee_response_free(&response);
            cJSON_Delete(root);
         }
   for (int wire = 0; wire < 3; wire++)
      for (int enabled = 0; enabled < 2; enabled++)
      {
         const char *raw =
             wire == 0 ? "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
                         "\"tool_calls\":[{\"id\":\"n1\",\"type\":\"function\",\"function\":{"
                         "\"name\":\"bash\",\"arguments\":\"{\\\"command\\\":\\\"safe\\\"}\"}}]},"
                         "\"finish_reason\":\"tool_calls\"}]}"
             : wire == 1 ? "{\"id\":\"r\",\"object\":\"response\",\"status\":\"completed\","
                           "\"output\":[{\"type\":\"function_call\",\"call_id\":\"n1\",\"name\":"
                           "\"bash\",\"arguments\":\"{\\\"command\\\":\\\"safe\\\"}\"}]}"
                         : "{\"id\":\"m\",\"type\":\"message\",\"role\":\"assistant\",\"content\":["
                           "{\"type\":\"tool_use\",\"id\":\"n1\",\"name\":\"bash\",\"input\":{"
                           "\"command\":\"safe\"}}],\"stop_reason\":\"tool_use\",\"usage\":{"
                           "\"input_tokens\":1,\"output_tokens\":1}}";
         aimee_response_t response = {0};
         int rescued = -1, rc;
         cJSON *root = cJSON_Parse(raw);
         assert(root);
         aimee_ir_session_set_sink(NULL, NULL);
         if (wire == 1)
            rc = agent_ir_decode_responses(raw, enabled ? 1 : -1, &rescued, &response);
         else
            rc = agent_ir_decode_json_response_bound(root, wire == 2, enabled ? 1 : -1, &rescued,
                                                     NULL, &response);
         assert(!rc && !rescued && aimee_ir_response_tool_count(&response) == 1);
         const aimee_block_t *call = aimee_ir_response_tool(&response, 0);
         assert(!strcmp(call->tool_name, "bash") && !strcmp(call->tool_id, "n1"));
         assert(!strcmp(
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call->tool_input, "command")),
             "safe"));
         aimee_response_free(&response);
         cJSON_Delete(root);
      }
   puts("PASS actual Chat/Responses/Anthropic structured text preservation, legacy XML rescue and "
        "native tool-use (18 cases)");
   return 0;
}
