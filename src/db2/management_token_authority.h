#ifndef AIMEE_DB2_MANAGEMENT_TOKEN_AUTHORITY_H
#define AIMEE_DB2_MANAGEMENT_TOKEN_AUTHORITY_H

#include "../kb/kb_mgmt_token_authority.h"

#include <stddef.h>

typedef enum
{
   DB2_MANAGEMENT_TOKEN_AUTHORITY_OK = 0,
   DB2_MANAGEMENT_TOKEN_AUTHORITY_DENIED,
   DB2_MANAGEMENT_TOKEN_AUTHORITY_CONFLICT,
   DB2_MANAGEMENT_TOKEN_AUTHORITY_EXPIRED,
   DB2_MANAGEMENT_TOKEN_AUTHORITY_SEALED,
   DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY,
   DB2_MANAGEMENT_TOKEN_AUTHORITY_ABSENT,
   DB2_MANAGEMENT_TOKEN_AUTHORITY_UNAVAILABLE,
   DB2_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS
} db2_management_token_authority_result_t;

typedef struct
{
   void *connection;
   int use_transaction_open;
   char correlation_id[65];
   char jti[65];
   kb_mgmt_token_authority_record_t use_record;
} db2_management_token_authority_ctx_t;

#ifdef __cplusplus
extern "C"
{
#endif

   int db2_management_token_authority_open(db2_management_token_authority_ctx_t *ctx,
                                           const char *conninfo, char *errbuf, size_t errlen);
   void db2_management_token_authority_close(db2_management_token_authority_ctx_t *ctx);

   /* Admission commits before returning any envelope. A replay is returned as
    * OK with newly_admitted=0 and must not proceed to private-key use. */
   db2_management_token_authority_result_t
   db2_management_token_authority_admit(db2_management_token_authority_ctx_t *ctx,
                                        const char correlation_id[65], const char jti[65],
                                        kb_mgmt_token_authority_record_t *out);

   /* Resolve a lost admission COMMIT acknowledgement without private use. */
   db2_management_token_authority_result_t
   db2_management_token_authority_readback(db2_management_token_authority_ctx_t *ctx,
                                           const char correlation_id[65], const char jti[65],
                                           kb_mgmt_token_authority_record_t *out);

   /* Begin the fresh REPEATABLE READ use transaction. On OK the transaction
    * and facade-acquired row locks remain held until finalize or abort. */
   db2_management_token_authority_result_t
   db2_management_token_authority_use_begin(db2_management_token_authority_ctx_t *ctx,
                                            const char correlation_id[65], const char jti[65],
                                            kb_mgmt_token_authority_record_t *out);

   /* Recheck the locked tuple at a fresh database time and commit. JWT bytes
    * must not be released until this returns OK. */
   db2_management_token_authority_result_t
   db2_management_token_authority_finalize(db2_management_token_authority_ctx_t *ctx);
   void db2_management_token_authority_abort(db2_management_token_authority_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
