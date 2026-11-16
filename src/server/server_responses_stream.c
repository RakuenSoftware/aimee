#include "server_responses_stream.h"
#include "wire_fence.h"
#include "sse_parser.h"
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

#define RESPONSE_FRAME_LIMIT (16u * 1024u * 1024u)
typedef struct
{
   server_responses_stream_t *client;
   aimee_responses_stream_t *backend;
   sse_parser_t parser;
   char event[128];
   char *data;
   size_t length, bytes;
   int failed, terminal;
} response_transport_t;

int server_responses_stream_publish(server_responses_stream_t *stream, const aimee_delta_t *delta)
{
   if (!stream || !delta || stream->terminal)
      return -1;
   aimee_session_event_t event = {.kind = AIMEE_SESSION_DELTA,
                                  .role = "assistant",
                                  .message_id = stream->message_id,
                                  .delta = delta};
   int rc = aimee_ir_session_emit(&event);
   if (!rc && delta->type == AIMEE_DELTA_TURN_STOP)
      stream->terminal = 1;
   return rc;
}

static int delta(void *context, const aimee_delta_t *value)
{
   response_transport_t *transport = context;
   server_responses_stream_t *stream = transport->client;
   if (server_responses_stream_publish(stream, value))
      return -1;
   return aimee_responses_frontend_delta(stream->frontend, value, stream->emit, stream->context);
}

static int flush(response_transport_t *transport)
{
   if (!transport->length)
   {
      transport->event[0] = 0;
      return 0;
   }
   if (transport->terminal && transport->length == 6 && !memcmp(transport->data, "[DONE]", 6))
   {
      free(transport->data);
      transport->data = NULL;
      transport->length = 0;
      transport->event[0] = 0;
      return 0;
   }
   cJSON *frame = aimee_ir_json_parse_frame(transport->data, transport->length);
   int rc = !frame || aimee_responses_stream_feed(transport->backend, transport->event, frame,
                                                  delta, transport);
   const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "type"));
   const char *event = transport->event[0] ? transport->event : type;
   if (!rc && event &&
       (!strcmp(event, "response.completed") || !strcmp(event, "response.incomplete")))
      transport->terminal = 1;
   cJSON_Delete(frame);
   free(transport->data);
   transport->data = NULL;
   transport->length = 0;
   transport->event[0] = 0;
   return rc ? -1 : 0;
}

static int line(const char *text, size_t length, void *context)
{
   response_transport_t *transport = context;
   if (!length)
      return flush(transport);
   if (length >= 6 && !strncmp(text, "event:", 6))
   {
      size_t offset = length > 6 && text[6] == ' ' ? 7 : 6;
      if (length - offset >= sizeof transport->event || transport->event[0])
         return -1;
      memcpy(transport->event, text + offset, length - offset);
      transport->event[length - offset] = 0;
   }
   else if (length >= 5 && !strncmp(text, "data:", 5))
   {
      size_t offset = length > 5 && text[5] == ' ' ? 6 : 5;
      size_t size = length - offset, separator = transport->length ? 1 : 0;
      if (size + separator > RESPONSE_FRAME_LIMIT ||
          transport->length > RESPONSE_FRAME_LIMIT - size - separator)
         return -1;
      char *grown = realloc(transport->data, transport->length + size + separator + 1);
      if (!grown)
         return -1;
      transport->data = grown;
      if (separator)
         grown[transport->length++] = '\n';
      memcpy(grown + transport->length, text + offset, size);
      transport->length += size;
      grown[transport->length] = 0;
   }
   return 0;
}

static int chunk(const char *data, size_t length, void *context)
{
   response_transport_t *transport = context;
   /* Bound framing, including a peer that never terminates an SSE line. */
   if (length > RESPONSE_FRAME_LIMIT || transport->bytes > RESPONSE_FRAME_LIMIT - length)
   {
      transport->failed = 1;
      return -1;
   }
   transport->bytes += length;
   int rc = sse_parser_feed(&transport->parser, data, length, line, transport);
   if (rc)
      transport->failed = 1;
   return rc;
}

int server_responses_stream_post(server_responses_stream_t *stream, const char *url,
                                 const char *auth, const void *body, size_t length,
                                 const char *headers, const char *provider, const char *model,
                                 int timeout_ms, aimee_response_t *response)
{
   if (!stream || !stream->frontend || !stream->emit || !stream->message_id || stream->used)
      return -1;
   stream->used = 1;
   response_transport_t transport = {.client = stream, .backend = aimee_responses_stream_new()};
   if (!transport.backend)
      return -1;
   sse_parser_init(&transport.parser);
   int status = wire_fence_post_stream(url, auth, body, length, chunk, &transport, timeout_ms,
                                       headers, provider, model, WIRE_FENCE_OPENAI_RESPONSES);
   /* A partial line or un-delimited event is truncation, even if its JSON happens
    * to parse. The native terminal event must have been fully received. */
   if (status == 200 &&
       (transport.failed || transport.parser.buf_len || transport.length || transport.event[0] ||
        aimee_responses_stream_take(transport.backend, response)))
      status = -1;
   sse_parser_free(&transport.parser);
   free(transport.data);
   aimee_responses_stream_free(transport.backend);
   return status;
}
