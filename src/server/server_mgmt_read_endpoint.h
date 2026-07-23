#ifndef AIMEE_SERVER_MGMT_READ_ENDPOINT_H
#define AIMEE_SERVER_MGMT_READ_ENDPOINT_H

#include "server_mgmt_endpoint.h"
#include "management_read.h"

typedef enum
{
   SERVER_MGMT_READ_OK = 0,
   SERVER_MGMT_READ_FORBIDDEN,
   SERVER_MGMT_READ_CONFLICT,
   SERVER_MGMT_READ_INTEGRITY,
   SERVER_MGMT_READ_UNAVAILABLE
} server_mgmt_read_result_t;

typedef struct
{
   unsigned char nonce[32];
   uint64_t revocation_generation;
   char staple_sha256[65];
} server_mgmt_read_status_proof_t;

typedef struct
{
   const char *jwt;
   size_t jwt_len;
   const char *staple;
   size_t staple_len;
   const char *expected_issuer;
   const char *server_id;
   const server_tls_peer_cert_t *peer;
   const char *local_issuer;
   const char *local_serial;
   const char *local_fingerprint;
   uint64_t publication_generation;
   int64_t now;
} server_mgmt_read_request_t;

typedef server_mgmt_read_result_t (*server_mgmt_read_status_fn)(void *,
                                                                const server_mgmt_read_request_t *,
                                                                server_mgmt_read_status_proof_t *);
typedef server_mgmt_read_result_t (*server_mgmt_read_token_fn)(void *,
                                                               const server_mgmt_read_request_t *,
                                                               server_mgmt_token_claims_t *);
typedef server_mgmt_read_result_t (*server_mgmt_read_checkpoint_fn)(
    void *, const server_mgmt_read_request_t *, const server_mgmt_token_claims_t *,
    const server_mgmt_read_status_proof_t *);
typedef int (*server_mgmt_read_load_fn)(void *, server_mgmt_read_agent_t *, size_t, size_t *);

typedef struct
{
   server_mgmt_read_status_fn verify_and_consume_status;
   server_mgmt_read_token_fn verify_token;
   server_mgmt_endpoint_jti_fn consume_jti;
   server_mgmt_read_load_fn load_agents;
   server_mgmt_read_checkpoint_fn verify_checkpoint;
   void *ctx;
} server_mgmt_read_deps_t;

server_mgmt_read_result_t server_mgmt_read_dispatch(const server_mgmt_read_request_t *,
                                                    const server_mgmt_read_deps_t *, char *, size_t,
                                                    size_t *);

#endif
