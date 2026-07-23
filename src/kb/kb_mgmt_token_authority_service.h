#ifndef AIMEE_KB_MGMT_TOKEN_AUTHORITY_SERVICE_H
#define AIMEE_KB_MGMT_TOKEN_AUTHORITY_SERVICE_H

#include "kb_mgmt_token_authority_ipc.h"
#include "management_token_authority.h"

typedef int (*kb_mgmt_token_authority_db_reopen_fn)(void *opaque,
                                                    db2_management_token_authority_ctx_t *db);

typedef struct
{
   db2_management_token_authority_ctx_t *db;
   kb_mgmt_token_authority_db_reopen_fn reopen_db;
   void *reopen_opaque;
} kb_mgmt_token_authority_service_t;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Exact callback shape consumed by the authority IPC daemon. The service
    * derives every signed field from the primary facade and cannot accept an
    * arbitrary claim, digest, private key, or signing input. */
   kb_mgmt_token_authority_ipc_result_t
   kb_mgmt_token_authority_service_issue(const char *correlation_id, const char *jti,
                                         kb_mgmt_token_authority_output_t *out, void *opaque);

#ifdef __cplusplus
}
#endif

#endif
