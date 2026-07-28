/* kb_http_client.h -- strict, one-shot, binary-safe HTTPS/1.1 client for kb egress. */
#ifndef DEC_KB_HTTP_CLIENT_H
#define DEC_KB_HTTP_CLIENT_H 1

#include <stddef.h>

#define KB_HTTP_HEADER_BLOCK_MAX    (32U * 1024U)
#define KB_HTTP_HEADER_LINE_MAX     8192U
#define KB_HTTP_HEADER_COUNT_MAX    64U
#define KB_HTTP_CHUNK_LINE_MAX      128U
#define KB_HTTP_BODY_MAX            (16U * 1024U * 1024U)
#define KB_HTTP_REQUEST_HEADERS_MAX 32U

typedef enum
{
   KB_HTTP_OK = 0,
   KB_HTTP_MORE = 1,
   KB_HTTP_INVALID_ARGUMENT = -1,
   KB_HTTP_RESOLVE_ERROR = -2,
   KB_HTTP_CONNECT_ERROR = -3,
   KB_HTTP_TLS_ERROR = -4,
   KB_HTTP_IO_ERROR = -5,
   KB_HTTP_TIMEOUT = -6,
   KB_HTTP_MALFORMED_RESPONSE = -7,
   KB_HTTP_TOO_LARGE = -8,
   KB_HTTP_CALLBACK_ABORT = -9,
   KB_HTTP_INTERNAL_ERROR = -10
} kb_http_result_t;

typedef enum
{
   KB_HTTP_GATE_DELIVER = 0,
   KB_HTTP_GATE_DISCARD = 1,
   KB_HTTP_GATE_ABORT = 2
} kb_http_gate_t;

typedef enum
{
   KB_HTTP_FRAMING_NONE = 0,
   KB_HTTP_FRAMING_CONTENT_LENGTH,
   KB_HTTP_FRAMING_CHUNKED
} kb_http_framing_t;

typedef struct
{
   const char *name;
   const char *value;
} kb_http_header_t;

typedef struct
{
   int status;
   kb_http_framing_t framing;
   size_t content_length;
   char content_type[128];
} kb_http_response_t;

/* Callbacks execute synchronously on the exchange thread. They must not block,
 * retain borrowed pointers, or re-enter the exchange; return promptly. */
typedef kb_http_gate_t (*kb_http_headers_fn)(const kb_http_response_t *response, void *context);
typedef enum
{
   KB_HTTP_BODY_CONTINUE = 0,
   KB_HTTP_BODY_CALLER_ABORT = 1
} kb_http_body_action_t;

typedef kb_http_body_action_t (*kb_http_body_fn)(const unsigned char *bytes, size_t length,
                                                 void *context);

typedef struct
{
   const char *authority; /* DNS destination, Host, SNI, and verification name; port is 443. */
   const char *method;
   const char *target;
   const kb_http_header_t *headers;
   size_t header_count;
   const unsigned char *body;
   size_t body_len;
   size_t response_body_max;
   int connect_timeout_ms;
   int total_timeout_ms;
   /* Deliver the response body even when the status is not 2xx.
    *
    * OFF BY DEFAULT, and that default is the right one: for ordinary egress an error
    * body is attacker-influenced content nobody reads, so the parser discards it
    * rather than buffering it.
    *
    * The exception is a protocol where the error body IS the protocol. RFC 6749 §5.2
    * specifies the OAuth token endpoint's failure as a 400 carrying
    * {"error":"invalid_grant", ...}; discarding it leaves a caller unable to tell "the
    * IdP rejected this code" from "the IdP sent us something unparseable". Set only by
    * the token exchange, and found to be necessary by running against a real
    * identity provider — the discard silently made that distinction unreachable. */
   int deliver_error_body;
} kb_http_request_t;

/* Pure request validation used by exchange and focused tests. */
kb_http_result_t kb_http_request_validate(const kb_http_request_t *request);

/* Opaque strict response parser. Exposed so framing can be exhaustively tested without I/O.
 * feed returns KB_HTTP_MORE until EOF, or a terminal error/abort. finish_eof succeeds only
 * after exact Content-Length or the complete zero-chunk + empty-trailer terminator. */
typedef struct kb_http_response_parser kb_http_response_parser_t;
/* Opt the parser into delivering a non-2xx body (see
 * kb_http_request_t.deliver_error_body). Set by kb_http_tls_exchange from the
 * request; exposed so the framing tests can exercise both settings without I/O. */
void kb_http_response_parser_deliver_error_body(kb_http_response_parser_t *parser, int deliver);

kb_http_result_t kb_http_response_parser_init(kb_http_response_parser_t **parser, size_t body_max,
                                              kb_http_headers_fn headers_cb,
                                              kb_http_body_fn body_cb, void *context);
kb_http_result_t kb_http_response_parser_feed(kb_http_response_parser_t *parser,
                                              const unsigned char *bytes, size_t length);
kb_http_result_t kb_http_response_parser_finish_eof(kb_http_response_parser_t *parser);
void kb_http_response_parser_free(kb_http_response_parser_t **parser);

/* Performs exactly one HTTPS POST exchange. `response` is zero on every failure. */
kb_http_result_t kb_http_tls_exchange(const kb_http_request_t *request,
                                      kb_http_response_t *response, kb_http_headers_fn headers_cb,
                                      kb_http_body_fn body_cb, void *context);

/* Private, hidden implementation test hooks.  These are not production ABI. */
#ifdef KB_HTTP_CLIENT_TESTING
int kb_http_client_test__tls_eof_is_authenticated(int ssl_error);
kb_http_result_t kb_http_client_test__wait_fd(int fd, short events, int timeout_ms);
int kb_http_client_test__nosigpipe_bio_write(int fd);
int kb_http_client_test__nosigpipe_ssl_fd(int fd);
kb_http_result_t kb_http_client_test__resolve(const char *host, int timeout_ms, int hang);
kb_http_result_t kb_http_client_test__dns_wait_idle(int timeout_ms, size_t *high_water);
kb_http_result_t kb_http_client_test__dns_slots(size_t *used, size_t *high_water);
kb_http_result_t kb_http_client_test__parse_resolver_response(const unsigned char *wire,
                                                              size_t length);
#endif

#endif
