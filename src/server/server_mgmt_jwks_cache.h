#ifndef AIMEE_SERVER_MGMT_JWKS_CACHE_H
#define AIMEE_SERVER_MGMT_JWKS_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include "server_mgmt_token.h"

#define SERVER_MGMT_JWKS_BUNDLE_MAX   1536
#define SERVER_MGMT_JWKS_ENVELOPE_MAX 3072
#define SERVER_MGMT_JWKS_BYTES_MAX    1024

typedef enum
{
   SERVER_MGMT_JWKS_CACHE_OK = 0,
   SERVER_MGMT_JWKS_CACHE_INVALID = 1,
   SERVER_MGMT_JWKS_CACHE_STALE = 2,
   SERVER_MGMT_JWKS_CACHE_CONFLICT = 3,
   SERVER_MGMT_JWKS_CACHE_STORAGE = 4,
   SERVER_MGMT_JWKS_CACHE_MISSING = 5,
} server_mgmt_jwks_cache_result_t;

typedef struct
{
   int64_t generation;
   int64_t valid_from;
   int64_t valid_until;
   char jwks[SERVER_MGMT_JWKS_BYTES_MAX];
   size_t jwks_len;
   unsigned char envelope_sha256[32];
   unsigned char manifest_sha256[32];
   unsigned char trust_bundle_sha256[32];
} server_mgmt_jwks_cache_record_t;

int server_mgmt_jwks_trust_bundle_load(const char *absolute_path, char *out, size_t cap,
                                       size_t *out_len);

server_mgmt_jwks_cache_result_t
server_mgmt_jwks_envelope_validate(const char *trust_bundle, size_t trust_bundle_len,
                                   const char *envelope, size_t envelope_len, int64_t now,
                                   server_mgmt_jwks_cache_record_t *out);
server_mgmt_jwks_cache_result_t server_mgmt_jwks_cache_install(const char *trust_bundle,
                                                               size_t trust_bundle_len,
                                                               const char *envelope,
                                                               size_t envelope_len, int64_t now);
server_mgmt_jwks_cache_result_t server_mgmt_jwks_cache_load(const char *trust_bundle,
                                                            size_t trust_bundle_len, int64_t now,
                                                            char *jwks_out, size_t jwks_cap,
                                                            size_t *jwks_len);

typedef int (*server_mgmt_jwks_fetch_fn)(void *ctx, char *envelope_out, size_t envelope_cap,
                                         size_t *envelope_len);

server_mgmt_jwks_cache_result_t server_mgmt_jwks_cache_refresh(const char *trust_bundle,
                                                               size_t trust_bundle_len, int64_t now,
                                                               server_mgmt_jwks_fetch_fn fetch,
                                                               void *fetch_ctx);

server_mgmt_jwks_cache_result_t server_mgmt_jwks_cache_startup(const char *trust_bundle_path,
                                                               int64_t now,
                                                               server_mgmt_jwks_fetch_fn fetch,
                                                               void *fetch_ctx);

server_mgmt_token_result_t server_mgmt_token_verify_cached(
    const char *jwt, size_t jwt_len, const char *trust_bundle, size_t trust_bundle_len,
    const char *expected_issuer, const char *expected_audience, const char *peer_issuer,
    const char *peer_serial, const char *peer_fingerprint, const char *request_sha256, int64_t now,
    server_mgmt_jwks_fetch_fn fetch, void *fetch_ctx, server_mgmt_token_claims_t *out);

server_mgmt_token_result_t server_mgmt_token_verify_read_claims_cached(
    const char *jwt, size_t jwt_len, const char *trust_bundle, size_t trust_bundle_len,
    const char *expected_issuer, const char *expected_audience, const char *peer_issuer,
    const char *peer_serial, const char *peer_fingerprint, int64_t now,
    server_mgmt_jwks_fetch_fn fetch, void *fetch_ctx, server_mgmt_token_claims_t *out);

#endif
