/* db1/pki_store.h: the certificate roster and the mTLS ramp row.
 *
 * Certificate generation and signing stay in the daemon, where the private key
 * is. This is what the server remembers: which certificates exist, when each
 * was last presented, and how far the ramp from optional to required mTLS has
 * come.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB1_PKI_STORE_H
#define DEC_DB1_PKI_STORE_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_PKI_SERIAL_MAX 128
#define DB1_PKI_CN_MAX     256
/* A SHA-256 of the roster, as hex, plus the NUL. */
#define DB1_PKI_HASH_LEN 65

   /* The same answers pki_cert_status_t gives, by the same values. They stay
    * distinct because a caller admits a connection on one and refuses it on
    * every other, and "expired" and "revoked" are different things to log. */
   typedef enum
   {
      DB1_PKI_CERT_VALID = 0,
      DB1_PKI_CERT_REVOKED = 1,
      DB1_PKI_CERT_EXPIRED = 2,
      DB1_PKI_CERT_UNKNOWN = 3,
      DB1_PKI_CERT_ERROR = 4
   } db1_pki_cert_status_t;

   typedef struct
   {
      char serial[DB1_PKI_SERIAL_MAX];
      char cn[DB1_PKI_CN_MAX];
      int64_t issued_at;
      int64_t expires_at;
      int revoked;
   } db1_pki_cert_t;

   /* Remember an issued certificate. */
   int db1_pki_cert_upsert(const char *serial, const char *cn, long issued_at, long expires_at);

   /* The roster, newest first. Returns the count written, or -1. */
   int db1_pki_cert_list(db1_pki_cert_t *out, int max);

   /* The serials that are revoked, for the caller's in-memory snapshot. The
    * snapshot is a cache the store cannot see, so it is filled rather than
    * consulted here. Returns the count written, or -1. */
   int db1_pki_revoked_serials(char (*out)[DB1_PKI_SERIAL_MAX], int max);

   /* Mark one revoked. */
   int db1_pki_cert_revoke(const char *serial);

   /* What the roster says about a serial right now. Answers the enum above. */
   int db1_pki_cert_check(const char *serial, long now);

   /* Record that a certificate was presented, which is what the ramp waits for. */
   int db1_pki_note_presentation(const char *serial, long now);

   /* The ramp. init returns the persisted mode (or -1); ready and advance
    * answer 1 when the roster is ready, 0 when it is not, -1 on a store
    * failure. The roster hash is taken inside the same transaction that may
    * advance, so a certificate added mid-decision cannot be missed. */
   int db1_pki_ramp_init(int configured_mode);
   int db1_pki_ramp_ready(long now);
   int db1_pki_ramp_advance(long now);
   int db1_pki_ramp_get(int *state_out, char *hash_out, size_t hash_len, long *advanced_at_out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_PKI_STORE_H */
