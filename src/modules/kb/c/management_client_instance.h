/* management_client_instance.h: P5-B2b typed management-instance DB boundary. */
#ifndef AIMEE_DB2_MANAGEMENT_CLIENT_INSTANCE_H
#define AIMEE_DB2_MANAGEMENT_CLIENT_INSTANCE_H

#include <stddef.h>
#include <stdint.h>

#define DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX   600U
#define DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN 32U
#define DB2_MANAGEMENT_CLIENT_INSTANCE_DIGEST_LEN 32U
#define DB2_MANAGEMENT_CLIENT_INSTANCE_ID_HEX     64U
#define DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX 128U

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

typedef enum
{
   DB2_MANAGEMENT_CLIENT_ISSUE_INITIAL = 1,
   DB2_MANAGEMENT_CLIENT_ISSUE_RENEW
} db2_management_client_issue_kind_t;

typedef enum
{
   DB2_MANAGEMENT_CLIENT_ISSUE_PENDING = 1,
   DB2_MANAGEMENT_CLIENT_ISSUE_ACTIVE,
   DB2_MANAGEMENT_CLIENT_ISSUE_EXPIRED,
   DB2_MANAGEMENT_CLIENT_ISSUE_QUARANTINED
} db2_management_client_issue_state_t;

typedef struct
{
   char installation_id[33];
   db2_management_client_instance_binding_t binding;
} db2_management_client_grant_preflight_request_t;

typedef struct
{
   char installation_id[33];
   char replacement_lineage_id[33];
   int64_t expires_at_epoch;
} db2_management_client_grant_preflight_t;

typedef struct
{
   char operation_id[DB2_MANAGEMENT_CLIENT_INSTANCE_ID_HEX + 1];
   char authority_id[33];
   char installation_id[33];
   char expected_lineage_id[33];
   db2_management_client_instance_binding_t binding;
   uint8_t csr_digest[32];
   uint8_t csr_spki_digest[32];
} db2_management_client_initial_request_t;

typedef struct
{
   char operation_id[DB2_MANAGEMENT_CLIENT_INSTANCE_ID_HEX + 1];
   char installation_id[33];
   db2_management_client_instance_binding_t binding;
   int64_t generation;
   int64_t previous_enrollment_id;
   char previous_cert_issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char previous_cert_serial_norm[DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t previous_cert_fingerprint[32];
   uint8_t csr_digest[32];
   uint8_t csr_spki_digest[32];
} db2_management_client_renewal_request_t;

typedef struct
{
   int replayed;
   char installation_id[33];
   char replacement_lineage_id[33];
   char authority_id[33];
   int64_t team_id;
   uint8_t binding_digest[32];
   int64_t generation;
   char operation_id[65];
   db2_management_client_issue_kind_t issue_kind;
   db2_management_client_issue_state_t issue_state;
   int has_previous;
   int64_t previous_enrollment_id;
   char previous_cert_issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char previous_cert_serial_norm[DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t previous_cert_fingerprint[32];
   uint8_t csr_digest[32];
   uint8_t csr_spki_digest[32];
   int64_t pending_expires_at_epoch;
} db2_management_client_pending_t;

typedef struct
{
   char operation_id[65];
   char installation_id[33];
   db2_management_client_instance_binding_t binding;
   db2_management_client_issue_kind_t issue_kind;
   int64_t generation;
   int has_previous;
   int64_t previous_enrollment_id;
   char previous_cert_issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char previous_cert_serial_norm[DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t previous_cert_fingerprint[32];
   uint8_t csr_digest[32], csr_spki_digest[32], public_bundle_digest[32];
   char verified_ca_issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   uint8_t verified_ca_fingerprint[32];
   char leaf_issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char leaf_serial_norm[DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t leaf_fingerprint[32], leaf_spki_digest[32];
   int64_t leaf_not_before_epoch, leaf_not_after_epoch;
} db2_management_client_activation_request_t;

typedef struct
{
   int replayed;
   char installation_id[33], replacement_lineage_id[33], authority_id[33];
   int64_t team_id, generation, enrollment_id;
   uint8_t binding_digest[32];
   char operation_id[65];
   db2_management_client_issue_kind_t issue_kind;
   db2_management_client_issue_state_t issue_state;
   uint8_t csr_digest[32], csr_spki_digest[32], public_bundle_digest[32];
   char cert_identity[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char cert_issuer[DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char cert_serial_norm[DB2_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t cert_fingerprint[32], cert_spki_digest[32];
   int64_t cert_not_before_epoch, cert_not_after_epoch, revocation_generation, activated_at_epoch;
} db2_management_client_active_t;

typedef struct
{
   int64_t expired_grants, expired_issues, quarantined_issues;
} db2_management_client_maintenance_t;

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

   db2_management_client_instance_result_t db2_management_client_instance_grant_preflight(
       const db2_management_client_grant_preflight_request_t *,
       db2_management_client_grant_preflight_t *);
   db2_management_client_instance_result_t
   db2_management_client_instance_begin_initial(const db2_management_client_initial_request_t *,
                                                db2_management_client_pending_t *);
   db2_management_client_instance_result_t
   db2_management_client_instance_begin_renewal(const db2_management_client_renewal_request_t *,
                                                db2_management_client_pending_t *);
   db2_management_client_instance_result_t
   db2_management_client_instance_activate(const db2_management_client_activation_request_t *,
                                           db2_management_client_active_t *);
   db2_management_client_instance_result_t
   db2_management_client_instance_snapshot(const char installation_id[33],
                                           const db2_management_client_instance_binding_t *,
                                           db2_management_client_active_t *);
   db2_management_client_instance_result_t
   db2_management_client_instance_expire_quarantine(int limit,
                                                    db2_management_client_maintenance_t *);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_MANAGEMENT_CLIENT_INSTANCE_H */
