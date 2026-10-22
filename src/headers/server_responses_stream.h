#ifndef AIMEE_SERVER_RESPONSES_STREAM_H
#define AIMEE_SERVER_RESPONSES_STREAM_H
#include <aimee/translation/aimee_ir_stream.h>

typedef struct
{
   aimee_responses_frontend_t *frontend;
   aimee_sse_emit_fn emit;
   void *context;
   const char *message_id;
   int used, terminal;
} server_responses_stream_t;

/* One admitted fenced transport attempt. No retry after partial output. */
int server_responses_stream_post(server_responses_stream_t *stream, const char *url,
                                 const char *auth, const void *body, size_t length,
                                 const char *headers, const char *provider, const char *model,
                                 int timeout_ms, aimee_response_t *response);
int server_responses_stream_publish(server_responses_stream_t *stream, const aimee_delta_t *delta);
#endif
