#ifndef AIMEE_SERVER_MGMT_CHECKPOINT_CLIENT_H
#define AIMEE_SERVER_MGMT_CHECKPOINT_CLIENT_H

#include "server_http.h"
#include "server_mgmt_endpoint.h"

#define SERVER_MGMT_CHECKPOINT_PEM_MAX 65536

typedef struct
{
   const char *endpoint;
   const char *ca_pem;
   const char *client_cert_pem;
   const char *client_key_pem;
   const char *leaf_pin;
   const char *secondary_leaf_pin;
   const char *key_id;
   const unsigned char *public_key;
} server_mgmt_checkpoint_material_t;

typedef int (*server_mgmt_checkpoint_transport_fn)(
    void *, const server_mgmt_checkpoint_material_t *, const char *, uint64_t, char *, size_t,
    int *);

int server_mgmt_checkpoint_client_start(const server_http_management_config_t *);
void server_mgmt_checkpoint_client_stop(void);
int server_mgmt_checkpoint_pin_matches(const char *, const char *, const char *);
int server_mgmt_checkpoint_request_build(const server_mgmt_endpoint_request_t *,
                                         const server_mgmt_token_claims_t *, uint64_t,
                                         const char *, char *, size_t, char digest[65]);
server_mgmt_checkpoint_result_t server_mgmt_checkpoint_client_verify_with(
    const server_mgmt_checkpoint_material_t *, server_mgmt_checkpoint_transport_fn, void *,
    const server_mgmt_endpoint_request_t *, const server_mgmt_token_claims_t *, uint64_t,
    const char *);

#endif
