#ifndef AIMEE_KB_STORE_MANAGEMENT_STATUS_PROVISION_H
#define AIMEE_KB_STORE_MANAGEMENT_STATUS_PROVISION_H

#include "org_vault_key_use.h"

#include <stddef.h>
#include <stdint.h>

typedef struct
{
   void *connection;
} kb_store_management_status_provision_ctx_t;

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
   kb_store_vault_key_use_envelope_t v1;
   kb_store_vault_key_use_envelope_t v2;
} kb_store_management_status_provision_record_t;

int kb_store_management_status_provision_open(kb_store_management_status_provision_ctx_t *,
                                              const char *conninfo, char *errbuf, size_t errlen);
void kb_store_management_status_provision_close(kb_store_management_status_provision_ctx_t *);
int kb_store_management_status_provision_bootstrap_id(const char *custody_key_id, char out[65]);
int kb_store_management_status_provision_inspect(kb_store_management_status_provision_ctx_t *,
                                                 const char *custody_key_id,
                                                 kb_store_management_status_provision_record_t *);

int kb_store_management_status_provision_stage(
    kb_store_management_status_provision_ctx_t *,
    const kb_store_management_status_provision_record_t *, int64_t *rotation_id,
    int64_t *seal_epoch);
int kb_store_management_status_provision_resume(kb_store_management_status_provision_ctx_t *,
                                                const char *bootstrap_id,
                                                const char *custody_key_id,
                                                kb_store_management_status_provision_record_t *);
int kb_store_management_status_provision_prepare_activation(
    kb_store_management_status_provision_ctx_t *, const char *bootstrap_id, int64_t *rotation_id,
    int64_t *expected_version, int64_t *next_version);
int kb_store_management_status_provision_finalize(kb_store_management_status_provision_ctx_t *,
                                                  const char *bootstrap_id,
                                                  const uint8_t hwm2_attestation[64],
                                                  kb_store_management_status_provision_record_t *);

#endif
