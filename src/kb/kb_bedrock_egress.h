#ifndef KB_BEDROCK_EGRESS_H
#define KB_BEDROCK_EGRESS_H

#include "aimee_backend.h"
#include "aimee_ir_stream.h"
#include "db2/org_model_catalog.h"
#include "modules/aws/aws_eventstream.h"
#include "modules/aws/aws_sigv4.h"

#include <stddef.h>

#define KB_BEDROCK_BODY_MAX         (16U * 1024U * 1024U)
#define KB_BEDROCK_EVENT_JSON_MAX   (1U * 1024U * 1024U)
#define KB_BEDROCK_RAW_PATH_CAP     256
#define KB_BEDROCK_ENCODED_PATH_CAP 769
#define KB_BEDROCK_HOST_CAP         128
#define KB_BEDROCK_MAX_HEADERS      6

typedef enum
{
   KB_BEDROCK_OK = 0,
   KB_BEDROCK_INVALID_ARGUMENT,
   KB_BEDROCK_INVALID_TARGET,
   KB_BEDROCK_TOO_LARGE,
   KB_BEDROCK_SIGNING_ERROR,
   KB_BEDROCK_MALFORMED_RESPONSE,
   KB_BEDROCK_MALFORMED_STREAM,
   KB_BEDROCK_INCOMPLETE_STREAM,
   KB_BEDROCK_PROVIDER_ERROR,
   KB_BEDROCK_CALLBACK_ABORT,
   KB_BEDROCK_BUSY,
   KB_BEDROCK_POISONED,
   KB_BEDROCK_INTERNAL_ERROR
} kb_bedrock_result_t;

typedef struct
{
   const char *access_key_id;
   const char *secret_access_key;
   const char *session_token;
   const char *amz_date;
   const char *date;
} kb_bedrock_credentials_t;

typedef struct
{
   const char *name;
   const char *value;
} kb_bedrock_header_t;

typedef struct
{
   unsigned initialized;
   char *body;
   size_t body_len;
   char raw_path[KB_BEDROCK_RAW_PATH_CAP];
   char encoded_path[KB_BEDROCK_ENCODED_PATH_CAP];
   char host[KB_BEDROCK_HOST_CAP];
   char payload_hash[65];
   aws_sigv4_result_t sig;
   int streaming;
} kb_bedrock_wire_request_t;

/* Owned and non-copyable. Initialize before first use and clear exactly once. */
void kb_bedrock_wire_request_init(kb_bedrock_wire_request_t *request);
void kb_bedrock_wire_request_clear(kb_bedrock_wire_request_t *request);
kb_bedrock_result_t kb_bedrock_wire_request_build(const db2_bedrock_target_t *target,
                                                  const aimee_request_t *ir, int streaming,
                                                  const kb_bedrock_credentials_t *credentials,
                                                  kb_bedrock_wire_request_t *request);
kb_bedrock_result_t kb_bedrock_wire_request_headers(const kb_bedrock_wire_request_t *request,
                                                    kb_bedrock_header_t *headers, size_t capacity,
                                                    size_t *count);

/* Initialize before first parse. Thereafter parse accepts only this response or a
 * prior successful engine response; free final success with aimee_response_free. */
void kb_bedrock_response_init(aimee_response_t *response);
kb_bedrock_result_t kb_bedrock_nonstream_parse(const unsigned char *body, size_t body_len,
                                               aimee_response_t *response);

typedef int (*kb_bedrock_stream_callback_t)(const aimee_delta_t *delta, void *context);
typedef struct kb_bedrock_stream kb_bedrock_stream_t;

kb_bedrock_result_t kb_bedrock_stream_init(kb_bedrock_stream_t **stream,
                                           kb_bedrock_stream_callback_t callback, void *context);
kb_bedrock_result_t kb_bedrock_stream_feed(kb_bedrock_stream_t *stream, const unsigned char *bytes,
                                           size_t length);
kb_bedrock_result_t kb_bedrock_stream_finish(kb_bedrock_stream_t *stream);
kb_bedrock_result_t kb_bedrock_stream_clear(kb_bedrock_stream_t **stream);

/* Compatibility-only ABI. It performs no I/O and always fails closed. */
typedef struct
{
   const char *model_id, *region, *partition, *endpoint;
} kb_bedrock_target_t;
int kb_bedrock_dispatch_https(const kb_bedrock_target_t *, const aimee_request_t *, int,
                              const char *, const char *, const char *, const char *, const char *,
                              const char *, const char *, char *, size_t, int *);

#endif
