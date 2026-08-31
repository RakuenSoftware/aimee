#ifndef AIMEE_DB2_MANAGEMENT_READ_JOURNAL_H
#define AIMEE_DB2_MANAGEMENT_READ_JOURNAL_H

#include "kb_identity.h"
#include "management_read.h"
#include <stdint.h>

typedef enum
{
   DB2_MANAGEMENT_READ_OK = 0,
   DB2_MANAGEMENT_READ_INVALID,
   DB2_MANAGEMENT_READ_DENIED,
   DB2_MANAGEMENT_READ_CONFLICT,
   DB2_MANAGEMENT_READ_INTEGRITY,
   DB2_MANAGEMENT_READ_UNAVAILABLE,
   DB2_MANAGEMENT_READ_COMMIT_AMBIGUOUS
} db2_management_read_result_t;

typedef struct
{
   char correlation_id[65], jti[65];
   int64_t team_id;
   char actor_identity[577], target_server_id[128], request_sha256[65], kid[65];
   int64_t issued_at, expires_at, issuance_deadline_epoch;
   char local_cert_issuer[512], local_cert_serial_norm[80], local_cert_fingerprint[65];
   char target_mgmt_issuer[512], target_mgmt_serial_norm[80], target_mgmt_fingerprint[65];
   int64_t revocation_generation, publication_generation;
} db2_management_read_intent_t;

db2_management_read_result_t db2_management_read_publication_generation(int64_t *);
db2_management_read_result_t
db2_management_read_intent_start(const kb_principal_t *, int64_t, const char *,
                                 server_mgmt_read_selector_t, const char *, const uint8_t[32],
                                 const char *, const char *, const char *, int,
                                 db2_management_read_intent_t *);

#endif
