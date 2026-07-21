#ifndef KB_MGMT_CLIENT_H
#define KB_MGMT_CLIENT_H
#include "kb_mgmt_endpoint.h"
#include <openssl/ssl.h>
#include <stddef.h>

typedef struct
{
   kb_mgmt_endpoint_t endpoint;
   SSL_CTX *ctx;
   SSL *ssl;
   int fd;
} kb_mgmt_client_session_t;

int kb_mgmt_client_session_open(kb_mgmt_client_session_t *, const char *endpoint, const char *ca,
                                const char *client_cert, const char *client_key,
                                const char *expected_issuer, const char *expected_serial_norm,
                                const char *expected_fingerprint);
int kb_mgmt_client_session_request(kb_mgmt_client_session_t *, const char *method, const char *path,
                                   const char *body, const char *extra_headers, char *resp,
                                   size_t cap, int *status);
void kb_mgmt_client_session_close(kb_mgmt_client_session_t *);

int kb_mgmt_client_request(const char *endpoint, const char *ca, const char *client_cert,
                           const char *client_key, const char *method, const char *path,
                           const char *body, char *resp, size_t cap, int *status);
int kb_mgmt_client_request_auth(const char *endpoint, const char *ca, const char *client_cert,
                                const char *client_key, const char *method, const char *path,
                                const char *body, const char *authorization, char *resp, size_t cap,
                                int *status);
#endif
