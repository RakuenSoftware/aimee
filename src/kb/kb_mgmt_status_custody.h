#ifndef AIMEE_KB_MGMT_STATUS_CUSTODY_H
#define AIMEE_KB_MGMT_STATUS_CUSTODY_H

#include "kb_mgmt_status.h"
#include "kb_mgmt_status_authority.h"
#include "management_status_key.h"

typedef struct
{
   const char *custody_key_id;
   db2_management_status_key_ctx_t *database;
} kb_mgmt_status_custody_t;

typedef enum
{
   KB_MGMT_STATUS_CUSTODY_OK = 0,
   KB_MGMT_STATUS_CUSTODY_CONFLICT = 1,
   KB_MGMT_STATUS_CUSTODY_UNAVAILABLE = -1,
   KB_MGMT_STATUS_CUSTODY_INTEGRITY = -2,
} kb_mgmt_status_custody_result_t;

/* Narrow authority callback: the sole plaintext operation is Ed25519 signing.
 * Calls are serialized process-wide. A database context must not be used by
 * any other caller while this function owns its admission/guard transaction. */
kb_mgmt_status_custody_result_t kb_mgmt_status_custody_sign(kb_mgmt_status_t *status, void *ctx);
kb_mgmt_status_custody_result_t
kb_mgmt_status_custody_sign_checkpoint(kb_mgmt_checkpoint_t *checkpoint,
                                       const kb_mgmt_checkpoint_request_t *request, void *ctx);

#endif
