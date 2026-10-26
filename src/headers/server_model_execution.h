#ifndef AIMEE_SERVER_MODEL_EXECUTION_H
#define AIMEE_SERVER_MODEL_EXECUTION_H
#include "agent_request_build.h"
#include "server_responses_stream.h"

/* Execution settings and existing edge/cache hooks; all model content uses the
 * canonical Request/Response. No protocol-shaped message DTO crosses this API. */
typedef struct
{
   int preserve_messages, responses_stream, capture_error_observations;
   int retry_attempts, retry_base_ms, retry_max_ms;
   server_responses_stream_t *stream;
   int (*stream_post)(server_responses_stream_t *, const char *, const char *, const void *, size_t,
                      const char *, const char *, const char *, int, aimee_response_t *);
   int (*connection)(const agent_t *, aimee_wire_t, char *, size_t, char *, size_t);
   void (*upstream_error)(int, const char *, char *, size_t);
   void (*native_error)(int, const char *, void *);
   void *error_context;
   void (*admitted)(const aimee_request_t *);
   int (*response_projection)(const aimee_response_t *);
   int (*cache_read)(const agent_t *, const aimee_request_t *, aimee_response_t *, char *, size_t,
                     void *);
   void (*cache_invalidate)(void *);
   void (*cache_write)(const agent_t *, const aimee_response_t *, void *);
   void *cache_context;
} server_model_execution_options_t;
enum
{
   SERVER_MODEL_CONNECTION_UNAVAILABLE = -3,
   SERVER_MODEL_RENDER_REFUSED = -4,
   SERVER_MODEL_HISTORY_REFUSED = -5
};
int server_model_governance_enabled(void);
int server_model_execute_prepared(const agent_t *, aimee_request_t *, aimee_response_t *, char *,
                                  size_t, const server_model_execution_options_t *);
#endif
