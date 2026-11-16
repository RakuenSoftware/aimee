/* Exercise the production canonical client output boundary after governance.
 * The old flat response emitters no longer exist. */
#include <aimee/translation/aimee_frontend.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   cJSON *events;
   int created;
} capture_t;
static void capture(void *context, const char *event, const char *json)
{
   capture_t *out = context;
   if (event && !strcmp(event, "response.created"))
      out->created++;
   cJSON *value = cJSON_Parse(json);
   assert(value && cJSON_AddItemToArray(out->events, value));
}
static void chat_capture(void *context, const char *json)
{
   capture(context, NULL, json);
}
static const cJSON *field(const cJSON *object, const char *name)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
   assert(value);
   return value;
}
static const char *string(const cJSON *object, const char *name)
{
   const char *value = cJSON_GetStringValue(field(object, name));
   assert(value);
   return value;
}
static aimee_response_t response(int tools)
{
   aimee_response_t result = {.n_content = tools + 1,
                              .stop_reason = AIMEE_STOP_TOOL_USE,
                              .usage_in = 17,
                              .usage_out = 23,
                              .usage_cache_read = 11,
                              .usage_reasoning = 7};
   result.content = calloc((size_t)result.n_content, sizeof *result.content);
   assert(result.content);
   result.content[0] = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup("Let me check.\n雪")};
   for (int i = 0; i < tools; i++)
   {
      char id[512];
      memset(id, 'q', 400);
      snprintf(id + 400, sizeof id - 400, "-%d", i);
      result.content[i + 1] = (aimee_block_t){
          .type = AIMEE_BLK_TOOL_USE,
          .tool_id = strdup(id),
          .tool_name = strdup("query"),
          .tool_namespace = strdup(i % 2 ? "mcp__personal" : "mcp__work"),
          .tool_input =
              cJSON_Parse("{\"query\":\"quoted \\\"text\\\"\\n雪\",\"exact\":9007199254740993}")};
      assert(result.content[i + 1].tool_input);
   }
   return result;
}
static void test_responses(int tools, aimee_stop_reason_t stop)
{
   aimee_response_t reply = response(tools);
   reply.stop_reason = stop;
   cJSON *buffered = aimee_openai_render_response(&reply, "resp-owned", "actual-model", 123, 1);
   assert(buffered);
   capture_t out = {.events = cJSON_CreateArray()};
   assert(
       !aimee_openai_emit_responses_tail(&reply, "resp-owned", "actual-model", 123, capture, &out));
   assert(!out.created);
   const cJSON *terminal = cJSON_GetArrayItem(out.events, cJSON_GetArraySize(out.events) - 1);
   assert(!strcmp(string(terminal, "type"),
                  stop == AIMEE_STOP_MAX_TOKENS ? "response.incomplete" : "response.completed"));
   assert(cJSON_Compare(field(terminal, "response"), buffered, 1));
   const cJSON *event;
   int started = 0, finished = 0, arguments = 0;
   cJSON_ArrayForEach(event, out.events)
   {
      const char *type = string(event, "type");
      if (!strcmp(type, "response.output_item.added"))
         started++;
      if (!strcmp(type, "response.output_item.done"))
         finished++;
      if (!strcmp(type, "response.function_call_arguments.done"))
         arguments++;
   }
   assert(started == tools + 1 && finished == tools + 1 && arguments == tools);
   const cJSON *items = field(buffered, "output");
   assert(cJSON_GetArraySize(items) == tools + 1);
   for (int i = 0; i < tools; i++)
   {
      const cJSON *item = cJSON_GetArrayItem(items, i + 1);
      assert(!strcmp(string(item, "call_id"), reply.content[i + 1].tool_id));
      assert(!strcmp(string(item, "namespace"), reply.content[i + 1].tool_namespace));
      assert(strstr(string(item, "arguments"), "9007199254740993"));
   }
   assert(
       field(field(field(buffered, "usage"), "input_tokens_details"), "cached_tokens")->valueint ==
       11);
   assert(reply.n_content == tools + 1 && reply.stop_reason == stop);
   cJSON_Delete(out.events);
   cJSON_Delete(buffered);
   aimee_response_free(&reply);
}
static void test_chat(void)
{
   aimee_response_t reply = response(40);
   capture_t out = {.events = cJSON_CreateArray()};
   assert(!aimee_openai_emit_chat_response(&reply, "chat-owned", "actual-model", 123, chat_capture,
                                           &out));
   assert(cJSON_GetArraySize(out.events) == 2);
   const cJSON *first = cJSON_GetArrayItem(out.events, 0),
               *last = cJSON_GetArrayItem(out.events, 1);
   const cJSON *choice = cJSON_GetArrayItem(field(first, "choices"), 0);
   assert(cJSON_IsNull(field(choice, "finish_reason")));
   const cJSON *delta = field(choice, "delta");
   assert(!strcmp(string(delta, "content"), reply.content[0].text));
   const cJSON *calls = field(delta, "tool_calls");
   assert(cJSON_GetArraySize(calls) == 40);
   for (int i = 0; i < 40; i++)
   {
      const cJSON *call = cJSON_GetArrayItem(calls, i);
      assert(field(call, "index")->valueint == i);
      assert(!strcmp(string(call, "id"), reply.content[i + 1].tool_id));
      assert(!strcmp(string(call, "namespace"), reply.content[i + 1].tool_namespace));
      assert(strstr(string(field(call, "function"), "arguments"), "9007199254740993"));
   }
   assert(!strcmp(string(cJSON_GetArrayItem(field(last, "choices"), 0), "finish_reason"),
                  "tool_calls"));
   assert(field(field(field(last, "usage"), "prompt_tokens_details"), "cached_tokens")->valueint ==
          11);
   char tiny[8];
   assert(aimee_openai_buffered_response(&reply, "id", "model", 0, 0, tiny, sizeof tiny) == -1);
   assert(aimee_openai_emit_chat_response(&reply, "id", "model", 0, NULL, NULL) == -1);
   cJSON_Delete(out.events);
   aimee_response_free(&reply);
}
int main(void)
{
   test_responses(0, AIMEE_STOP_END_TURN);
   test_responses(2, AIMEE_STOP_TOOL_USE);
   test_responses(40, AIMEE_STOP_MAX_TOKENS);
   test_chat();
   puts("canonical client output: ordered tools/text, namespaces, full IDs, usage and truncation "
        "passed");
   return 0;
}
