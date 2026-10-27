#include <aimee/translation/aimee_ir_stream.h>
#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static aimee_ir_response_builder_t observed;
static aimee_delta_t opening;
static aimee_responses_frontend_t *frontend;
static int client_deltas, client_done;
static void capture(void *context, const char *name, const char *json)
{
   (void)context;
   cJSON *event = cJSON_Parse(json);
   assert(event);
   if (strstr(name, ".delta"))
      client_deltas++;
   if (strstr(name, ".done") || !strcmp(name, "response.completed"))
      client_done++;
   cJSON_Delete(event);
}
static int sink(void *context, const aimee_delta_t *delta)
{
   int *count = context;
   (*count)++;
   if (delta->type == AIMEE_DELTA_TURN_START)
      opening = *delta;
   /* The adapter must leave client completion to the durable host owner. */
   assert(delta->type != AIMEE_DELTA_TURN_STOP);
   int rc = aimee_ir_response_builder_apply(&observed, delta);
   return rc || (frontend && aimee_responses_frontend_delta(frontend, delta, capture, NULL));
}
static int event(aimee_responses_stream_t *stream, const char *name, const char *body, int *count)
{
   cJSON *payload = cJSON_Parse(body);
   assert(payload);
   int rc = aimee_responses_stream_feed(stream, name, payload, sink, count);
   cJSON_Delete(payload);
   return rc;
}
static aimee_responses_stream_t *start(int *count)
{
   aimee_ir_response_builder_free(&observed);
   *count = 0;
   aimee_responses_stream_t *stream = aimee_responses_stream_new();
   assert(stream);
   assert(!event(stream, "response.output_item.added",
                 "{\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\"}}", count));
   return stream;
}
static int text(aimee_responses_stream_t *stream, int *count)
{
   return event(stream, "response.output_text.delta",
                "{\"output_index\":0,\"content_index\":0,\"item_id\":\"m\",\"delta\":\"Hel\"}",
                count);
}
#define COMPLETE                                                                                   \
   "{\"response\":{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"message\","       \
   "\"id\":\"m\",\"content\":[{\"type\":\"output_text\",\"text\":\"Hello\"}]}],\"usage\":{"        \
   "\"input_tokens\":7,\"output_tokens\":2}}}"
static void refusal_projection(void)
{
   aimee_refusal_details_t details = {.category = "future_category",
                                      .explanation = "Inert diagnostic"};
   aimee_delta_t start = {.type = AIMEE_DELTA_TURN_START};
   aimee_delta_t stop = {.type = AIMEE_DELTA_TURN_STOP,
                         .stop_reason = AIMEE_STOP_CONTENT_FILTER,
                         .raw_stop_reason = "refusal",
                         .refusal = &details};
   aimee_ir_response_builder_t canonical = {0};
   assert(!aimee_ir_response_builder_apply(&canonical, &start));
   assert(!aimee_ir_response_builder_apply(&canonical, &stop));
   aimee_responses_frontend_t *target = aimee_responses_frontend_new("refusal", "model", 1);
   assert(target && !aimee_responses_frontend_delta(target, &start, capture, NULL));
   int before = client_deltas + client_done;
   assert(aimee_responses_frontend_matches(target, &canonical.response) < 0);
   assert(aimee_responses_frontend_complete(target, &canonical.response, capture, NULL) < 0);
   assert(client_deltas + client_done == before);
   aimee_responses_frontend_free(target);
   target = aimee_responses_frontend_new("delta", "model", 1);
   assert(target && !aimee_responses_frontend_delta(target, &start, capture, NULL));
   assert(aimee_responses_frontend_delta(target, &stop, capture, NULL) < 0);
   assert(client_deltas + client_done == before);
   assert(canonical.response.refusal &&
          !strcmp(canonical.response.refusal->category, "future_category"));
   assert(!strcmp(canonical.response.refusal->explanation, "Inert diagnostic"));
   assert(stop.refusal == &details); /* Projection failure cannot mutate canonical evidence. */
   aimee_responses_frontend_free(target);
   target = aimee_responses_frontend_new("legacy", "model", 1);
   aimee_response_t legacy = canonical.response;
   legacy.refusal = NULL;
   assert(target && !aimee_responses_frontend_delta(target, &start, capture, NULL));
   assert(!aimee_responses_frontend_matches(target, &legacy));
   assert(!aimee_responses_frontend_complete(target, &legacy, capture, NULL));
   aimee_responses_frontend_free(target);
   client_deltas = client_done = 0;
   aimee_ir_response_builder_free(&canonical);
}

static void initial_usage_observation(void)
{
   const char *usage[] = {
       "",
       ",\"usage\":null",
       ",\"usage\":{}",
       ",\"usage\":{\"input_tokens\":0}",
       ",\"usage\":{\"output_tokens\":0}",
       ",\"usage\":{\"input_tokens\":0,\"output_tokens\":0}",
       ",\"usage\":{\"input_tokens\":20,\"output_tokens\":3,\"input_tokens_details\":{\"cached_"
       "tokens\":7},\"output_tokens_details\":{\"reasoning_tokens\":2}}"};
   for (size_t i = 0; i < sizeof usage / sizeof *usage; i++)
   {
      aimee_ir_response_builder_free(&observed);
      aimee_responses_stream_t *stream = aimee_responses_stream_new();
      assert(stream);
      int count = 0;
      char frame[512];
      snprintf(frame, sizeof frame, "{\"response\":{\"id\":\"r\",\"status\":\"in_progress\"%s}}",
               usage[i]);
      assert(!event(stream, "response.created", frame, &count));
      assert(count == 1 && opening.usage_unobserved == (i < 5));
      if (i == 6)
         assert(opening.usage_in == 20 && opening.usage_out == 3 && opening.usage_cache_read == 7 &&
                opening.usage_reasoning == 2);
      aimee_responses_stream_free(stream);
   }
   aimee_ir_response_builder_free(&observed);
}

/* The same real native frames enter buffered and live paths. Late done items
 * supply opaque/tool state omitted from the terminal response. */
static void buffered_live_parity(void)
{
   const char *frames[] = {
       "{\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"m\","
       "\"type\":\"message\"}}",
       "{\"type\":\"response.output_text.delta\",\"output_index\":0,\"content_index\":0,\"item_"
       "id\":\"m\",\"delta\":\"Hel\"}",
       "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":"
       "\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"Hello\"}]}}",
       "{\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"id\":\"r\","
       "\"type\":\"reasoning\"}}",
       "{\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"id\":\"r\",\"type\":"
       "\"reasoning\",\"encrypted_content\":\"cipher\",\"content\":[{\"type\":\"reasoning_text\","
       "\"text\":\"opaque\"}],\"summary\":[]}}",
       "{\"type\":\"response.output_item.added\",\"output_index\":2,\"item\":{\"id\":\"f\","
       "\"type\":\"function_call\",\"call_id\":\"c\",\"name\":\"read\",\"namespace\":\"fs\"}}",
       "{\"type\":\"response.output_item.done\",\"output_index\":2,\"item\":{\"id\":\"f\",\"type\":"
       "\"function_call\",\"call_id\":\"c\",\"name\":\"read\",\"namespace\":\"fs\",\"arguments\":"
       "\"{\\\"n\\\":123.1234567890123456789}\"}}",
       "{\"type\":\"response.completed\",\"response\":{\"id\":\"result\",\"status\":\"completed\","
       "\"metadata\":{\"native\":true},\"output\":[{\"id\":\"m\",\"type\":\"message\",\"content\":["
       "]}],\"usage\":{\"input_tokens\":7,\"output_tokens\":2}}}"};
   char body[8192] = "", error[128];
   size_t used = 0;
   int count = 0;
   aimee_ir_response_builder_free(&observed);
   aimee_responses_stream_t *stream = aimee_responses_stream_new();
   assert(stream);
   for (size_t i = 0; i < sizeof frames / sizeof *frames; i++)
   {
      cJSON *frame = cJSON_Parse(frames[i]);
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "type"));
      assert(type && !aimee_responses_stream_feed(stream, type, frame, sink, &count));
      int n = snprintf(body + used, sizeof body - used, "event: %s\ndata: %s\n\n", type, frames[i]);
      assert(n > 0 && (size_t)n < sizeof body - used);
      used += (size_t)n;
      cJSON_Delete(frame);
   }
   aimee_response_t live = {0}, buffered = {0};
   assert(!aimee_responses_stream_take(stream, &live));
   assert(!responses_backend_decode_body(body, &buffered, error, sizeof error));
   assert(cJSON_Compare(live.raw, buffered.raw, 1));
   assert(live.n_content == 3 && buffered.n_content == 3 && live.usage_in == 7 &&
          buffered.usage_out == 2 && live.stop_reason == buffered.stop_reason);
   assert(!strcmp(buffered.content[0].text, "Hello") &&
          !strcmp(buffered.content[1].thinking_encrypted_content, "cipher") &&
          !strcmp(buffered.content[2].tool_namespace, "fs"));
   char *arguments = cJSON_PrintUnformatted(buffered.content[2].tool_input);
   assert(arguments && strstr(arguments, "123.1234567890123456789"));
   free(arguments);
   aimee_response_free(&live);
   aimee_response_free(&buffered);
   aimee_responses_stream_free(stream);
   aimee_ir_response_builder_free(&observed);
}

static void buffered_alias_admission(void)
{
   const char *item =
       "data: "
       "{\"type\":\"response.output_item.done\",\"item\":{\"id\":\"a\",\"type\":\"message\","
       "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}}\n\n";
   const char *terminal = "data: "
                          "{\"type\":\"response.completed\",\"response\":{\"id\":\"owned\","
                          "\"status\":\"completed\",\"output\":[]}}\n\n";
   const char *bad[] = {
       "data: "
       "{\"type\":\"response.content_part.done\",\"item_id\":\"a\",\"part\":{\"type\":\"output_"
       "text\",\"text\":\"changed\"}}\n\n",
       "data: {\"type\":\"response.output_text.done\",\"item_id\":\"foreign\",\"text\":\"ok\"}\n\n",
       item};
   char body[2048], error[128];
   const char *bad_role = "data: "
                          "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\","
                          "\"output\":[{\"type\":\"message\",\"role\":17,\"content\":[{\"type\":"
                          "\"output_text\",\"text\":\"ok\"}]}]}}\n\n";
   const char *sparse =
       "data: {\"type\":\"response.output_text.done\",\"item_id\":\"a\",\"text\":\"ok\"}\n\ndata: "
       "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[{"
       "\"type\":\"message\",\"id\":\"a\"}]}}\n\n";
   aimee_response_t sparse_response = {0};
   assert(!responses_backend_decode_body(sparse, &sparse_response, error, sizeof error));
   assert(sparse_response.n_content == 1 && !strcmp(sparse_response.content[0].text, "ok"));
   aimee_response_free(&sparse_response);
   aimee_response_t malformed = {0};
   assert(responses_backend_decode_body(bad_role, &malformed, error, sizeof error));
   aimee_response_free(&malformed);
   for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
   {
      snprintf(body, sizeof body, "%s%s%s", item, bad[i], terminal);
      aimee_response_t response = {0};
      assert(responses_backend_decode_body(body, &response, error, sizeof error));
      aimee_response_free(&response);
   }
   snprintf(body, sizeof body, "%s%s%s", item, terminal,
            "data: "
            "{\"type\":\"response.refusal.done\",\"response_id\":\"foreign\",\"refusal\":"
            "\"declined\"}\n\n");
   aimee_response_t response = {0};
   assert(!responses_backend_decode_body(body, &response, error, sizeof error));
   assert(!aimee_ir_response_valid(&response) && response.n_content == 0 &&
          response.stop_reason == AIMEE_STOP_CONTENT_FILTER &&
          !strcmp(response.raw_stop_reason, "refusal") && response.raw);
   aimee_response_free(&response);
}

int main(void)
{
   buffered_alias_admission();
   buffered_live_parity();
   initial_usage_observation();
   refusal_projection();
   int count;
   aimee_response_t final = {0};
   frontend = aimee_responses_frontend_new("client", "model", 10);
   assert(frontend);
   aimee_responses_stream_t *stream = start(&count);
   assert(!text(stream, &count));
   assert(count == 3 && !strcmp(observed.response.content[0].text, "Hel"));
   assert(client_deltas == 1 && client_done == 0);
   assert(aimee_responses_stream_take(stream, &final) < 0); /* truncated stream */
   assert(!event(stream, "response.completed", COMPLETE, &count));
   assert(!observed.complete && !strcmp(observed.response.content[0].text, "Hello"));
   assert(!aimee_responses_stream_take(stream, &final));
   assert(final.usage_in == 7 && final.usage_out == 2 && final.content[0].message_group == 1);
   assert(!aimee_responses_frontend_matches(frontend, &final));
   assert(client_done == 0 && client_deltas == 2);
   assert(!aimee_responses_frontend_complete(frontend, &final, capture, NULL));
   assert(client_done == 4);
   assert(aimee_responses_frontend_complete(frontend, &final, capture, NULL) < 0);
   aimee_responses_frontend_free(frontend);
   frontend = NULL;
   assert(aimee_responses_stream_take(stream, &final) < 0);
   aimee_response_free(&final);
   aimee_responses_stream_free(stream);

   frontend = aimee_responses_frontend_new("grouped", "model", 1);
   client_deltas = client_done = 0;
   stream = start(&count);
   assert(!text(stream, &count));
   assert(!event(stream, "response.output_text.delta",
                 "{\"output_index\":0,\"content_index\":1,\"item_id\":\"m\",\"delta\":\"second\"}",
                 &count));
   assert(!event(stream, "response.output_item.added",
                 "{\"output_index\":1,\"item\":{\"id\":\"m2\",\"type\":\"message\"}}", &count));
   assert(!event(stream, "response.output_text.delta",
                 "{\"output_index\":1,\"content_index\":0,\"item_id\":\"m2\",\"delta\":\"third\"}",
                 &count));
   assert(!event(stream, "response.completed",
                 "{\"response\":{\"output\":[{\"id\":\"m\",\"type\":\"message\",\"content\":[{"
                 "\"type\":\"output_text\",\"text\":\"Hel\"},{\"type\":\"output_text\",\"text\":"
                 "\"second\"}]},{\"id\":\"m2\",\"type\":\"message\",\"content\":[{\"type\":"
                 "\"output_text\",\"text\":\"third\"}]}]}}",
                 &count));
   assert(!aimee_responses_stream_take(stream, &final));
   assert(final.n_content == 3 && final.content[0].message_group == 1 &&
          final.content[1].message_group == 1 && final.content[2].message_group == 2);
   assert(client_deltas == 3 && !client_done);
   assert(!aimee_responses_frontend_complete(frontend, &final, capture, NULL));
   assert(client_done == 9);
   aimee_responses_frontend_free(frontend);
   frontend = NULL;
   aimee_response_free(&final);
   aimee_responses_stream_free(stream);

   stream = start(&count);
   assert(!text(stream, &count));
   assert(event(stream, "response.completed",
                "{\"response\":{\"output\":[{\"id\":\"m\",\"type\":\"message\",\"content\":[{"
                "\"type\":\"output_text\",\"text\":\"rewritten\"}]}]}}",
                &count) < 0);
   assert(aimee_responses_stream_take(stream, &final) < 0);
   aimee_responses_stream_free(stream);

   stream = start(&count);
   assert(
       event(stream, "response.output_text.delta",
             "{\"output_index\":0,\"content_index\":0,\"item_id\":\"foreign\",\"delta\":\"bad\"}",
             &count) < 0);
   assert(event(stream, "response.completed", COMPLETE, &count) < 0);
   aimee_responses_stream_free(stream);

   stream = start(&count);
   assert(!event(stream, "response.output_item.done",
                 "{\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\",\"content\":[{"
                 "\"type\":\"output_text\",\"text\":\"first completed value\"}]}}",
                 &count));
   assert(event(stream, "response.completed",
                "{\"response\":{\"output\":[{\"id\":\"m\",\"type\":\"message\",\"content\":[{"
                "\"type\":\"output_text\",\"text\":\"conflicting terminal value\"}]}]}}",
                &count) < 0);
   assert(aimee_responses_stream_take(stream, &final) < 0);
   aimee_responses_stream_free(stream);

   aimee_ir_response_builder_free(&observed);
   stream = aimee_responses_stream_new();
   count = 0;
   assert(!event(stream, "response.output_item.added",
                 "{\"output_index\":0,\"item\":{\"id\":\"fc\",\"type\":\"function_call\",\"call_"
                 "id\":\"call\",\"name\":\"read\",\"namespace\":\"fs\"}}",
                 &count));
   assert(!event(stream, "response.function_call_arguments.delta",
                 "{\"output_index\":0,\"item_id\":\"fc\",\"delta\":\"{\\\"path\\\":\"}", &count));
   assert(!event(
       stream, "response.completed",
       "{\"response\":{\"output\":[{\"id\":\"fc\",\"type\":\"function_call\",\"call_id\":\"call\","
       "\"name\":\"read\",\"namespace\":\"fs\",\"arguments\":\"{\\\"path\\\":\\\"a\\\"}\"}]}}",
       &count));
   assert(!aimee_responses_stream_take(stream, &final));
   assert(final.stop_reason == AIMEE_STOP_TOOL_USE &&
          !strcmp(final.content[0].tool_namespace, "fs"));
   aimee_response_free(&final);
   aimee_responses_stream_free(stream);

   aimee_ir_response_builder_free(&observed);
   stream = aimee_responses_stream_new();
   assert(!event(stream, "response.output_item.added",
                 "{\"output_index\":0,\"item\":{\"id\":\"white\",\"type\":\"function_call\",\"call_"
                 "id\":\"w\",\"name\":\"read\"}}",
                 &count));
   assert(!event(stream, "response.function_call_arguments.delta",
                 "{\"output_index\":0,\"item_id\":\"white\",\"delta\":\"{ \\\"path\\\" : \"}",
                 &count));
   assert(!event(stream, "response.completed",
                 "{\"response\":{\"output\":[{\"id\":\"white\",\"type\":\"function_call\",\"call_"
                 "id\":\"w\",\"name\":\"read\",\"arguments\":\"{ \\\"path\\\" : \\\"a\\\" }\"}]}}",
                 &count));
   assert(!aimee_responses_stream_take(stream, &final));
   aimee_response_free(&final);
   aimee_responses_stream_free(stream);

   aimee_ir_response_builder_free(&observed);
   frontend = aimee_responses_frontend_new("thinking", "model", 1);
   stream = aimee_responses_stream_new();
   assert(!event(stream, "response.output_item.added",
                 "{\"output_index\":0,\"item\":{\"id\":\"rs\",\"type\":\"reasoning\"}}", &count));
   assert(!event(stream, "response.reasoning_summary_text.delta",
                 "{\"output_index\":0,\"summary_index\":0,\"item_id\":\"rs\",\"delta\":\"first\"}",
                 &count));
   assert(!event(stream, "response.reasoning_summary_text.delta",
                 "{\"output_index\":0,\"summary_index\":1,\"item_id\":\"rs\",\"delta\":\"second\"}",
                 &count));
   assert(!event(stream, "response.incomplete",
                 "{\"response\":{\"status\":\"incomplete\",\"output\":[{\"id\":\"rs\",\"type\":"
                 "\"reasoning\",\"encrypted_content\":\"cipher\",\"summary\":[{\"text\":\"first\"},"
                 "{\"text\":\"second\"}]}]}}",
                 &count));
   assert(!aimee_responses_stream_take(stream, &final));
   assert(final.stop_reason == AIMEE_STOP_MAX_TOKENS);
   assert(!aimee_responses_frontend_complete(frontend, &final, capture, NULL));
   aimee_responses_frontend_free(frontend);
   frontend = NULL;
   assert(!strcmp(observed.response.content[0].thinking_encrypted_content, "cipher"));
   assert(!strcmp(observed.response.content[0].text, "first\n\nsecond"));
   aimee_response_free(&final);
   aimee_responses_stream_free(stream);
   aimee_ir_response_builder_free(&observed);
   puts("responses stream: passed");
   return 0;
}
