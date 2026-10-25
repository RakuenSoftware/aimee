#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *parse(const char *text, size_t unused)
{
   (void)unused;
   return aimee_ir_json_parse_frame(text, strlen(text));
}
static cJSON *fixture(void)
{
   return parse(
       "{\"version\":1,\"model\":\"selected\",\"source_wire\":\"anthropic\",\"cache_policy\":"
       "\"stable_prefix\","
       "\"system\":[{\"type\":\"text\",\"text\":\"system\"}],"
       "\"messages\":[{\"role\":\"user\",\"content_scalar\":true,\"blocks\":[{\"type\":\"text\","
       "\"text\":\"question\"}]},"
       "{\"role\":\"assistant\",\"blocks\":[{\"type\":\"thinking\",\"text\":\"retained\","
       "\"signature\":\"opaque\"},"
       "{\"type\":\"tool_use\",\"tool_id\":\"call\",\"name\":\"lookup\",\"input\":{\"n\":"
       "9007199254740993}}]},"
       "{\"role\":\"tool\",\"blocks\":[{\"type\":\"tool_result\",\"tool_id\":\"call\",\"content\":{"
       "\"ok\":true},\"is_error\":false}]}],"
       "\"tools\":[{\"kind\":\"function\",\"name\":\"lookup\",\"strict\":\"true\",\"defer_"
       "loading\":false,\"schema\":{\"type\":\"object\"}},"
       "{\"kind\":\"namespace\",\"name\":\"ns\",\"children\":[{\"kind\":\"function\",\"name\":"
       "\"child\"}]},"
       "{\"kind\":\"hosted\",\"name\":\"search\",\"provider\":\"anthropic\",\"native_type\":\"web_"
       "search\",\"native_options\":{\"limit\":7}}],"
       "\"tool_choice\":{\"mode\":\"tool\",\"name\":\"lookup\"},\"parallel_tool_calls\":false,"
       "\"max_tokens\":2048,\"temperature\":0.5,\"top_p\":0.9,\"top_k\":40,"
       "\"stream\":true,\"stop_sequences\":[\"stop\"],\"metadata\":{\"opaque\":9007199254740993},"
       "\"service_tier\":\"auto\","
       "\"prompt_cache_key\":\"\",\"prompt_cache_retention\":\"24h\",\"prompt_cache_options\":{"
       "\"ttl\":3600},"
       "\"reasoning_effort\":\"high\",\"reasoning_summary\":\"concise\",\"provider_config\":{"
       "\"wire\":\"anthropic\",\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":1024}},"
       "\"output_format\":{\"kind\":\"json_schema\",\"name\":\"artifact\",\"description\":"
       "\"domain\",\"strict\":\"true\",\"schema\":{\"type\":\"object\"}}}",
       0);
}
static void roundtrip(void)
{
   cJSON *input = fixture();
   assert(input);
   aimee_request_t first = {0}, second = {0};
   assert(!aimee_ir_request_parse(input, &first));
   assert(first.n_messages == 3 && first.n_tools == 3 && first.n_system == 1);
   first.messages[0].compaction_boundary = 1;
   aimee_request_t clone = {0};
   assert(!aimee_ir_request_clone(&first, &clone) && clone.messages[0].compaction_boundary);
   aimee_request_free(&clone);
   assert(first.messages[0].content_scalar && first.has_parallel_tool_calls &&
          !first.parallel_tool_calls);
   assert(first.tools[1].n_children == 1 && first.tools[2].provider_wire == AIMEE_WIRE_ANTHROPIC);
   assert(first.has_max_tokens && first.max_tokens == 2048 && first.thinking && first.metadata);
   assert(!first.raw && !first.raw_bytes && !first.raw_length);
   cJSON_Delete(input); /* Every decoded allocation is owned independently. */
   cJSON *encoded = aimee_ir_request_json(&first);
   assert(encoded && !aimee_ir_request_parse(encoded, &second));
   assert(second.messages[0].compaction_boundary);
   cJSON *again = aimee_ir_request_json(&second);
   char *a = cJSON_PrintUnformatted(encoded), *b = cJSON_PrintUnformatted(again);
   assert(a && b && !strcmp(a, b) && strstr(a, "9007199254740993"));
   cJSON_free(a);
   cJSON_free(b);
   cJSON_Delete(encoded);
   cJSON_Delete(again);
   aimee_request_free(&first);
   aimee_request_free(&second);
}
static void unchanged(cJSON *input)
{
   aimee_request_t out = {.model = strdup("preserved")};
   char *identity = out.model;
   assert(aimee_ir_request_parse(input, &out) == -1);
   assert(out.model == identity && !strcmp(out.model, "preserved") && !out.n_messages);
   aimee_request_free(&out);
   cJSON_Delete(input);
}
static void negative(void)
{
   const char *required[] = {"version", "source_wire", "cache_policy", "system",        "messages",
                             "tools",   "tool_choice", "stream",       "stop_sequences"};
   for (size_t i = 0; i < sizeof required / sizeof *required; i++)
   {
      cJSON *j = fixture();
      cJSON_DeleteItemFromObjectCaseSensitive(j, required[i]);
      unchanged(j);
   }
   cJSON *j = fixture();
   cJSON_AddStringToObject(j, "raw", "not authority");
   unchanged(j);
   j = fixture();
   cJSON_AddNumberToObject(j, "version", 1);
   unchanged(j); /* duplicate */
   j = fixture();
   cJSON_ReplaceItemInObjectCaseSensitive(j, "max_tokens", cJSON_CreateNumber(1.5));
   unchanged(j);
   j = fixture();
   cJSON_ReplaceItemInObjectCaseSensitive(j, "max_tokens", cJSON_CreateNumber(2147483648.0));
   unchanged(j);
   j = fixture();
   cJSON_ReplaceItemInObjectCaseSensitive(j, "parallel_tool_calls", cJSON_CreateNumber(1));
   unchanged(j);
   j = fixture();
   cJSON_AddStringToObject(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "system"), 0),
                           "tool_id", "silently lost");
   unchanged(j);
   j = fixture();
   cJSON_AddStringToObject(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "messages"), 0),
                           "raw", "no native sidecar");
   unchanged(j);
   j = fixture();
   cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(j, "tool_choice"), "unexpected",
                           "lost");
   unchanged(j);
   j = fixture();
   cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "provider_config"),
                                          "wire", cJSON_CreateString("responses"));
   unchanged(j);
   j = fixture();
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(j, "tools");
   while (cJSON_GetArraySize(tools) <= AIMEE_IR_MAX_BLOCKS)
      cJSON_AddItemToArray(tools, cJSON_Duplicate(cJSON_GetArrayItem(tools, 0), 1));
   unchanged(j);
}
int main(void)
{
   roundtrip();
   negative();
   puts("IR request codec: PASS");
   return 0;
}
