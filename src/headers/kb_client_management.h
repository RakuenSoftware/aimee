#ifndef KB_CLIENT_MANAGEMENT_H
#define KB_CLIENT_MANAGEMENT_H

#include "kb_mgmt_endpoint.h"

#include <openssl/ssl.h>
#include <stddef.h>
#include <stdint.h>

/* Shared, storage-neutral mTLS transport contract.  The KB owns the
 * implementation; server-plane callers may use this public client boundary. */
typedef struct
{
   kb_mgmt_endpoint_t endpoint;
   SSL_CTX *ctx;
   SSL *ssl;
   int fd;
} kb_mgmt_client_session_t;

typedef enum
{
   KB_MGMT_CLIENT_NOT_SENT = 0,
   KB_MGMT_CLIENT_SENT_RESPONSE,
   KB_MGMT_CLIENT_SENT_AMBIGUOUS
} kb_mgmt_client_send_result_t;

int kb_mgmt_client_session_open(kb_mgmt_client_session_t *, const char *endpoint, const char *ca,
                                const char *client_cert, const char *client_key,
                                const char *expected_issuer, const char *expected_serial_norm,
                                const char *expected_fingerprint);
int kb_mgmt_client_session_open_deadline(kb_mgmt_client_session_t *, const char *endpoint,
                                         const char *ca, const char *client_cert,
                                         const char *client_key, const char *expected_issuer,
                                         const char *expected_serial_norm,
                                         const char *expected_fingerprint, uint64_t deadline_millis,
                                         int trusted_addresses);
int kb_mgmt_client_session_request(kb_mgmt_client_session_t *, const char *method, const char *path,
                                   const char *body, const char *extra_headers, char *resp,
                                   size_t cap, int *status);
int kb_mgmt_client_session_request_deadline(kb_mgmt_client_session_t *, const char *method,
                                            const char *path, const char *body,
                                            const char *extra_headers, uint64_t deadline_millis,
                                            char *resp, size_t cap, int *status);
kb_mgmt_client_send_result_t
kb_mgmt_client_session_action_deadline(kb_mgmt_client_session_t *, const char *body,
                                       const char *extra_headers, uint64_t deadline_millis,
                                       char *resp, size_t cap, int *status);
int kb_mgmt_client_session_checkpoint_deadline(kb_mgmt_client_session_t *, const char *body,
                                               uint64_t deadline_millis, char *resp, size_t cap,
                                               int *status);
void kb_mgmt_client_session_close(kb_mgmt_client_session_t *);

int kb_mgmt_client_request(const char *endpoint, const char *ca, const char *client_cert,
                           const char *client_key, const char *method, const char *path,
                           const char *body, char *resp, size_t cap, int *status);
int kb_mgmt_client_request_auth(const char *endpoint, const char *ca, const char *client_cert,
                                const char *client_key, const char *method, const char *path,
                                const char *body, const char *authorization, char *resp, size_t cap,
                                int *status);

#endif
