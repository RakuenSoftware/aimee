#ifndef AIMEE_SERVER_MGMT_ENDPOINT_H
#define AIMEE_SERVER_MGMT_ENDPOINT_H

#include "server_mgmt_token.h"
#include "server_tls.h"
#include <stddef.h>
#include <stdint.h>

#define SERVER_MGMT_ACTION_BODY_MAX 256

typedef struct
{
   char action[14];
   char agent[64];
   char canonical[112];
   size_t canonical_len;
   char digest[65];
} server_mgmt_action_t;

typedef struct
{
   const char *body;
   size_t body_len;
   const char *jwt;
   size_t jwt_len;
   const char *staple;
   size_t staple_len;
   const char *expected_issuer;
   const char *server_id;
   const server_tls_peer_cert_t *peer;
   const char *local_fingerprint;
   int64_t now;
} server_mgmt_endpoint_request_t;

typedef enum
{
   SERVER_MGMT_CHECKPOINT_OK = 0,
   SERVER_MGMT_CHECKPOINT_DENIED = 1,
   SERVER_MGMT_CHECKPOINT_UNAVAILABLE = 2
} server_mgmt_checkpoint_result_t;

typedef enum
{
   SERVER_MGMT_JTI_OK = 0,
   SERVER_MGMT_JTI_REPLAY = 1,
   SERVER_MGMT_JTI_FAILED = 2
} server_mgmt_endpoint_jti_result_t;

typedef int (*server_mgmt_endpoint_token_fn)(void *, const server_mgmt_endpoint_request_t *,
                                             const char *, server_mgmt_token_claims_t *);
typedef int (*server_mgmt_endpoint_staple_fn)(void *, const server_mgmt_endpoint_request_t *,
                                              uint64_t *, char staple_digest[65]);
typedef server_mgmt_checkpoint_result_t (*server_mgmt_endpoint_checkpoint_fn)(
    void *, const server_mgmt_endpoint_request_t *, const server_mgmt_token_claims_t *, uint64_t,
    const char *);
typedef server_mgmt_endpoint_jti_result_t (*server_mgmt_endpoint_jti_fn)(
    void *, const server_mgmt_endpoint_request_t *, const server_mgmt_token_claims_t *);
typedef int (*server_mgmt_endpoint_remote_writes_fn)(void *);
typedef int (*server_mgmt_endpoint_audit_fn)(void *, const server_mgmt_token_claims_t *,
                                             const server_mgmt_action_t *, int, int);
typedef int (*server_mgmt_endpoint_action_fn)(void *, const server_mgmt_action_t *);
/* apply returns 0=applied, 1=proved no effect, 2=effect unknown. */

typedef struct
{
   server_mgmt_endpoint_token_fn verify_token;
   server_mgmt_endpoint_staple_fn verify_and_consume_staple;
   server_mgmt_endpoint_checkpoint_fn verify_checkpoint;
   server_mgmt_endpoint_jti_fn consume_jti;
   server_mgmt_endpoint_remote_writes_fn remote_writes;
   server_mgmt_endpoint_audit_fn audit;
   server_mgmt_endpoint_action_fn apply;
   void *ctx;
} server_mgmt_endpoint_deps_t;

typedef struct
{
   int status;
   const char *result;
   const char *effect;
} server_mgmt_endpoint_result_t;

int server_mgmt_action_parse(const char *body, size_t body_len, server_mgmt_action_t *out);
int server_mgmt_endpoint_dispatch(const server_mgmt_endpoint_request_t *,
                                  const server_mgmt_endpoint_deps_t *,
                                  server_mgmt_endpoint_result_t *);
int server_mgmt_endpoint_render(const server_mgmt_endpoint_result_t *, char *, size_t);

/* Production strict status-authority checkpoint client. */
server_mgmt_checkpoint_result_t
server_mgmt_checkpoint_client_verify(const server_mgmt_endpoint_request_t *,
                                     const server_mgmt_token_claims_t *, uint64_t,
                                     const char *staple_digest);

#endif
