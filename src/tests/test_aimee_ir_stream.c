/* test_aimee_ir_stream.c -- Slice 4: OpenAI-chat SSE chunks -> IR deltas ->
 * Anthropic SSE, via the neutral delta model (no direct SSE->SSE translation). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/translation/aimee_ir_stream.h>
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

   /* --- OpenAI-chat reasoning -> THINKING deltas ------------------------------ */
   {
      openai_stream_state_t r;
      openai_stream_state_init(&r);
      aimee_delta_t d[8];

      /* reasoning_content (DeepSeek / vLLM / llama.cpp spelling) opens its OWN block,
       * distinct from the text block, so thought is never mistaken for answer. */
      cJSON *c1 = cJSON_Parse("{\"choices\":[{\"delta\":{\"reasoning_content\":\"we need\"}}]}");
      assert(c1);
      int n = openai_chunk_to_deltas(c1, &r, d, 8);
      assert(n == 3); /* TURN_START + BLOCK_START + BLOCK_DELTA */
      assert(d[1].type == AIMEE_DELTA_BLOCK_START && d[1].kind == AIMEE_BLK_THINKING);
      assert(d[2].type == AIMEE_DELTA_BLOCK_DELTA && d[2].kind == AIMEE_BLK_THINKING);
      assert(strcmp(d[2].text_delta, "we need") == 0);
      int think_block = d[1].block_id;
      cJSON_Delete(c1);

      /* content arriving closes the reasoning block before opening the text block */
      cJSON *c2 = cJSON_Parse("{\"choices\":[{\"delta\":{\"content\":\"Answer\"}}]}");
      assert(c2);
      n = openai_chunk_to_deltas(c2, &r, d, 8);
      assert(n == 3); /* BLOCK_STOP(thinking) + BLOCK_START(text) + BLOCK_DELTA(text) */
      assert(d[0].type == AIMEE_DELTA_BLOCK_STOP && d[0].kind == AIMEE_BLK_THINKING &&
             d[0].block_id == think_block);
      assert(d[1].type == AIMEE_DELTA_BLOCK_START && d[1].kind == AIMEE_BLK_TEXT);
      assert(d[1].block_id != think_block); /* a separate block, not a reused one */
      cJSON_Delete(c2);

      /* the OpenRouter spelling (`reasoning`) is accepted too */
      openai_stream_state_t r2;
      openai_stream_state_init(&r2);
      cJSON *c3 = cJSON_Parse("{\"choices\":[{\"delta\":{\"reasoning\":\"hmm\"}}]}");
      assert(c3);
      n = openai_chunk_to_deltas(c3, &r2, d, 8);
      assert(n == 3 && d[2].kind == AIMEE_BLK_THINKING && strcmp(d[2].text_delta, "hmm") == 0);
      cJSON_Delete(c3);

      /* an open reasoning block is closed by `finish` when no content ever arrives */
      cJSON *c4 = cJSON_Parse("{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}");
      assert(c4);
      n = openai_chunk_to_deltas(c4, &r2, d, 8);
      assert(n == 2 && d[0].type == AIMEE_DELTA_BLOCK_STOP && d[0].kind == AIMEE_BLK_THINKING);
      assert(d[1].type == AIMEE_DELTA_TURN_STOP);
      cJSON_Delete(c4);
   }

   /* --- Anthropic SSE -> IR deltas (the backend cell that was missing) --------- */
   {
      anthropic_backend_stream_state_t st;
      anthropic_backend_stream_state_init(&st);
      aimee_delta_t d[4];
      cJSON *p;
      int n;

#define A_FEED(ev, json)                                                                           \
   (p = cJSON_Parse(json), assert(p), n = anthropic_stream_to_deltas((ev), p, &st, d, 4))
#define A_DONE() cJSON_Delete(p)

      A_FEED("message_start",
             "{\"message\":{\"role\":\"assistant\",\"usage\":{\"input_tokens\":7}}}");
      assert(n == 1 && d[0].type == AIMEE_DELTA_TURN_START);
      A_DONE();

      /* ping is a keepalive, not a content event */
      A_FEED("ping", "{}");
      assert(n == 0);
      A_DONE();

      A_FEED("content_block_start",
             "{\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}");
      assert(n == 1 && d[0].type == AIMEE_DELTA_BLOCK_START && d[0].kind == AIMEE_BLK_THINKING &&
             d[0].block_id == 0);
      A_DONE();

      A_FEED("content_block_delta", "{\"index\":0,\"delta\":{\"type\":\"thinking_delta\","
                                    "\"thinking\":\"I need a test env\"}}");
      assert(n == 1 && d[0].kind == AIMEE_BLK_THINKING &&
             strcmp(d[0].text_delta, "I need a test env") == 0);
      A_DONE();

      /* signature_delta is DROPPED, openly (see the header's KNOWN GAPS) -- it must
       * neither surface as text nor drop the stream. */
      A_FEED("content_block_delta",
             "{\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"abc\"}}");
      assert(n == 0);
      A_DONE();

      A_FEED("content_block_stop", "{\"index\":0}");
      assert(n == 1 && d[0].type == AIMEE_DELTA_BLOCK_STOP && d[0].kind == AIMEE_BLK_THINKING);
      A_DONE();

      A_FEED("content_block_start",
             "{\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_9\","
             "\"name\":\"Bash\",\"input\":{}}}");
      assert(n == 1 && d[0].kind == AIMEE_BLK_TOOL_USE && strcmp(d[0].tool_id, "toolu_9") == 0 &&
             strcmp(d[0].tool_name, "Bash") == 0);
      A_DONE();

      /* partial_json is forwarded verbatim, NOT parsed */
      A_FEED("content_block_delta", "{\"index\":1,\"delta\":{\"type\":\"input_json_delta\","
                                    "\"partial_json\":\"{\\\"a\\\":\"}}");
      assert(n == 1 && strcmp(d[0].tool_args_delta, "{\"a\":") == 0);
      A_DONE();

      /* a future delta variant is ignored rather than dropping the stream */
      A_FEED("content_block_delta", "{\"index\":1,\"delta\":{\"type\":\"future_delta\"}}");
      assert(n == 0);
      A_DONE();

      A_FEED("message_delta",
             "{\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":42}}");
      assert(n == 0); /* stashed; message_stop carries the terminal delta */
      A_DONE();

      A_FEED("message_stop", "{}");
      assert(n == 1 && d[0].type == AIMEE_DELTA_TURN_STOP);
      assert(d[0].stop_reason == AIMEE_STOP_TOOL_USE);
      assert(d[0].usage_in == 7 && d[0].usage_out == 42);
      A_DONE();

      /* a second terminal is a protocol violation, not a no-op */
      A_FEED("message_stop", "{}");
      assert(n == -1);
      A_DONE();

      /* an unknown event type is forward-compat ignored */
      A_FEED("message_future", "{}");
      assert(n == 0);
      A_DONE();
   }

   /* --- Anthropic backend rejects malformed KNOWN events ---------------------- */
   {
      anthropic_backend_stream_state_t st;
      aimee_delta_t d[4];
      cJSON *p;
      int n;

      /* a delta with no preceding content_block_start has no kind to belong to */
      anthropic_backend_stream_state_init(&st);
      A_FEED("message_start", "{\"message\":{\"role\":\"assistant\"}}");
      assert(n == 1);
      A_DONE();
      A_FEED("content_block_delta",
             "{\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"x\"}}");
      assert(n == -1);
      A_DONE();

      /* an out-of-range index must not escape as an unbounded block_id */
      anthropic_backend_stream_state_init(&st);
      A_FEED("content_block_start", "{\"index\":100000,\"content_block\":{\"type\":\"text\"}}");
      assert(n == -1);
      A_DONE();

      /* a delta whose variant contradicts the block's declared kind */
      anthropic_backend_stream_state_init(&st);
      A_FEED("content_block_start", "{\"index\":0,\"content_block\":{\"type\":\"text\"}}");
      assert(n == 1);
      A_DONE();
      A_FEED("content_block_delta",
             "{\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"x\"}}");
      assert(n == -1);
      A_DONE();

      /* a KNOWN variant missing its own payload field */
      anthropic_backend_stream_state_init(&st);
      A_FEED("content_block_start", "{\"index\":0,\"content_block\":{\"type\":\"text\"}}");
      assert(n == 1);
      A_DONE();
      A_FEED("content_block_delta", "{\"index\":0,\"delta\":{\"type\":\"text_delta\"}}");
      assert(n == -1);
      A_DONE();

      /* a tool_use block with no id/name could never be answered */
      anthropic_backend_stream_state_init(&st);
      A_FEED("content_block_start", "{\"index\":0,\"content_block\":{\"type\":\"tool_use\"}}");
      assert(n == -1);
      A_DONE();

      /* message_start must be an assistant message, and must arrive once */
      anthropic_backend_stream_state_init(&st);
      A_FEED("message_start", "{\"message\":{\"role\":\"user\"}}");
      assert(n == -1);
      A_DONE();
      anthropic_backend_stream_state_init(&st);
      A_FEED("message_start", "{\"message\":{\"role\":\"assistant\"}}");
      assert(n == 1);
      A_DONE();
      A_FEED("message_start", "{\"message\":{\"role\":\"assistant\"}}");
      assert(n == -1);
      A_DONE();

      /* an error event surfaces the provider's message, and survives its absence */
      anthropic_backend_stream_state_init(&st);
      A_FEED("error", "{\"error\":{\"type\":\"overloaded_error\",\"message\":\"overloaded\"}}");
      assert(n == 1 && d[0].type == AIMEE_DELTA_ERROR &&
             strcmp(d[0].error_message, "overloaded") == 0);
      A_DONE();
      A_FEED("error", "{}");
      assert(n == 1 && d[0].type == AIMEE_DELTA_ERROR && d[0].error_message);
      A_DONE();
   }

   /* --- the point of the exercise: ONE provider-neutral rule reads thought from
    * both backends. No branch on provider, no branch on wire -- just
    * "BLOCK_DELTA where kind == AIMEE_BLK_THINKING". ---------------------------- */
   {
      char thought[256] = "";

      openai_stream_state_t o;
      openai_stream_state_init(&o);
      aimee_delta_t d[8];
      cJSON *c =
          cJSON_Parse("{\"choices\":[{\"delta\":{\"reasoning_content\":\"need a test env;\"}}]}");
      assert(c);
      int n = openai_chunk_to_deltas(c, &o, d, 8);
      for (int i = 0; i < n; i++)
         if (d[i].type == AIMEE_DELTA_BLOCK_DELTA && d[i].kind == AIMEE_BLK_THINKING)
            strncat(thought, d[i].text_delta, sizeof thought - strlen(thought) - 1);
      cJSON_Delete(c);

      anthropic_backend_stream_state_t st;
      anthropic_backend_stream_state_init(&st);
      cJSON *p;
      A_FEED("message_start", "{\"message\":{\"role\":\"assistant\"}}");
      A_DONE();
      A_FEED("content_block_start", "{\"index\":0,\"content_block\":{\"type\":\"thinking\"}}");
      A_DONE();
      A_FEED("content_block_delta",
             "{\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\" which one?\"}}");
      for (int i = 0; i < n; i++)
         if (d[i].type == AIMEE_DELTA_BLOCK_DELTA && d[i].kind == AIMEE_BLK_THINKING)
            strncat(thought, d[i].text_delta, sizeof thought - strlen(thought) - 1);
      A_DONE();

      assert(strcmp(thought, "need a test env; which one?") == 0);
#undef A_FEED
#undef A_DONE
   }

   printf("ok\n");
   return 0;
}
