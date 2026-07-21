#ifndef DEC_SERVER_MGMT_STATUS_H
#define DEC_SERVER_MGMT_STATUS_H 1

#include "kb_mgmt_status.h"
#include "server_tls.h"
#include <stdint.h>

typedef enum
{
   SERVER_MGMT_NONCE_OK = 0,
   SERVER_MGMT_NONCE_NOT_FOUND = -1,
   SERVER_MGMT_NONCE_MISMATCH = -2,
   SERVER_MGMT_NONCE_EXPIRED = -3,
   SERVER_MGMT_NONCE_ROLLBACK = -4,
   SERVER_MGMT_NONCE_INVALID = -5,
   SERVER_MGMT_NONCE_STORAGE = -6,
   SERVER_MGMT_NONCE_SATURATED = -7
} server_mgmt_nonce_result_t;

/* Delete restart-stale challenges while retaining the durable generation HWM. */
int server_mgmt_status_init(void);

int server_mgmt_nonce_issue(const server_tls_peer_cert_t *, const char *target_server_id,
                            uint64_t now, unsigned char nonce[KB_MGMT_STATUS_NONCE_LEN],
                            uint64_t *expires_at);

/* Delete the nonce and, only for a completely valid staple, compare/advance HWM
 * in the same SQLite transaction. Every non-storage result consumes the nonce. */
server_mgmt_nonce_result_t server_mgmt_nonce_consume(
    const kb_mgmt_status_t *, const server_tls_peer_cert_t *, const char *target_server_id,
    uint64_t now, int signature_and_shape_valid);

int server_mgmt_status_hwm(uint64_t *generation);

#endif
