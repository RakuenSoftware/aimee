/* P5-C1c: typed boundary for the primary/WORM management-action journal. */
#ifndef AIMEE_KB_STORE_MANAGEMENT_ACTION_JOURNAL_H
#define AIMEE_KB_STORE_MANAGEMENT_ACTION_JOURNAL_H

#include "kb_identity.h"

#include <stddef.h>
#include <stdint.h>

#define KB_STORE_MANAGEMENT_ACTION_ID_HEX           64U
#define KB_STORE_MANAGEMENT_ACTION_SERVER_MAX       127U
#define KB_STORE_MANAGEMENT_ACTION_TOKEN_ISSUER_MAX 255U
#define KB_STORE_MANAGEMENT_ACTION_CERT_ISSUER_MAX  511U
#define KB_STORE_MANAGEMENT_ACTION_ACTOR_MAX        576U
#define KB_STORE_MANAGEMENT_ACTION_KID_MAX          64U
#define KB_STORE_MANAGEMENT_ACTION_INSTALL_ID_HEX   32U
#define KB_STORE_MANAGEMENT_ACTION_SERIAL_MAX       79U

typedef enum
{
   KB_STORE_MANAGEMENT_ACTION_OK = 0,
   KB_STORE_MANAGEMENT_ACTION_INVALID,
   KB_STORE_MANAGEMENT_ACTION_DENIED,
   KB_STORE_MANAGEMENT_ACTION_CONFLICT,
   KB_STORE_MANAGEMENT_ACTION_RETRY,
   KB_STORE_MANAGEMENT_ACTION_INTEGRITY,
   KB_STORE_MANAGEMENT_ACTION_UNAVAILABLE,
   /* The COMMIT acknowledgement was lost. Outputs are clear; retry the exact
    * caller-owned operation object, whose identifiers must not be regenerated. */
   KB_STORE_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS
} kb_store_management_action_result_t;

typedef enum
{
   KB_STORE_MANAGEMENT_ACTION_CAP_REMOTE_WRITES = 1
} kb_store_management_action_capability_t;

typedef enum
{
   KB_STORE_MANAGEMENT_ACTION_SUCCEEDED = 1,
   KB_STORE_MANAGEMENT_ACTION_DENIED_RESULT,
   KB_STORE_MANAGEMENT_ACTION_FAILED,
   KB_STORE_MANAGEMENT_ACTION_INDETERMINATE
} kb_store_management_action_outcome_result_t;

typedef enum
{
   KB_STORE_MANAGEMENT_ACTION_CLASS_REMOTE_SUCCESS = 1,
   KB_STORE_MANAGEMENT_ACTION_CLASS_REMOTE_DENIED,
   KB_STORE_MANAGEMENT_ACTION_CLASS_REMOTE_FAILURE,
   KB_STORE_MANAGEMENT_ACTION_CLASS_TRANSPORT_AMBIGUOUS,
   KB_STORE_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE,
   KB_STORE_MANAGEMENT_ACTION_CLASS_LOCAL_FAILURE
} kb_store_management_action_outcome_class_t;

typedef enum
{
   /* C1c is only a durable authorization record. C2/C3 must independently bind
    * the kid to custody and recheck every snapshot before mint or dispatch. */
   KB_STORE_MANAGEMENT_ACTION_JOURNALED_ONLY = 1
} kb_store_management_action_dispatch_eligibility_t;

/* Caller-owned and retained across an ambiguous start. Every char array is a
 * canonical NUL-terminated fixed record with an all-zero unused tail. */
typedef struct
{
   char correlation_id[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1];
   char jti[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t team_id;
   char target_server_id[KB_STORE_MANAGEMENT_ACTION_SERVER_MAX + 1];
   kb_store_management_action_capability_t capability;
   char request_sha256[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1];
   char token_issuer[KB_STORE_MANAGEMENT_ACTION_TOKEN_ISSUER_MAX + 1];
   char kid[KB_STORE_MANAGEMENT_ACTION_KID_MAX + 1];
   int ttl_seconds;
   char installation_id[KB_STORE_MANAGEMENT_ACTION_INSTALL_ID_HEX + 1];
} kb_store_management_action_operation_t;

typedef struct
{
   int replayed;
   kb_store_management_action_dispatch_eligibility_t dispatch_eligibility;
   char correlation_id[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1];
   char jti[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t team_id;
   char actor_identity[KB_STORE_MANAGEMENT_ACTION_ACTOR_MAX + 1];
   kb_store_management_action_capability_t capability;
   char target_server_id[KB_STORE_MANAGEMENT_ACTION_SERVER_MAX + 1];
   char request_sha256[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1];
   char token_issuer[KB_STORE_MANAGEMENT_ACTION_TOKEN_ISSUER_MAX + 1];
   char audience[KB_STORE_MANAGEMENT_ACTION_SERVER_MAX + 1];
   char kid[KB_STORE_MANAGEMENT_ACTION_KID_MAX + 1];
   int64_t issued_at, expires_at;
   char installation_id[KB_STORE_MANAGEMENT_ACTION_INSTALL_ID_HEX + 1];
   int64_t installation_generation, installation_enrollment_id;
   char local_cert_issuer[KB_STORE_MANAGEMENT_ACTION_CERT_ISSUER_MAX + 1];
   char local_cert_serial_norm[KB_STORE_MANAGEMENT_ACTION_SERIAL_MAX + 1];
   char local_cert_fingerprint[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t target_enrollment_id;
   char target_mgmt_issuer[KB_STORE_MANAGEMENT_ACTION_CERT_ISSUER_MAX + 1];
   char target_mgmt_serial_norm[KB_STORE_MANAGEMENT_ACTION_SERIAL_MAX + 1];
   char target_mgmt_fingerprint[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t revocation_generation, created_at_epoch;
} kb_store_management_action_intent_t;

typedef struct
{
   char correlation_id[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t team_id;
   kb_store_management_action_outcome_result_t result;
   kb_store_management_action_outcome_class_t result_class;
   int has_status_code;
   int status_code;
   int has_response_sha256;
   char response_sha256[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1];
} kb_store_management_action_outcome_operation_t;

typedef struct
{
   int replayed;
   char correlation_id[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t team_id;
   kb_store_management_action_outcome_result_t result;
   kb_store_management_action_outcome_class_t result_class;
   int has_status_code;
   int status_code;
   int has_response_sha256;
   char response_sha256[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t completed_at_epoch;
} kb_store_management_action_outcome_t;

#ifdef __cplusplus
extern "C"
{
#endif

   kb_store_management_action_result_t
   kb_store_management_action_classify_sqlstate(const char *sqlstate);

   /* Generate correlation/JTI once and canonicalize a binary request digest.
    * `out` is cleared on entry and failure. */
   kb_store_management_action_result_t kb_store_management_action_operation_init(
       int64_t team_id, const char *target_server_id,
       kb_store_management_action_capability_t capability, const uint8_t request_sha256[32],
       const char *token_issuer, const char *kid, int ttl_seconds, const char *installation_id,
       kb_store_management_action_operation_t *out);

   /* Explicit canonical-hex alternative for callers that already own a digest. */
   kb_store_management_action_result_t kb_store_management_action_operation_init_hex(
       int64_t team_id, const char *target_server_id,
       kb_store_management_action_capability_t capability, const char *request_sha256,
       const char *token_issuer, const char *kid, int ttl_seconds, const char *installation_id,
       kb_store_management_action_operation_t *out);

   kb_store_management_action_result_t
   kb_store_management_action_intent_start(const kb_principal_t *principal,
                                           const kb_store_management_action_operation_t *operation,
                                           kb_store_management_action_intent_t *out);

   kb_store_management_action_result_t kb_store_management_action_outcome_append(
       const kb_principal_t *principal,
       const kb_store_management_action_outcome_operation_t *operation,
       kb_store_management_action_outcome_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_KB_STORE_MANAGEMENT_ACTION_JOURNAL_H */
