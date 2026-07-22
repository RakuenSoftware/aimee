#ifndef AIMEE_SERVER_MGMT_TOKEN_H
#define AIMEE_SERVER_MGMT_TOKEN_H

#include <stddef.h>
#include <stdint.h>

#define SERVER_MGMT_TOKEN_MAX_LIFETIME 90

typedef struct
{
   int version;
   char issuer[256];
   char audience[128];
   char subject[577];
   int64_t team_id;
   char capability[65];
   char jti[129];
   char correlation_id[129];
   char request_sha256[65];
   char peer_issuer[512];
   char peer_serial[80];
   char peer_fingerprint[65];
   char kid[65];
   int64_t issued_at;
   int64_t expires_at;
} server_mgmt_token_claims_t;

/* Verify an authenticated, caller-supplied management JWKS and return one
 * fully typed claim tuple. No caller may reparse the compact token. */
int server_mgmt_token_verify(const char *jwt, size_t jwt_len, const char *jwks_json,
                             const char *expected_issuer, const char *expected_audience,
                             const char *peer_issuer, const char *peer_serial,
                             const char *peer_fingerprint, const char *request_sha256, int64_t now,
                             server_mgmt_token_claims_t *out);

#endif
