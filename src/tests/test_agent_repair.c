/* Core repair accepts canonical history only. Native migration is a separate
 * translation boundary, and a repair diagnostic never proves an effect outcome. */
#include "agent_protocol.h"
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void append(cJSON *history, const char *role, aimee_block_t *blocks, int count)
{
   aimee_message_t message = {.role = (char *)role, .blocks = blocks, .n_blocks = count};
   cJSON *json = aimee_ir_message_json(&message);
   assert(json && cJSON_AddItemToArray(history, json));
}
static void unchanged(const cJSON *before, cJSON *history, int expected)
{
   assert(message_history_repair(history) == expected);
   assert(cJSON_Compare(before, history, 1));
}
static void text_only(void)
{
   assert(message_history_repair(NULL) == 0);
   cJSON *history = cJSON_CreateArray();
   assert(message_history_repair(history) == 0);
   aimee_block_t text = {.type = AIMEE_BLK_TEXT, .text = "hello"};
   append(history, "system", &text, 1);
   append(history, "user", &text, 1);
   append(history, "assistant", &text, 1);
   cJSON *before = cJSON_Duplicate(history, 1);
   unchanged(before, history, 0);
   assert(!aimee_ir_history_admit(history));
   cJSON_Delete(before);
   cJSON_Delete(history);
}
static void cycles(void)
{
   cJSON *arguments = cJSON_Parse("{\"marker\":0.123456789012345678901234567890}");
   cJSON *output = cJSON_CreateString("actual failure");
   aimee_block_t calls[] = {{.type = AIMEE_BLK_TOOL_USE,
                             .tool_id = "reused",
                             .tool_name = "bash",
                             .tool_input = arguments},
                            {.type = AIMEE_BLK_TOOL_USE,
                             .tool_id = "second",
                             .tool_name = "read_file",
                             .tool_input = arguments}};
   aimee_block_t result = {.type = AIMEE_BLK_TOOL_RESULT,
                           .tool_id = "reused",
                           .tool_result = output,
                           .tool_is_error = 1};
   cJSON *history = cJSON_CreateArray();
   append(history, "assistant", calls, 1);
   append(history, "tool", &result, 1);
   cJSON *before = cJSON_Duplicate(history, 1);
   unchanged(before, history, 0);
   assert(!aimee_ir_history_admit(history));
   cJSON_Delete(before);
   /* The earlier receipt does not resolve a later call with the same ID. */
   append(history, "assistant", calls, 2);
   assert(message_history_repair(history) == 2);
   assert(cJSON_GetArraySize(history) == 5);
   assert(aimee_ir_history_admit(history) < 0);
   before = cJSON_Duplicate(history, 1);
   unchanged(before, history, 0);
   const cJSON *block = cJSON_GetArrayItem(
       cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(history, 3), "blocks"), 0);
   const cJSON *diagnostic = cJSON_GetObjectItemCaseSensitive(block, "content");
   assert(cJSON_IsString(diagnostic) && strstr(diagnostic->valuestring, "outcome is unknown"));
   const cJSON *context = cJSON_GetObjectItemCaseSensitive(block, "context");
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(context, "revision_domain")),
               "history-repair"));
   char *wire = cJSON_PrintUnformatted(history);
   assert(wire && strstr(wire, "0.123456789012345678901234567890"));
   free(wire);
   cJSON_Delete(before);
   cJSON_Delete(history);
   /* An orphaned result is removed, while a matched result and signed reasoning
    * remain exact. Orphan removal is ordered rather than a global ID-set test. */
   history = cJSON_CreateArray();
   append(history, "tool", &result, 1);
   aimee_block_t thought = {
       .type = AIMEE_BLK_THINKING, .text = "reason", .thinking_signature = "signed"};
   append(history, "assistant", &thought, 1);
   append(history, "assistant", calls, 1);
   append(history, "tool", &result, 1);
   assert(message_history_repair(history) == 1);
   assert(cJSON_GetArraySize(history) == 3);
   assert(!aimee_ir_history_admit(history));
   before = cJSON_Duplicate(history, 1);
   unchanged(before, history, 0);
   cJSON_Delete(before);
   cJSON_Delete(history);
   cJSON_Delete(arguments);
   cJSON_Delete(output);
}
static void native_and_malformed_refuse(void)
{
   const char *native[] = {
       "[{\"role\":\"user\",\"content\":\"hello\"}]",
       "[{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"call\",\"type\":\"function\","
       "\"function\":{\"name\":\"bash\",\"arguments\":\"{}\"}}]}]",
       "[{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"call\",\"name\":"
       "\"bash\",\"input\":{}}]}]",
       "[{\"type\":\"function_call\",\"call_id\":\"call\",\"name\":\"bash\",\"arguments\":\"{}\"}]",
       "[{\"role\":\"assistant\",\"blocks\":[{\"type\":\"tool_use\",\"tool_id\":\"call\",\"tool_"
       "name\":\"bash\",\"tool_input\":\"invalid\"}]}]",
       "{}"};
   for (size_t i = 0; i < sizeof native / sizeof native[0]; i++)
   {
      cJSON *history = cJSON_Parse(native[i]);
      cJSON *before = cJSON_Duplicate(history, 1);
      unchanged(before, history, -1);
      cJSON_Delete(before);
      if (cJSON_IsArray(history))
      {
         aimee_block_t text = {.type = AIMEE_BLK_TEXT, .text = "canonical first"};
         aimee_message_t message = {.role = "user", .blocks = &text, .n_blocks = 1};
         assert(cJSON_InsertItemInArray(history, 0, aimee_ir_message_json(&message)));
         before = cJSON_Duplicate(history, 1);
         unchanged(before, history, -1);
         cJSON_Delete(before);
      }
      cJSON_Delete(history);
   }
}
int main(void)
{
   text_only();
   cycles();
   native_and_malformed_refuse();
   puts("agent_repair: canonical causal repair, exact arguments, diagnostics and native refusal "
        "pass");
   return 0;
}
