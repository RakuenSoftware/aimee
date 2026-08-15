/* remote_client_grant.h: durable first-user remote-client authorization.
 *
 * The setup wizard owns the first interactive identity.  Its bearer is only an
 * enrollment credential; the standing write grant is attached to the mTLS
 * certificate produced by that enrollment.  DB1 keeps the binding local to the
 * server that terminates the certificate and enforces /v1 capabilities. */
#ifndef DEC_REMOTE_CLIENT_GRANT_H
#define DEC_REMOTE_CLIENT_GRANT_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_REMOTE_CLIENT_PRINCIPAL_MAX   255
#define DB1_REMOTE_CLIENT_BEARER_HASH_LEN 64
#define DB1_REMOTE_CLIENT_CERT_SERIAL_MAX 79

   typedef enum
   {
      DB1_REMOTE_CLIENT_CLAIM_NEW = 0,
      DB1_REMOTE_CLIENT_CLAIM_UNBOUND = 1,
      DB1_REMOTE_CLIENT_CLAIM_BOUND = 2,
      DB1_REMOTE_CLIENT_CLAIM_OWNED_BY_OTHER = 3,
      DB1_REMOTE_CLIENT_CLAIM_INVALID = 4,
      DB1_REMOTE_CLIENT_CLAIM_STORAGE = 5
   } db1_remote_client_claim_result_t;

   typedef struct
   {
      char principal[DB1_REMOTE_CLIENT_PRINCIPAL_MAX + 1];
      char bearer_sha256[DB1_REMOTE_CLIENT_BEARER_HASH_LEN + 1];
      char cert_serial[DB1_REMOTE_CLIENT_CERT_SERIAL_MAX + 1];
      int tier; /* 0=off, 1=data, 2=full */
   } db1_remote_client_grant_t;

   /* Atomically claim the appliance's first-user slot and create its unbound
    * enrollment record.  Re-entry by the same principal returns the existing
    * record; a different principal can never replace the first owner. */
   db1_remote_client_claim_result_t db1_remote_client_claim(const char *principal,
                                                            const char *new_bearer_sha256,
                                                            int64_t now,
                                                            db1_remote_client_grant_t *out);

   /* Remove a failed/orphaned enrollment only while it is still unbound. */
   int db1_remote_client_abandon(const char *bearer_sha256);

   /* Bind the one enrollment bearer to exactly one certificate serial.
    * Returns 1 when newly/idempotently bound, 0 when the bearer is not a wizard
    * enrollment, -2 when it was already bound elsewhere, and -1 on storage or
    * validation failure. */
   int db1_remote_client_bind(const char *bearer_sha256, const char *cert_serial, int64_t now);

   /* Resolve the explicit grant for a verified certificate.  Returns its tier,
    * 0 when no grant exists, or -1 on a storage/validation failure. */
   int db1_remote_client_tier(const char *cert_serial, char *principal, size_t principal_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_REMOTE_CLIENT_GRANT_H */
