/* P5-C1c: typed boundary for the primary/WORM management-action journal. */
#ifndef AIMEE_DB2_MANAGEMENT_ACTION_JOURNAL_H
#define AIMEE_DB2_MANAGEMENT_ACTION_JOURNAL_H

#include "kb_identity.h"

#include <stddef.h>
#include <stdint.h>

#define DB2_MANAGEMENT_ACTION_ID_HEX           64U
#define DB2_MANAGEMENT_ACTION_SERVER_MAX       127U
#define DB2_MANAGEMENT_ACTION_TOKEN_ISSUER_MAX 255U
#define DB2_MANAGEMENT_ACTION_CERT_ISSUER_MAX  511U
#define DB2_MANAGEMENT_ACTION_ACTOR_MAX        576U
#define DB2_MANAGEMENT_ACTION_KID_MAX          64U
#define DB2_MANAGEMENT_ACTION_INSTALL_ID_HEX   32U
#define DB2_MANAGEMENT_ACTION_SERIAL_MAX       79U

typedef enum
{
   DB2_MANAGEMENT_ACTION_OK = 0,
   DB2_MANAGEMENT_ACTION_INVALID,
   DB2_MANAGEMENT_ACTION_DENIED,
   DB2_MANAGEMENT_ACTION_CONFLICT,
   DB2_MANAGEMENT_ACTION_RETRY,
   DB2_MANAGEMENT_ACTION_INTEGRITY,
   DB2_MANAGEMENT_ACTION_UNAVAILABLE,
   /* The COMMIT acknowledgement was lost. Outputs are clear; retry the exact
    * caller-owned operation object, whose identifiers must not be regenerated. */
   DB2_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS
} db2_management_action_result_t;

typedef enum
{
   DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES = 1
} db2_management_action_capability_t;

typedef enum
{
   DB2_MANAGEMENT_ACTION_SUCCEEDED = 1,
   DB2_MANAGEMENT_ACTION_DENIED_RESULT,
   DB2_MANAGEMENT_ACTION_FAILED,
   DB2_MANAGEMENT_ACTION_INDETERMINATE
} db2_management_action_outcome_result_t;

typedef enum
{
   DB2_MANAGEMENT_ACTION_CLASS_REMOTE_SUCCESS = 1,
   DB2_MANAGEMENT_ACTION_CLASS_REMOTE_DENIED,
   DB2_MANAGEMENT_ACTION_CLASS_REMOTE_FAILURE,
   DB2_MANAGEMENT_ACTION_CLASS_TRANSPORT_AMBIGUOUS,
   DB2_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE,
   DB2_MANAGEMENT_ACTION_CLASS_LOCAL_FAILURE
} db2_management_action_outcome_class_t;

typedef enum
{
   /* C1c is only a durable authorization record. C2/C3 must independently bind
    * the kid to custody and recheck every snapshot before mint or dispatch. */
   DB2_MANAGEMENT_ACTION_JOURNALED_ONLY = 1
} db2_management_action_dispatch_eligibility_t;

/* Caller-owned and retained across an ambiguous start. Every char array is a
 * canonical NUL-terminated fixed record with an all-zero unused tail. */
typedef struct
{
   char correlation_id[DB2_MANAGEMENT_ACTION_ID_HEX + 1];
   char jti[DB2_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t team_id;
   char target_server_id[DB2_MANAGEMENT_ACTION_SERVER_MAX + 1];
   db2_management_action_capability_t capability;
   char request_sha256[DB2_MANAGEMENT_ACTION_ID_HEX + 1];
   char token_issuer[DB2_MANAGEMENT_ACTION_TOKEN_ISSUER_MAX + 1];
   char kid[DB2_MANAGEMENT_ACTION_KID_MAX + 1];
   int ttl_seconds;
   char installation_id[DB2_MANAGEMENT_ACTION_INSTALL_ID_HEX + 1];
} db2_management_action_operation_t;

typedef struct
{
   int replayed;
   db2_management_action_dispatch_eligibility_t dispatch_eligibility;
   char correlation_id[DB2_MANAGEMENT_ACTION_ID_HEX + 1];
   char jti[DB2_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t team_id;
   char actor_identity[DB2_MANAGEMENT_ACTION_ACTOR_MAX + 1];
   db2_management_action_capability_t capability;
   char target_server_id[DB2_MANAGEMENT_ACTION_SERVER_MAX + 1];
   char request_sha256[DB2_MANAGEMENT_ACTION_ID_HEX + 1];
   char token_issuer[DB2_MANAGEMENT_ACTION_TOKEN_ISSUER_MAX + 1];
   char audience[DB2_MANAGEMENT_ACTION_SERVER_MAX + 1];
   char kid[DB2_MANAGEMENT_ACTION_KID_MAX + 1];
   int64_t issued_at, expires_at;
   char installation_id[DB2_MANAGEMENT_ACTION_INSTALL_ID_HEX + 1];
   int64_t installation_generation, installation_enrollment_id;
   char local_cert_issuer[DB2_MANAGEMENT_ACTION_CERT_ISSUER_MAX + 1];
   char local_cert_serial_norm[DB2_MANAGEMENT_ACTION_SERIAL_MAX + 1];
   char local_cert_fingerprint[DB2_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t target_enrollment_id;
   char target_mgmt_issuer[DB2_MANAGEMENT_ACTION_CERT_ISSUER_MAX + 1];
   char target_mgmt_serial_norm[DB2_MANAGEMENT_ACTION_SERIAL_MAX + 1];
   char target_mgmt_fingerprint[DB2_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t revocation_generation, created_at_epoch;
} db2_management_action_intent_t;

typedef struct
{
   char correlation_id[DB2_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t team_id;
   db2_management_action_outcome_result_t result;
   db2_management_action_outcome_class_t result_class;
   int has_status_code;
   int status_code;
   int has_response_sha256;
   char response_sha256[DB2_MANAGEMENT_ACTION_ID_HEX + 1];
} db2_management_action_outcome_operation_t;

typedef struct
{
   int replayed;
   char correlation_id[DB2_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t team_id;
   db2_management_action_outcome_result_t result;
   db2_management_action_outcome_class_t result_class;
   int has_status_code;
   int status_code;
   int has_response_sha256;
   char response_sha256[DB2_MANAGEMENT_ACTION_ID_HEX + 1];
   int64_t completed_at_epoch;
} db2_management_action_outcome_t;

#ifdef __cplusplus
extern "C"
{
#endif

   db2_management_action_result_t db2_management_action_classify_sqlstate(const char *sqlstate);

   /* Generate correlation/JTI once and canonicalize a binary request digest.
    * `out` is cleared on entry and failure. */
   db2_management_action_result_t db2_management_action_operation_init(
       int64_t team_id, const char *target_server_id, db2_management_action_capability_t capability,
       const uint8_t request_sha256[32], const char *token_issuer, const char *kid, int ttl_seconds,
       const char *installation_id, db2_management_action_operation_t *out);

   /* Explicit canonical-hex alternative for callers that already own a digest. */
   db2_management_action_result_t db2_management_action_operation_init_hex(
       int64_t team_id, const char *target_server_id, db2_management_action_capability_t capability,
       const char *request_sha256, const char *token_issuer, const char *kid, int ttl_seconds,
       const char *installation_id, db2_management_action_operation_t *out);

   db2_management_action_result_t
   db2_management_action_intent_start(const kb_principal_t *principal,
                                      const db2_management_action_operation_t *operation,
                                      db2_management_action_intent_t *out);

   db2_management_action_result_t
   db2_management_action_outcome_append(const kb_principal_t *principal,
                                        const db2_management_action_outcome_operation_t *operation,
                                        db2_management_action_outcome_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_MANAGEMENT_ACTION_JOURNAL_H */
