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

typedef int (*kb_mgmt_status_lookup_fn)(const char *issuer, const char *serial,
                                        const char *fingerprint, const char *target,
                                        const char *purpose, int64_t *generation,
                                        char *target_fingerprint, size_t target_fingerprint_len,
                                        void *ctx);
typedef int (*kb_mgmt_status_sign_fn)(kb_mgmt_status_t *status, void *ctx);

/* Pure authority decision. Peer identity must come from verified mTLS. The
 * signing callback is the only private-key seam and is expected to enter the
 * P7 custodial key-use boundary in the dedicated authority process. */
int kb_mgmt_status_authority_issue(const kb_mgmt_status_request_t *, const char *peer_issuer,
                                   const char *peer_serial_norm, const char *peer_fingerprint,
                                   const char *key_id, uint64_t now,
                                   kb_mgmt_status_lookup_fn, void *lookup_ctx,
                                   kb_mgmt_status_sign_fn, void *sign_ctx,
                                   kb_mgmt_status_t *out);

#endif
