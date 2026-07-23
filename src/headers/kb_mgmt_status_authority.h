#ifndef DEC_KB_MGMT_STATUS_AUTHORITY_H
#define DEC_KB_MGMT_STATUS_AUTHORITY_H 1

#include "kb_mgmt_status.h"
#include <stddef.h>
#include <stdint.h>

typedef struct
{
   unsigned char nonce[KB_MGMT_STATUS_NONCE_LEN];
   char target_server_id[128];
   char target_mgmt_fingerprint[65];
   char purpose[32];
} kb_mgmt_status_request_t;

typedef struct
{
   uint32_t version;
   unsigned char nonce[KB_MGMT_STATUS_NONCE_LEN];
   char caller_issuer[601];
   char caller_serial_norm[129];
   char caller_fingerprint[65];
   char target_server_id[128];
   uint64_t staple_generation;
   char staple_sha256[65];
   char correlation_id[65];
   char jti[65];
   char action_request_sha256[65];
   char canonical_sha256[65];
   /* Filled only after verified-mTLS admission; never part of request JSON. */
   char authenticated_peer_fingerprint[65];
} kb_mgmt_checkpoint_request_t;

#define KB_MGMT_STATUS_REQUEST_JSON_MAX 1024

typedef enum
{
   KB_MGMT_STATUS_AUTHORITY_OK = 0,
   KB_MGMT_STATUS_AUTHORITY_INVALID = 1,
   KB_MGMT_STATUS_AUTHORITY_DENIED = 2,
   KB_MGMT_STATUS_AUTHORITY_CONFLICT = 3,
   KB_MGMT_STATUS_AUTHORITY_UNAVAILABLE = 4,
   KB_MGMT_STATUS_AUTHORITY_INTEGRITY = 5,
} kb_mgmt_status_authority_result_t;

/* Callback return contract.  Lookup adapters may return DENIED for an
 * authoritative policy refusal, CONFLICT for an exact-use replay, or a
 * negative failure.  Sign adapters use OK, CONFLICT, or a negative failure. */
enum
{
   KB_MGMT_STATUS_CALLBACK_OK = 0,
   KB_MGMT_STATUS_CALLBACK_DENIED = 1,
   KB_MGMT_STATUS_CALLBACK_CONFLICT = 2,
   KB_MGMT_STATUS_CALLBACK_UNAVAILABLE = -1,
   KB_MGMT_STATUS_CALLBACK_INTEGRITY = -2,
};

typedef int (*kb_mgmt_status_lookup_fn)(const char *issuer, const char *serial,
                                        const char *fingerprint, const char *target,
                                        const char *purpose, int64_t *generation,
                                        char *target_fingerprint, size_t target_fingerprint_len,
                                        void *ctx);
typedef int (*kb_mgmt_status_sign_fn)(kb_mgmt_status_t *status, void *ctx);
typedef int (*kb_mgmt_checkpoint_lookup_fn)(const char *peer_issuer, const char *peer_serial,
                                            const char *peer_fingerprint,
                                            const kb_mgmt_checkpoint_request_t *request,
                                            int *revoked, int64_t *generation, void *ctx);
typedef int (*kb_mgmt_checkpoint_sign_fn)(kb_mgmt_checkpoint_t *checkpoint,
                                          const kb_mgmt_checkpoint_request_t *request, void *ctx);

/* Strict length-aware request decoder.  The body must be one JSON object with
 * exactly the four string fields nonce, target, target_mgmt_fp and purpose.
 * Embedded NUL, duplicate/unknown/missing fields and non-canonical base64url
 * are rejected.  out is cleared on every failure. */
kb_mgmt_status_authority_result_t kb_mgmt_status_request_from_json(const char *raw, size_t raw_len,
                                                                   kb_mgmt_status_request_t *out);

/* Pure authority decision. Peer identity must come from verified mTLS. The
 * signing callback is the only private-key seam and is expected to enter the
 * P7 custodial key-use boundary in the dedicated authority process. */
kb_mgmt_status_authority_result_t kb_mgmt_status_authority_issue(
    const kb_mgmt_status_request_t *, const char *peer_issuer, const char *peer_serial_norm,
    const char *peer_fingerprint, const char *key_id, uint64_t now, kb_mgmt_status_lookup_fn,
    void *lookup_ctx, kb_mgmt_status_sign_fn, void *sign_ctx, kb_mgmt_status_t *out);

kb_mgmt_status_authority_result_t
kb_mgmt_checkpoint_request_from_json(const char *raw, size_t raw_len,
                                     kb_mgmt_checkpoint_request_t *out);
kb_mgmt_status_authority_result_t kb_mgmt_checkpoint_authority_issue(
    const kb_mgmt_checkpoint_request_t *, const char *peer_issuer, const char *peer_serial_norm,
    const char *peer_fingerprint, const char *key_id, uint64_t now, kb_mgmt_checkpoint_lookup_fn,
    void *lookup_ctx, kb_mgmt_checkpoint_sign_fn, void *sign_ctx, kb_mgmt_checkpoint_t *out);

#endif
