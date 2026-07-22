/* management_client_instance.h: P5-B2b typed management-instance DB boundary. */
#ifndef AIMEE_DB2_MANAGEMENT_CLIENT_INSTANCE_H
#define AIMEE_DB2_MANAGEMENT_CLIENT_INSTANCE_H

#include <stddef.h>
#include <stdint.h>

#define DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX   600U
#define DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN 32U
#define DB2_MANAGEMENT_CLIENT_INSTANCE_DIGEST_LEN 32U

typedef enum
{
   DB2_MANAGEMENT_CLIENT_INSTANCE_OK = 0,
   DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID,
   DB2_MANAGEMENT_CLIENT_INSTANCE_DENIED,
   DB2_MANAGEMENT_CLIENT_INSTANCE_CONFLICT,
   DB2_MANAGEMENT_CLIENT_INSTANCE_RETRY,
   DB2_MANAGEMENT_CLIENT_INSTANCE_INTEGRITY,
   DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE
} db2_management_client_instance_result_t;

typedef struct
{
   char issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char subject[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   uint8_t proof_anchor[DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN];
   uint8_t custody_anchor[DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN];
   uint8_t binding_digest[DB2_MANAGEMENT_CLIENT_INSTANCE_DIGEST_LEN];
} db2_management_client_instance_binding_t;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Stable SQLSTATE-only classification used by every B2b SQL facade. */
   db2_management_client_instance_result_t
   db2_management_client_instance_classify_sqlstate(const char *sqlstate);

   /* Compute the canonical v1 binding transcript. `out` is cleared on entry and
    * remains clear on every failure. Issuer and subject must be printable ASCII,
    * non-empty and at most TEXT_MAX bytes; no terminal NUL is hashed. */
   db2_management_client_instance_result_t db2_management_client_instance_binding_digest(
       const char *issuer, const char *subject,
       const uint8_t proof_anchor[DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN],
       const uint8_t custody_anchor[DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN],
       uint8_t out[DB2_MANAGEMENT_CLIENT_INSTANCE_DIGEST_LEN]);

   /* Construct the fixed-bound representation consumed by future typed SQL
    * facades. Rejects truncation and clears `out` on every failure. */
   db2_management_client_instance_result_t db2_management_client_instance_binding_init(
       const char *issuer, const char *subject,
       const uint8_t proof_anchor[DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN],
       const uint8_t custody_anchor[DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN],
       db2_management_client_instance_binding_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_MANAGEMENT_CLIENT_INSTANCE_H */
