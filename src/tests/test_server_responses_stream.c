#include "server_responses_stream.h"
#include "wire_fence.h"
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *frames;
static int http_status = 200, fail_persistence, persistence_calls, deltas, done;
static aimee_ir_response_builder_t durable;
int aimee_ir_session_emit(const aimee_session_event_t *event)
{
   assert(event->kind == AIMEE_SESSION_DELTA && !strcmp(event->message_id, "client"));
   if (++persistence_calls == fail_persistence)
      return -1;
   return aimee_ir_response_builder_apply(&durable, event->delta);
}
static void emit(void *context, const char *name, const char *json)
{
   (void)context;
   cJSON *value = cJSON_Parse(json);
   assert(value);
   cJSON_Delete(value);
   if (strstr(name, ".delta"))
      deltas++;
   if (strstr(name, ".done") || !strcmp(name, "response.completed"))
      done++;
}
int wire_fence_post_stream(const char *url, const char *auth, const void *body, size_t length,
                           wire_fence_stream_cb callback, void *context, int timeout_ms,
                           const char *headers, const char *provider, const char *model,
                           wire_fence_route_t route)
{
   (void)url;
   (void)auth;
   (void)body;
   (void)length;
   (void)timeout_ms;
   (void)headers;
   (void)provider;
   (void)model;
   assert(route == WIRE_FENCE_OPENAI_RESPONSES);
   for (size_t i = 0; frames[i]; i++)
   {
      if (callback(frames + i, 1, context))
         return -1;
      /* The first text delta must be visible before any terminal provider bytes. */
      if (!strncmp(frames + i + 1, "event: response.completed", 25))
         assert(deltas == 1 && done == 0);
   }
   return http_status;
}
#define LIVE                                                                                       \
   "event: response.output_item.added\r\ndata: "                                                   \
   "{\"output_index\":0,\"item\":{\"id\":\"m\",\"type\":\"message\"}}\r\n\r\nevent: "              \
   "response.output_text.delta\ndata: "                                                            \
   "{\"output_index\":0,\"content_index\":0,\"item_id\":\"m\",\"delta\":\"hello\"}\n\n"
#define TERMINAL                                                                                   \
   "event: response.completed\ndata: "                                                             \
   "{\"response\":{\"output\":[{\"id\":\"m\",\"type\":\"message\",\"content\":[{\"type\":"         \
   "\"output_text\",\"text\":\"hello\"}]}]}}\n\n"
static void attempt(const char *input, int expected, int persistence_failure)
{
   aimee_ir_response_builder_free(&durable);
   deltas = done = persistence_calls = 0;
   frames = input;
   fail_persistence = persistence_failure;
   aimee_response_t response = {0};
   server_responses_stream_t stream = {.frontend =
                                           aimee_responses_frontend_new("client", "model", 1),
                                       .emit = emit,
                                       .message_id = "client"};
   assert(stream.frontend);
   int status = server_responses_stream_post(&stream, "url", "auth", "{}", 2, "", "provider",
                                             "model", 100, &response);
   assert(status == expected && stream.used && !done);
   if (expected == 200)
   {
      assert(!strcmp(response.content[0].text, "hello"));
      assert(!aimee_responses_frontend_matches(stream.frontend, &response));
      assert(!aimee_responses_frontend_complete(stream.frontend, &response, emit, NULL));
      assert(done == 4);
   }
   else
      assert(!response.content);
   assert(server_responses_stream_post(&stream, "url", "auth", "{}", 2, "", "provider", "model",
                                       100, &response) < 0);
   aimee_responses_frontend_free(stream.frontend);
   aimee_response_free(&response);
}
int main(void)
{
   attempt(LIVE TERMINAL, 200, 0);
   attempt(LIVE TERMINAL "data: [DONE]\n\n", 200, 0);
   attempt(LIVE, -1, 0);
   attempt(LIVE "event: response.completed\ndata: {}", -1, 0);
   attempt(LIVE TERMINAL "data: {}\n\n", -1, 0);
   attempt(LIVE TERMINAL, -1, 1);
   attempt(LIVE TERMINAL, -1, 3);
   http_status = 503;
   attempt("", 503, 0);
   attempt("data: {}\n\n", -1, 0);
   aimee_ir_response_builder_free(&durable);
   puts("Responses transport: fragmentation, early deltas, truncation and persistence refusal "
        "passed");
   return 0;
}
