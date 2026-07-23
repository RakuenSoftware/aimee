#ifndef AIMEE_KB_MGMT_STATUS_LISTENER_H
#define AIMEE_KB_MGMT_STATUS_LISTENER_H

#include "kb_mgmt_status_peer.h"

#include <openssl/ssl.h>
#include <stddef.h>
#include <stdint.h>

#define KB_MGMT_STATUS_HTTP_HEADER_MAX   8192
#define KB_MGMT_STATUS_HTTP_BODY_MAX     1024
#define KB_MGMT_CHECKPOINT_HTTP_BODY_MAX 4096
#define KB_MGMT_STATUS_HTTP_HEADERS_MAX  32
#define KB_MGMT_STATUS_LISTENER_WORKERS  4
#define KB_MGMT_STATUS_LISTENER_QUEUE    32

typedef enum
{
   KB_MGMT_STATUS_LISTENER_OK = 0,
   KB_MGMT_STATUS_LISTENER_INVALID = 1,
   KB_MGMT_STATUS_LISTENER_DENIED = 2,
   KB_MGMT_STATUS_LISTENER_CONFLICT = 3,
   KB_MGMT_STATUS_LISTENER_UNAVAILABLE = 4,
} kb_mgmt_status_listener_result_t;

typedef kb_mgmt_status_listener_result_t (*kb_mgmt_status_listener_handler_fn)(
    size_t worker_id, const kb_mgmt_status_peer_t *peer, const char *request, size_t request_len,
    char *response, size_t response_cap, void *opaque);

typedef enum
{
   KB_MGMT_STATUS_ROUTE_STATUS = 1,
   KB_MGMT_STATUS_ROUTE_ACTION_CHECKPOINT = 2
} kb_mgmt_status_route_t;

typedef struct
{
   const char *bind_address;
   uint16_t port;
   SSL_CTX *tls;
   kb_mgmt_status_listener_handler_fn handle;
   kb_mgmt_status_listener_handler_fn handle_checkpoint;
   void *opaque;
} kb_mgmt_status_listener_config_t;

/* Dedicated, required-client-certificate context. The returned context neither
 * caches sessions nor permits tickets, early data, compression, renegotiation,
 * post-handshake authentication, or protocols other than TLS 1.2+. */
SSL_CTX *kb_mgmt_status_listener_tls_ctx(const char *trust_bundle_path,
                                         const char *server_cert_path, const char *server_key_path);

/* Start one authority listener with four joined workers and a bounded queue.
 * The config strings may be released after return; tls remains caller-owned and
 * must outlive stop. A singleton is deliberate: this process has one purpose. */
int kb_mgmt_status_listener_start(const kb_mgmt_status_listener_config_t *config);
uint16_t kb_mgmt_status_listener_bound_port(void);
void kb_mgmt_status_listener_stop(void);

/* Pure framing seam for focused and fuzz tests. COMPLETE supplies an exact
 * request body view; MORE means a valid prefix; BAD/TOO_LARGE are terminal. */
typedef enum
{
   KB_MGMT_STATUS_HTTP_MORE = 0,
   KB_MGMT_STATUS_HTTP_COMPLETE = 1,
   KB_MGMT_STATUS_HTTP_BAD = -1,
   KB_MGMT_STATUS_HTTP_TOO_LARGE = -2,
} kb_mgmt_status_http_result_t;
kb_mgmt_status_http_result_t kb_mgmt_status_http_parse(const unsigned char *input, size_t len,
                                                       const char **body, size_t *body_len);
kb_mgmt_status_http_result_t kb_mgmt_status_http_parse_route(const unsigned char *input, size_t len,
                                                             kb_mgmt_status_route_t *route,
                                                             const char **body, size_t *body_len);

#endif
