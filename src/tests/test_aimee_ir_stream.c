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

static void test_unobserved_stream_usage(void)
{
   for (int observed = 0; observed < 2; observed++)
   {
      openai_stream_state_t state;
      openai_stream_state_init(&state);
      aimee_delta_t deltas[8];
      cJSON *chunk =
          cJSON_Parse(observed ? "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],"
                                 "\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0}}"
                               : "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}");
      int count = openai_chunk_to_deltas(chunk, &state, deltas, 8);
      assert(count >= 0);
      int found = 0;
      for (int i = 0; i < count; i++)
         if (deltas[i].type == AIMEE_DELTA_TURN_STOP)
         {
            assert(deltas[i].usage_unobserved == !observed);
            found = 1;
         }
      if (!found)
      {
         assert(openai_stream_finish(&state, deltas, 8) == 1);
         assert(deltas[0].usage_unobserved == !observed);
      }
      cJSON_Delete(chunk);
   }
   const char *partial[] = {"null", "{}", "{\"prompt_tokens\":7}", "{\"completion_tokens\":3}"};
   for (size_t i = 0; i < sizeof partial / sizeof *partial; i++)
   {
      openai_stream_state_t state;
      openai_stream_state_init(&state);
      cJSON *chunk = cJSON_Parse("{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}");
      assert(cJSON_AddItemToObject(chunk, "usage", cJSON_Parse(partial[i])));
      aimee_delta_t deltas[8];
      assert(openai_chunk_to_deltas(chunk, &state, deltas, 8) >= 0);
      assert(openai_stream_finish(&state, deltas, 8) == 1);
      assert(deltas[0].usage_unobserved && !deltas[0].usage_in && !deltas[0].usage_out);
      cJSON_Delete(chunk);
   }
   {
      openai_stream_state_t state;
      openai_stream_state_init(&state);
      cJSON *bad = cJSON_Parse("{\"usage\":\"not counters\",\"choices\":[]}");
      aimee_delta_t deltas[8];
      assert(openai_chunk_to_deltas(bad, &state, deltas, 8) < 0);
      cJSON_Delete(bad);
   }
   anthropic_backend_stream_state_t state;
   anthropic_backend_stream_state_init(&state);
   const char *events[] = {"message_start", "message_delta", "message_stop"};
   const char *frames[] = {"{\"message\":{\"role\":\"assistant\",\"usage\":{\"input_tokens\":7}}}",
                           "{\"delta\":{\"stop_reason\":\"end_turn\"}}", "{}"};
   aimee_delta_t delta[2];
   for (int i = 0; i < 3; i++)
   {
      cJSON *frame = cJSON_Parse(frames[i]);
      int count = anthropic_stream_to_deltas(events[i], frame, &state, delta, 2);
      assert(count >= 0);
      if (i == 2)
         assert(count == 1 && delta[0].usage_unobserved && !delta[0].usage_in &&
                !delta[0].usage_out);
      cJSON_Delete(frame);
   }
   /* Initial output is a progress observation, never a final total. */
   for (int final_observed = 0; final_observed <= 1; final_observed++)
   {
      anthropic_backend_stream_state_t source;
      anthropic_backend_stream_state_init(&source);
      cJSON *frame = cJSON_Parse("{\"message\":{\"role\":\"assistant\",\"usage\":{\"input_tokens\":"
                                 "20,\"output_tokens\":3}}}");
      aimee_delta_t result[2];
      assert(anthropic_stream_to_deltas("message_start", frame, &source, result, 2) == 1);
      assert(!result[0].usage_unobserved && result[0].usage_out == 3);
      cJSON_Delete(frame);
      frame = cJSON_Parse(
          final_observed
              ? "{\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":0}}"
              : "{\"delta\":{\"stop_reason\":\"end_turn\"}}");
      assert(anthropic_stream_to_deltas("message_delta", frame, &source, result, 2) >= 0);
      cJSON_Delete(frame);
      frame = cJSON_Parse("{}");
      assert(anthropic_stream_to_deltas("message_stop", frame, &source, result, 2) == 1);
      assert(result[0].usage_unobserved == !final_observed && !result[0].usage_out);
      assert(result[0].usage_in == (final_observed ? 20 : 0));
      cJSON_Delete(frame);
   }
   anthropic_stream_state_t client = {0};
   assert(!anthropic_delta_render(delta, &client, "id", "model"));
   assert(anthropic_delta_emit(delta, &client, "id", "model", NULL, NULL) < 0);
}

int main(void)
{
   test_unobserved_stream_usage();
   /* A role-only Chat opening cannot claim observed input zero on Anthropic. */
   for (int observed = 0; observed <= 1; observed++)
   {
      openai_stream_state_t source;
      openai_stream_state_init(&source);
      cJSON *chunk =
          cJSON_Parse(observed ? "{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}],\"usage\":{"
                                 "\"prompt_tokens\":0,\"completion_tokens\":0}}"
                               : "{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}");
      aimee_delta_t opening[4];
      assert(openai_chunk_to_deltas(chunk, &source, opening, 4) == 1);
      assert(opening[0].usage_unobserved == !observed);
      anthropic_stream_state_t frontend = {0};
      char *frame = anthropic_delta_render(opening, &frontend, "id", "model");
      assert((frame != NULL) == observed && frontend.started == observed);
      free(frame);
      g_emit_acc[0] = 0;
      assert((anthropic_delta_emit(opening, &frontend, "id", "model", emit_collect, NULL) > 0) ==
             observed);
      assert((g_emit_acc[0] != 0) == observed);
      cJSON_Delete(chunk);
   }
   printf("ir-stream: ");
   openai_stream_state_t ost;
   openai_stream_state_init(&ost);
   anthropic_stream_state_t ast = {0};
   char acc[4096] = "";

   /* text streaming */
   feed("{\"choices\":[{\"delta\":{"
        "\"role\":\"assistant\",\"content\":\"Hel\"}}]}",
        &ost, &ast, acc, sizeof acc);
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

   /* A Chat role-only opening has unknown usage and cannot render message_start.
    * Standalone block/terminal render cells still retain their exact content. */
   assert(!strstr(acc, "event: message_start"));
   assert(strstr(acc, "\"type\":\"content_block_start\"") && strstr(acc, "\"type\":\"text\""));
   assert(strstr(acc, "\"type\":\"text_delta\",\"text\":\"Hel\""));
   assert(strstr(acc, "\"type\":\"text_delta\",\"text\":\"lo\""));
   assert(strstr(acc, "\"type\":\"tool_use\"") && strstr(acc, "\"id\":\"call_1\"") &&
          strstr(acc, "\"name\":\"Read\""));
   assert(strstr(acc, "\"type\":\"input_json_delta\"") && strstr(acc, "\\\"p\\\":1"));
   assert(strstr(acc, "\"type\":\"content_block_stop\""));
   assert(strstr(acc, "\"stop_reason\":\"tool_use\""));
   assert(strstr(acc, "event: message_stop"));
   /* Only a complete canonical response can supply the deferred opening usage. */
   assert(!ast.started);

   /* The callback-emit path (anthropic_delta_emit, the live-relay sink) must
    * produce byte-identical framed SSE to anthropic_delta_render (shared builder). */
   openai_stream_state_t ost2;
   openai_stream_state_init(&ost2);
   anthropic_stream_state_t ast2 = {0};
   g_emit_acc[0] = '\0';
   feed_emit("{\"choices\":[{\"delta\":{"
             "\"role\":\"assistant\",\"content\":\"Hel\"}}]}",
             &ost2, &ast2);
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
      assert(n == 1 && d[0].type == AIMEE_DELTA_BLOCK_STOP && d[0].kind == AIMEE_BLK_THINKING);
      assert(openai_stream_finish(&r2, d, 8) == 1 && d[0].type == AIMEE_DELTA_TURN_STOP);
      cJSON_Delete(c4);
   }

   /* Providers may send usage only after finish_reason, before [DONE]. */
   {
      openai_stream_state_t late;
      openai_stream_state_init(&late);
      aimee_delta_t d[8];
      cJSON *chunk = cJSON_Parse(
          "{\"choices\":[{\"delta\":{\"content\":\"done\"},\"finish_reason\":\"stop\"}]}");
      assert(openai_chunk_to_deltas(chunk, &late, d, 8) == 4);
      assert(!late.stopped);
      cJSON_Delete(chunk);
      chunk = cJSON_Parse("{\"choices\":[],\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":9,"
                          "\"prompt_tokens_details\":{\"cached_tokens\":12},\"completion_tokens_"
                          "details\":{\"reasoning_tokens\":4}}}");
      assert(openai_chunk_to_deltas(chunk, &late, d, 8) == 1);
      assert(d[0].type == AIMEE_DELTA_TURN_STOP && d[0].usage_in == 20 && d[0].usage_out == 9 &&
             d[0].usage_cache_read == 12 && d[0].usage_reasoning == 4);
      assert(openai_stream_finish(&late, d, 8) == 0);
      cJSON_Delete(chunk);
      openai_stream_state_init(&late);
      assert(openai_stream_finish(&late, d, 8) == -1);
      chunk = cJSON_Parse("{\"choices\":[{\"delta\":{\"content\":\"too many deltas\"}}]}");
      assert(openai_chunk_to_deltas(chunk, &late, d, 1) == -1);
      cJSON_Delete(chunk);
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

      /* Signatures remain opaque thinking state, separate from visible text. */
      A_FEED("content_block_delta",
             "{\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"abc\"}}");
      assert(n == 1 && d[0].kind == AIMEE_BLK_THINKING && !d[0].text_delta &&
             strcmp(d[0].signature_delta, "abc") == 0);
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

      /* Unsupported content variants fail instead of losing model output. */
      A_FEED("content_block_delta", "{\"index\":1,\"delta\":{\"type\":\"future_delta\"}}");
      assert(n == -1);
      A_DONE();

      A_FEED("content_block_stop", "{\"index\":1}");
      assert(n == 1);
      A_DONE();

      A_FEED("message_delta",
             "{\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":42}}");
      assert(n == 0); /* stashed; message_stop carries the terminal delta */
      A_DONE();

      A_FEED("message_stop", "{}");
      assert(n == 1 && d[0].type == AIMEE_DELTA_TURN_STOP);
      assert(d[0].stop_reason == AIMEE_STOP_TOOL_USE);
      assert(!strcmp(d[0].raw_stop_reason, "tool_use"));
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

   /* Native Anthropic refusals have the same canonical meaning as content
    * filtering, while their original stop spelling must survive the wire relay.
    * Future provider stops still retain the unknown classification. */
   for (int refused = 0; refused < 2; refused++)
   {
      anthropic_backend_stream_state_t st;
      anthropic_backend_stream_state_init(&st);
      aimee_delta_t d[4];
      cJSON *p;
      int n;
      A_FEED("message_start",
             "{\"message\":{\"role\":\"assistant\",\"usage\":{\"input_tokens\":0}}}");
      assert(n == 1);
      A_DONE();
      const char *reason = refused ? "refusal" : "future_stop";
      char terminal[160];
      snprintf(terminal, sizeof terminal,
               "{\"delta\":{\"stop_reason\":\"%s\"},\"usage\":{\"output_tokens\":0}}", reason);
      A_FEED("message_delta", terminal);
      assert(n == 0);
      A_DONE();
      A_FEED("message_stop", "{}");
      assert(n == 1 && d[0].type == AIMEE_DELTA_TURN_STOP);
      assert(d[0].stop_reason == (refused ? AIMEE_STOP_CONTENT_FILTER : AIMEE_STOP_UNKNOWN));
      assert(!strcmp(d[0].raw_stop_reason, reason));
      anthropic_stream_state_t rendered = {0};
      char *wire = anthropic_delta_render(&d[0], &rendered, "msg_refusal", "claude");
      assert(wire);
      char expected[80];
      snprintf(expected, sizeof expected, "\"stop_reason\":\"%s\"", reason);
      assert(strstr(wire, expected));
      free(wire);
      A_DONE();
   }

   /* Refusal details outlive the message_delta JSON and remain diagnostic
    * through canonical assembly and native rendering. Nullable and empty hints
    * are distinct; unrelated provider fallback hints are never copied. */
   for (int hints = 0; hints < 3; hints++)
   {
      anthropic_backend_stream_state_t st;
      anthropic_backend_stream_state_init(&st);
      aimee_delta_t d[4];
      cJSON *p;
      int n;
      A_FEED("message_start",
             "{\"message\":{\"role\":\"assistant\",\"usage\":{\"input_tokens\":0}}}");
      assert(n == 1);
      A_DONE();
      const char *details[] = {"{\"type\":\"refusal\",\"category\":null,\"explanation\":null}",
                               "{\"type\":\"refusal\",\"category\":\"future_category\","
                               "\"explanation\":\"Declined <untrusted> "
                               "text.\",\"fallback_credit_token\":\"PRIVATE_HINT\"}",
                               "{\"type\":\"refusal\",\"category\":\"\",\"explanation\":\"\"}"};
      char terminal[512];
      snprintf(terminal, sizeof terminal,
               "{\"delta\":{\"stop_reason\":\"refusal\",\"stop_details\":%s},"
               "\"usage\":{\"output_tokens\":0}}",
               details[hints]);
      A_FEED("message_delta", terminal);
      assert(n == 0);
      A_DONE(); /* The following pointers must not borrow this deleted JSON. */
      A_FEED("message_stop", "{}");
      assert(n == 1 && d[0].stop_reason == AIMEE_STOP_CONTENT_FILTER && d[0].refusal);
      if (!hints)
         assert(!d[0].refusal->category && !d[0].refusal->explanation);
      else
      {
         assert(!strcmp(d[0].refusal->category, hints == 1 ? "future_category" : ""));
         assert(!strcmp(d[0].refusal->explanation, hints == 1 ? "Declined <untrusted> text." : ""));
      }
      anthropic_stream_state_t rendered = {0};
      char *wire = anthropic_delta_render(&d[0], &rendered, "msg_refusal", "claude");
      assert(wire && strstr(wire, "\"stop_reason\":\"refusal\"") &&
             strstr(wire, "\"stop_details\":{\"type\":\"refusal\""));
      assert(!strstr(wire, "PRIVATE_HINT") && !strstr(wire, "fallback_credit_token"));
      free(wire);
      A_DONE();
   }

   const char *invalid_details[] = {
       "{\"type\":\"future_type\"}", "{\"type\":\"refusal\",\"category\":[]}",
       "{\"type\":\"refusal\",\"explanation\":true}",
       "{\"type\":\"refusal\",\"category\":null,\"category\":\"cyber\"}"};
   for (size_t i = 0; i < sizeof invalid_details / sizeof invalid_details[0]; i++)
   {
      anthropic_backend_stream_state_t st;
      anthropic_backend_stream_state_init(&st);
      aimee_delta_t d[4];
      cJSON *p;
      int n;
      A_FEED("message_start", "{\"message\":{\"role\":\"assistant\"}}");
      assert(n == 1);
      A_DONE();
      char terminal[512];
      snprintf(terminal, sizeof terminal,
               "{\"delta\":{\"stop_reason\":\"refusal\",\"stop_details\":%s}}", invalid_details[i]);
      A_FEED("message_delta", terminal);
      assert(n == -1);
      A_DONE();
   }

   for (int oversized = 0; oversized < 3; oversized++)
   {
      anthropic_backend_stream_state_t st;
      anthropic_backend_stream_state_init(&st);
      aimee_delta_t d[4];
      cJSON *p;
      int n;
      A_FEED("message_start", "{\"message\":{\"role\":\"assistant\"}}");
      assert(n == 1);
      A_DONE();
      char category[66], explanation[4098];
      memset(category, 'c', 64 + (oversized == 1));
      category[64 + (oversized == 1)] = 0;
      memset(explanation, 'e', 4096 + (oversized == 2));
      explanation[4096 + (oversized == 2)] = 0;
      cJSON *terminal = cJSON_CreateObject(), *delta = cJSON_AddObjectToObject(terminal, "delta");
      cJSON_AddStringToObject(delta, "stop_reason", "refusal");
      cJSON *details = cJSON_AddObjectToObject(delta, "stop_details");
      cJSON_AddStringToObject(details, "type", "refusal");
      cJSON_AddStringToObject(details, "category", category);
      cJSON_AddStringToObject(details, "explanation", explanation);
      char *encoded = cJSON_PrintUnformatted(terminal);
      A_FEED("message_delta", encoded);
      assert(n == (oversized ? -1 : 0));
      A_DONE();
      free(encoded);
      cJSON_Delete(terminal);
      if (!oversized)
      {
         A_FEED("message_stop", "{}");
         assert(n == 1 && d[0].refusal && strlen(d[0].refusal->category) == 64 &&
                strlen(d[0].refusal->explanation) == 4096);
         A_DONE();
      }
   }

   /* A canonical content filter must remain a refusal even without native hints. */
   {
      aimee_delta_t d = {.type = AIMEE_DELTA_TURN_STOP, .stop_reason = AIMEE_STOP_CONTENT_FILTER};
      anthropic_stream_state_t st = {0};
      char *wire = anthropic_delta_render(&d, &st, "msg_refusal", "claude");
      assert(wire && strstr(wire, "\"stop_reason\":\"refusal\""));
      free(wire);
      aimee_refusal_details_t details = {.category = "future_category"};
      d.refusal = &details;
      d.stop_reason = AIMEE_STOP_END_TURN;
      assert(!anthropic_delta_render(&d, &st, "msg_refusal", "claude"));
      d.type = AIMEE_DELTA_TURN_START;
      d.stop_reason = AIMEE_STOP_CONTENT_FILTER;
      assert(!anthropic_delta_render(&d, &st, "msg_refusal", "claude"));
      g_emit_acc[0] = 0;
      assert(anthropic_delta_emit(&d, &st, "msg_refusal", "claude", emit_collect, NULL) < 0);
      assert(!g_emit_acc[0]);
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
      A_FEED("message_start", "{\"message\":{\"role\":\"assistant\"}}");
      A_DONE();
      A_FEED("content_block_start", "{\"index\":0,\"content_block\":{\"type\":\"text\"}}");
      assert(n == 1);
      A_DONE();
      A_FEED("content_block_delta",
             "{\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"x\"}}");
      assert(n == -1);
      A_DONE();

      /* a KNOWN variant missing its own payload field */
      anthropic_backend_stream_state_init(&st);
      A_FEED("message_start", "{\"message\":{\"role\":\"assistant\"}}");
      A_DONE();
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
      anthropic_backend_stream_state_init(&st);
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

   /* A wire without a representation must refuse opaque Responses state. */
   {
      anthropic_stream_state_t state = {0};
      aimee_delta_t delta = {.type = AIMEE_DELTA_BLOCK_START,
                             .kind = AIMEE_BLK_THINKING,
                             .thinking_id = "rs_1",
                             .thinking_encrypted_content = "cipher"};
      assert(!anthropic_delta_render(&delta, &state, "m", "model"));
      assert(anthropic_delta_emit(&delta, &state, "m", "model", NULL, NULL) < 0);
   }

   printf("ok\n");
   return 0;
}
