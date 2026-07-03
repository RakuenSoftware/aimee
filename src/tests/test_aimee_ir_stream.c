/* test_aimee_ir_stream.c -- Slice 4: OpenAI-chat SSE chunks -> IR deltas ->
 * Anthropic SSE, via the neutral delta model (no direct SSE->SSE translation). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee_ir_stream.h"
#include "cJSON.h"

/* accumulate the Anthropic SSE rendered from one OpenAI chunk */
static void feed(const char *chunk_json, openai_stream_state_t *ost, anthropic_stream_state_t *ast,
                 char *acc, size_t accn)
{
   cJSON *chunk = cJSON_Parse(chunk_json);
   assert(chunk);
   aimee_delta_t deltas[16];
   int n = openai_chunk_to_deltas(chunk, ost, deltas, 16);
   for (int i = 0; i < n; i++)
   {
      char *sse = anthropic_delta_render(&deltas[i], ast, "msg_1", "claude-3-5-sonnet");
      if (sse)
      {
         strncat(acc, sse, accn - strlen(acc) - 1);
         free(sse);
      }
   }
   cJSON_Delete(chunk);
}

/* Collector for the callback-emit path: reframe (event,data) back to SSE so the
 * SAME assertions apply -- proving anthropic_delta_emit == anthropic_delta_render. */
static char g_emit_acc[4096];
static void emit_collect(void *ctx, const char *event, const char *data_json)
{
   (void)ctx;
   char frame[1024];
   snprintf(frame, sizeof frame, "event: %s\ndata: %s\n\n", event, data_json);
   strncat(g_emit_acc, frame, sizeof g_emit_acc - strlen(g_emit_acc) - 1);
}
static void feed_emit(const char *chunk_json, openai_stream_state_t *ost,
                      anthropic_stream_state_t *ast)
{
   cJSON *chunk = cJSON_Parse(chunk_json);
   assert(chunk);
   aimee_delta_t deltas[16];
   int n = openai_chunk_to_deltas(chunk, ost, deltas, 16);
   for (int i = 0; i < n; i++)
      anthropic_delta_emit(&deltas[i], ast, "msg_1", "claude-3-5-sonnet", emit_collect, NULL);
   cJSON_Delete(chunk);
}

int main(void)
{
   printf("ir-stream: ");
   openai_stream_state_t ost;
   openai_stream_state_init(&ost);
   anthropic_stream_state_t ast = {0};
   char acc[4096] = "";

   /* text streaming */
   feed("{\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Hel\"}}]}", &ost, &ast, acc,
        sizeof acc);
   feed("{\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}", &ost, &ast, acc, sizeof acc);
   /* a tool call, id+name then streamed arguments */
   feed("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
        "\"function\":{\"name\":\"Read\",\"arguments\":\"\"}}]}}]}",
        &ost, &ast, acc, sizeof acc);
   feed("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"{\\\"p\\\":1}\"}}]}}]}",
        &ost, &ast, acc, sizeof acc);
   /* finish */
   feed("{\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":4}}",
        &ost, &ast, acc, sizeof acc);

   /* the accumulated Anthropic SSE must be well-formed + carry the content */
   assert(strstr(acc, "event: message_start"));
   assert(strstr(acc, "\"type\":\"content_block_start\"") && strstr(acc, "\"type\":\"text\""));
   assert(strstr(acc, "\"type\":\"text_delta\",\"text\":\"Hel\""));
   assert(strstr(acc, "\"type\":\"text_delta\",\"text\":\"lo\""));
   assert(strstr(acc, "\"type\":\"tool_use\"") && strstr(acc, "\"id\":\"call_1\"") &&
          strstr(acc, "\"name\":\"Read\""));
   assert(strstr(acc, "\"type\":\"input_json_delta\"") && strstr(acc, "\\\"p\\\":1"));
   assert(strstr(acc, "\"type\":\"content_block_stop\""));
   assert(strstr(acc, "\"stop_reason\":\"tool_use\""));
   assert(strstr(acc, "event: message_stop"));
   /* exactly one message_start event (not re-emitted per chunk) */
   char *ms = strstr(acc, "event: message_start");
   assert(ms && !strstr(ms + 1, "event: message_start"));

   /* The callback-emit path (anthropic_delta_emit, the live-relay sink) must
    * produce byte-identical framed SSE to anthropic_delta_render (shared builder). */
   openai_stream_state_t ost2;
   openai_stream_state_init(&ost2);
   anthropic_stream_state_t ast2 = {0};
   g_emit_acc[0] = '\0';
   feed_emit("{\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Hel\"}}]}", &ost2,
             &ast2);
   feed_emit("{\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}", &ost2, &ast2);
   feed_emit("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
             "\"function\":{\"name\":\"Read\",\"arguments\":\"\"}}]}}]}",
             &ost2, &ast2);
   feed_emit("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
             "\"function\":{\"arguments\":\"{\\\"p\\\":1}\"}}]}}]}",
             &ost2, &ast2);
   feed_emit("{\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}],"
             "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":4}}",
             &ost2, &ast2);
   assert(strcmp(acc, g_emit_acc) == 0); /* emit == render, event-for-event */

   printf("ok\n");
   return 0;
}
