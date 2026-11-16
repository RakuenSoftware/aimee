/* MiniMax's strict arguments contract is satisfied by typed egress, never by
 * rewriting malformed persisted native arguments to an invented empty object. */
#include <aimee/translation/aimee_frontend.h>
#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
   const char *arguments[] = {NULL, "{}", "\"{\\\"city\\\":\\\"Paris\\\"\"", "\"{} {}\""};
   for (size_t i = 0; i < sizeof arguments / sizeof arguments[0]; i++)
   {
      cJSON *request = cJSON_Parse(
          "{\"model\":\"MiniMax-M2.7\",\"messages\":[{\"role\":\"assistant\",\"tool_calls\":[{"
          "\"id\":\"call\",\"type\":\"function\",\"function\":{\"name\":\"weather\"}}]}]}");
      cJSON *messages = cJSON_GetObjectItemCaseSensitive(request, "messages");
      cJSON *function = cJSON_GetObjectItemCaseSensitive(
          cJSON_GetArrayItem(
              cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 0), "tool_calls"), 0),
          "function");
      if (arguments[i])
         assert(cJSON_AddItemToObject(function, "arguments", cJSON_Parse(arguments[i])));
      cJSON *before = cJSON_Duplicate(request, 1);
      aimee_request_t ir = {0};
      char error[256];
      assert(openai_frontend_parse(request, &ir, error, sizeof error) < 0);
      assert(cJSON_Compare(request, before, 1));
      aimee_request_free(&ir);
      cJSON_Delete(before);
      cJSON_Delete(request);
   }
   const char *exact = "{\"city\":\"Paris\",\"marker\":0.123456789012345678901234567890}";
   cJSON *input = cJSON_Parse(exact);
   aimee_block_t call = {
       .type = AIMEE_BLK_TOOL_USE, .tool_id = "call", .tool_name = "weather", .tool_input = input};
   aimee_message_t message = {.role = "assistant", .blocks = &call, .n_blocks = 1};
   aimee_request_t request = {.model = "MiniMax-M2.7", .messages = &message, .n_messages = 1};
   cJSON *wire = openai_backend_build(&request);
   assert(wire);
   cJSON *messages = cJSON_GetObjectItemCaseSensitive(wire, "messages");
   cJSON *calls = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 0), "tool_calls");
   cJSON *function = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(calls, 0), "function");
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(function, "arguments")),
                  exact));
   cJSON_Delete(wire);
   cJSON_Delete(input);
   puts("minimax_tool_call_args: malformed native history refuses; typed egress retains exact "
        "arguments");
   return 0;
}
