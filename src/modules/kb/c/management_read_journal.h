#ifndef AIMEE_KB_STORE_MANAGEMENT_READ_JOURNAL_H
#define AIMEE_KB_STORE_MANAGEMENT_READ_JOURNAL_H

#include "kb_identity.h"
#include "management_read.h"
#include <stdint.h>

typedef enum
{
   KB_STORE_MANAGEMENT_READ_OK = 0,
   KB_STORE_MANAGEMENT_READ_INVALID,
   KB_STORE_MANAGEMENT_READ_DENIED,
   KB_STORE_MANAGEMENT_READ_CONFLICT,
   KB_STORE_MANAGEMENT_READ_INTEGRITY,
   KB_STORE_MANAGEMENT_READ_UNAVAILABLE,
   KB_STORE_MANAGEMENT_READ_COMMIT_AMBIGUOUS
} kb_store_management_read_result_t;

typedef struct
{
   char correlation_id[65], jti[65];
   int64_t team_id;
   char actor_identity[577], target_server_id[128], request_sha256[65], kid[65];
   int64_t issued_at, expires_at, issuance_deadline_epoch;
   char local_cert_issuer[512], local_cert_serial_norm[80], local_cert_fingerprint[65];
   char target_mgmt_issuer[512], target_mgmt_serial_norm[80], target_mgmt_fingerprint[65];
   int64_t revocation_generation, publication_generation;
} kb_store_management_read_intent_t;

kb_store_management_read_result_t kb_store_management_read_publication_generation(int64_t *);
kb_store_management_read_result_t
kb_store_management_read_intent_start(const kb_principal_t *, int64_t, const char *,
                                      server_mgmt_read_selector_t, const char *, const uint8_t[32],
                                      const char *, const char *, const char *, int,
                                      kb_store_management_read_intent_t *);

#endif
