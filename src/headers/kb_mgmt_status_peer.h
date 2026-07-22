/* kb_mgmt_status_peer.h: exact verified mTLS identity for the P5 status authority. */
#ifndef DEC_KB_MGMT_STATUS_PEER_H
#define DEC_KB_MGMT_STATUS_PEER_H 1

#include <openssl/ssl.h>

typedef struct
{
   char issuer[601];
   char serial_norm[129];
   char fingerprint[65];
} kb_mgmt_status_peer_t;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Extract the peer leaf only after a completed, chain-verified TLS handshake
    * and enforce the dedicated management-client certificate profile. Returns
    * 1 on success. All failures return 0 and leave `out` entirely zeroed. */
   int kb_mgmt_status_peer_verify(SSL *ssl, kb_mgmt_status_peer_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_MGMT_STATUS_PEER_H */
