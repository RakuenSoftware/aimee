/* management_client_instance.h: P5-B2b typed management-instance DB boundary. */
#ifndef AIMEE_KB_STORE_MANAGEMENT_CLIENT_INSTANCE_H
#define AIMEE_KB_STORE_MANAGEMENT_CLIENT_INSTANCE_H

#include <stddef.h>
#include <stdint.h>

#define KB_STORE_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX   600U
#define KB_STORE_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN 32U
#define KB_STORE_MANAGEMENT_CLIENT_INSTANCE_DIGEST_LEN 32U
#define KB_STORE_MANAGEMENT_CLIENT_INSTANCE_ID_HEX     64U
#define KB_STORE_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX 128U

typedef enum
{
   KB_STORE_MANAGEMENT_CLIENT_INSTANCE_OK = 0,
   KB_STORE_MANAGEMENT_CLIENT_INSTANCE_INVALID,
   KB_STORE_MANAGEMENT_CLIENT_INSTANCE_DENIED,
   KB_STORE_MANAGEMENT_CLIENT_INSTANCE_CONFLICT,
   KB_STORE_MANAGEMENT_CLIENT_INSTANCE_RETRY,
   KB_STORE_MANAGEMENT_CLIENT_INSTANCE_INTEGRITY,
   KB_STORE_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE
} kb_store_management_client_instance_result_t;

typedef struct
{
   char issuer[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char subject[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   uint8_t proof_anchor[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN];
   uint8_t custody_anchor[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN];
   uint8_t binding_digest[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_DIGEST_LEN];
} kb_store_management_client_instance_binding_t;

typedef enum
{
   KB_STORE_MANAGEMENT_CLIENT_ISSUE_INITIAL = 1,
   KB_STORE_MANAGEMENT_CLIENT_ISSUE_RENEW
} kb_store_management_client_issue_kind_t;

typedef enum
{
   KB_STORE_MANAGEMENT_CLIENT_ISSUE_PENDING = 1,
   KB_STORE_MANAGEMENT_CLIENT_ISSUE_ACTIVE,
   KB_STORE_MANAGEMENT_CLIENT_ISSUE_EXPIRED,
   KB_STORE_MANAGEMENT_CLIENT_ISSUE_QUARANTINED
} kb_store_management_client_issue_state_t;

typedef struct
{
   char installation_id[33];
   kb_store_management_client_instance_binding_t binding;
} kb_store_management_client_grant_preflight_request_t;

typedef struct
{
   char installation_id[33];
   char replacement_lineage_id[33];
   int64_t expires_at_epoch;
} kb_store_management_client_grant_preflight_t;

typedef struct
{
   char operation_id[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_ID_HEX + 1];
   char authority_id[33];
   char installation_id[33];
   char expected_lineage_id[33];
   kb_store_management_client_instance_binding_t binding;
   uint8_t csr_digest[32];
   uint8_t csr_spki_digest[32];
} kb_store_management_client_initial_request_t;

typedef struct
{
   char operation_id[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_ID_HEX + 1];
   char installation_id[33];
   kb_store_management_client_instance_binding_t binding;
   int64_t generation;
   int64_t previous_enrollment_id;
   char previous_cert_issuer[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char previous_cert_serial_norm[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t previous_cert_fingerprint[32];
   uint8_t csr_digest[32];
   uint8_t csr_spki_digest[32];
} kb_store_management_client_renewal_request_t;

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
   kb_store_management_client_issue_kind_t issue_kind;
   kb_store_management_client_issue_state_t issue_state;
   int has_previous;
   int64_t previous_enrollment_id;
   char previous_cert_issuer[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char previous_cert_serial_norm[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t previous_cert_fingerprint[32];
   uint8_t csr_digest[32];
   uint8_t csr_spki_digest[32];
   int64_t pending_expires_at_epoch;
} kb_store_management_client_pending_t;

typedef struct
{
   char operation_id[65];
   char installation_id[33];
   kb_store_management_client_instance_binding_t binding;
   kb_store_management_client_issue_kind_t issue_kind;
   int64_t generation;
   int has_previous;
   int64_t previous_enrollment_id;
   char previous_cert_issuer[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char previous_cert_serial_norm[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t previous_cert_fingerprint[32];
   uint8_t csr_digest[32], csr_spki_digest[32], public_bundle_digest[32];
   char verified_ca_issuer[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   uint8_t verified_ca_fingerprint[32];
   char leaf_issuer[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char leaf_serial_norm[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t leaf_fingerprint[32], leaf_spki_digest[32];
   int64_t leaf_not_before_epoch, leaf_not_after_epoch;
} kb_store_management_client_activation_request_t;

typedef struct
{
   int replayed;
   char installation_id[33], replacement_lineage_id[33], authority_id[33];
   int64_t team_id, generation, enrollment_id;
   uint8_t binding_digest[32];
   char operation_id[65];
   kb_store_management_client_issue_kind_t issue_kind;
   kb_store_management_client_issue_state_t issue_state;
   uint8_t csr_digest[32], csr_spki_digest[32], public_bundle_digest[32];
   char cert_identity[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char cert_issuer[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1];
   char cert_serial_norm[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_SERIAL_MAX + 1];
   uint8_t cert_fingerprint[32], cert_spki_digest[32];
   int64_t cert_not_before_epoch, cert_not_after_epoch, revocation_generation, activated_at_epoch;
} kb_store_management_client_active_t;

typedef struct
{
   int64_t expired_grants, expired_issues, quarantined_issues;
} kb_store_management_client_maintenance_t;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Stable SQLSTATE-only classification used by every B2b SQL facade. */
   kb_store_management_client_instance_result_t
   kb_store_management_client_instance_classify_sqlstate(const char *sqlstate);

   /* Compute the canonical v1 binding transcript. `out` is cleared on entry and
    * remains clear on every failure. Issuer and subject must be printable ASCII,
    * non-empty and at most TEXT_MAX bytes; no terminal NUL is hashed. */
   kb_store_management_client_instance_result_t kb_store_management_client_instance_binding_digest(
       const char *issuer, const char *subject,
       const uint8_t proof_anchor[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN],
       const uint8_t custody_anchor[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN],
       uint8_t out[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_DIGEST_LEN]);

   /* Construct the fixed-bound representation consumed by future typed SQL
    * facades. Rejects truncation and clears `out` on every failure. */
   kb_store_management_client_instance_result_t kb_store_management_client_instance_binding_init(
       const char *issuer, const char *subject,
       const uint8_t proof_anchor[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN],
       const uint8_t custody_anchor[KB_STORE_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN],
       kb_store_management_client_instance_binding_t *out);

   kb_store_management_client_instance_result_t kb_store_management_client_instance_grant_preflight(
       const kb_store_management_client_grant_preflight_request_t *,
       kb_store_management_client_grant_preflight_t *);
   kb_store_management_client_instance_result_t kb_store_management_client_instance_begin_initial(
       const kb_store_management_client_initial_request_t *,
       kb_store_management_client_pending_t *);
   kb_store_management_client_instance_result_t kb_store_management_client_instance_begin_renewal(
       const kb_store_management_client_renewal_request_t *,
       kb_store_management_client_pending_t *);
   kb_store_management_client_instance_result_t kb_store_management_client_instance_activate(
       const kb_store_management_client_activation_request_t *,
       kb_store_management_client_active_t *);
   kb_store_management_client_instance_result_t kb_store_management_client_instance_snapshot(
       const char installation_id[33], const kb_store_management_client_instance_binding_t *,
       kb_store_management_client_active_t *);
   kb_store_management_client_instance_result_t
   kb_store_management_client_instance_expire_quarantine(
       int limit, kb_store_management_client_maintenance_t *);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_KB_STORE_MANAGEMENT_CLIENT_INSTANCE_H */
