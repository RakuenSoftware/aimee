#ifndef KB_BEDROCK_EGRESS_H
#define KB_BEDROCK_EGRESS_H

#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_ir_stream.h>
#include "modules/db2/c/org_model_catalog.h"
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
   KB_BEDROCK_TRANSPORT_ERROR,
   KB_BEDROCK_BUSY,
   KB_BEDROCK_POISONED,
   KB_BEDROCK_INTERNAL_ERROR
} kb_bedrock_result_t;

typedef struct
{
   const char *access_key_id;
   size_t access_key_id_len;
   const char *secret_access_key;
   size_t secret_access_key_len;
   const char *session_token;
   size_t session_token_len;
   const char *amz_date;
   const char *date;
} kb_bedrock_credentials_t;

typedef struct
{
   const unsigned char *access_key_id;
   size_t access_key_id_len;
   const unsigned char *secret_access_key;
   size_t secret_access_key_len;
   const unsigned char *session_token;
   size_t session_token_len;
   const char *amz_date;
   const char *date;
} kb_bedrock_credential_view_t;

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
/* Serialize the exact bounded Converse body used for digesting and later signing. */
kb_bedrock_result_t kb_bedrock_canonical_body(const aimee_request_t *ir, char **body,
                                              size_t *body_len);
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
typedef struct kb_bedrock_authorized_target kb_bedrock_authorized_target_t;

kb_bedrock_result_t kb_bedrock_stream_init(kb_bedrock_stream_t **stream,
                                           kb_bedrock_stream_callback_t callback, void *context);
kb_bedrock_result_t kb_bedrock_stream_feed(kb_bedrock_stream_t *stream, const unsigned char *bytes,
                                           size_t length);
kb_bedrock_result_t kb_bedrock_stream_finish(kb_bedrock_stream_t *stream);
kb_bedrock_result_t kb_bedrock_stream_clear(kb_bedrock_stream_t **stream);

/* Resolve an entitled catalog model inside the caller's already-active actor/team tenant scope.
 * The returned target has one owner, is non-copyable, and must be cleared exactly once.  The
 * owner must externally synchronize clear and call it only after every dispatch using the target
 * has returned.  Its catalog contents and lifecycle are deliberately opaque so a network caller
 * cannot substitute a raw db2 target. */
kb_bedrock_result_t kb_bedrock_authorized_target_resolve(int64_t team_id, const char *model_id,
                                                         kb_bedrock_authorized_target_t **target);
void kb_bedrock_authorized_target_clear(kb_bedrock_authorized_target_t **target);

/* Vault-safe split: build/sign while the borrowed credential view is live, then
 * dispatch the owned credential-free request only after the vault callback returns. */
kb_bedrock_result_t
kb_bedrock_authorized_wire_build(kb_bedrock_authorized_target_t *target, const aimee_request_t *ir,
                                 const kb_bedrock_credential_view_t *credentials,
                                 kb_bedrock_wire_request_t *request);
kb_bedrock_result_t kb_bedrock_authorized_wire_dispatch(kb_bedrock_authorized_target_t *target,
                                                        kb_bedrock_wire_request_t *request,
                                                        aimee_response_t *response,
                                                        int *http_status,
                                                        int *vendor_bytes_possible);

/* Production dispatch derives DNS, Host, SNI, and certificate authority solely from the
 * resolver-issued target.
 * `response` must have been initialized with kb_bedrock_response_init (or be a prior dispatch
 * output); success is caller-owned and released with aimee_response_free.  `http_status` remains
 * zero for every transport, framing, media, semantic, or callback failure.  A status is published
 * only after authenticated TLS EOF and complete response framing: 200 on semantic success, or the
 * validated non-2xx status with KB_BEDROCK_PROVIDER_ERROR. */
kb_bedrock_result_t kb_bedrock_dispatch_buffered(kb_bedrock_authorized_target_t *target,
                                                 const aimee_request_t *request,
                                                 const kb_bedrock_credentials_t *credentials,
                                                 aimee_response_t *response, int *http_status);
kb_bedrock_result_t kb_bedrock_dispatch_stream(kb_bedrock_authorized_target_t *target,
                                               const aimee_request_t *request,
                                               const kb_bedrock_credentials_t *credentials,
                                               kb_bedrock_stream_callback_t callback,
                                               void *callback_context, int *http_status);

#endif
