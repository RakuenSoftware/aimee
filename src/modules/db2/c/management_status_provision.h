#ifndef AIMEE_DB2_MANAGEMENT_STATUS_PROVISION_H
#define AIMEE_DB2_MANAGEMENT_STATUS_PROVISION_H

#include "org_vault_key_use.h"

#include <stddef.h>
#include <stdint.h>

typedef struct
{
   void *connection;
} db2_management_status_provision_ctx_t;

typedef struct
{
   char bootstrap_id[65];
   char custody_key_id[601];
   char wire_key_id[65];
   uint8_t public_key[32];
   uint8_t public_key_digest[32];
   uint8_t v1_envelope_digest[32];
   uint8_t v2_envelope_digest[32];
   int64_t rotation_id;
   int64_t seal_epoch;
   int64_t from_version;
   int64_t to_version;
   char state[16];
   int enabled;
   db2_vault_key_use_envelope_t v1;
   db2_vault_key_use_envelope_t v2;
} db2_management_status_provision_record_t;

int db2_management_status_provision_open(db2_management_status_provision_ctx_t *,
                                         const char *conninfo, char *errbuf, size_t errlen);
void db2_management_status_provision_close(db2_management_status_provision_ctx_t *);
int db2_management_status_provision_bootstrap_id(const char *custody_key_id, char out[65]);
int db2_management_status_provision_inspect(db2_management_status_provision_ctx_t *,
                                            const char *custody_key_id,
                                            db2_management_status_provision_record_t *);

int db2_management_status_provision_stage(db2_management_status_provision_ctx_t *,
                                          const db2_management_status_provision_record_t *,
                                          int64_t *rotation_id, int64_t *seal_epoch);
int db2_management_status_provision_resume(db2_management_status_provision_ctx_t *,
                                           const char *bootstrap_id, const char *custody_key_id,
                                           db2_management_status_provision_record_t *);
int db2_management_status_provision_prepare_activation(db2_management_status_provision_ctx_t *,
                                                       const char *bootstrap_id,
                                                       int64_t *rotation_id,
                                                       int64_t *expected_version,
                                                       int64_t *next_version);
int db2_management_status_provision_finalize(db2_management_status_provision_ctx_t *,
                                             const char *bootstrap_id,
                                             const uint8_t hwm2_attestation[64],
                                             db2_management_status_provision_record_t *);

#endif
