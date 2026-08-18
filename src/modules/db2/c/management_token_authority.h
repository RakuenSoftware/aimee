#ifndef AIMEE_DB2_MANAGEMENT_TOKEN_AUTHORITY_H
#define AIMEE_DB2_MANAGEMENT_TOKEN_AUTHORITY_H

#include "kb_mgmt_token_authority.h"
#include "kb_mgmt_token_authority_ipc.h"

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
   /* Which token kind opened the use transaction. finalize runs a DIFFERENT SQL
    * function per kind while keying only on correlation_id/jti, so without this
    * a mismatched finalize would run the wrong authority function against a
    * live signing transaction. Set by use_begin, required by finalize, cleared
    * by abort. */
   int use_kind;
} db2_management_token_authority_ctx_t;

typedef enum
{
   DB2_MANAGEMENT_TOKEN_INTENT_ACTION = 1,
   DB2_MANAGEMENT_TOKEN_INTENT_READ = 2,
   /* Data-plane identity token (per-user remote_writes §4). Resolved from the
    * same (correlation_id, jti) namespace as the other two, so the hardened IPC
    * seam carries no new request type. */
   DB2_MANAGEMENT_TOKEN_INTENT_IDENTITY = 3
} db2_management_token_intent_kind_t;

typedef int (*db2_mgmt_token_record_valid_fn)(const kb_mgmt_token_authority_record_t *record);
typedef int (*db2_identity_token_record_valid_fn)(
    const kb_identity_token_authority_record_t *record);

#ifdef __cplusplus
extern "C"
{
#endif

   /* Internal declaration of the paired authority-record host contract exported
    * publicly through <aimee/db2/host_contracts.h>. */
   void aimee_db2_register_token_record_validators(db2_mgmt_token_record_valid_fn management,
                                                   db2_identity_token_record_valid_fn identity);

   /* Fail closed unless the corresponding host validator is registered and
    * returns the contract's exact success value. Kept visible for focused
    * boundary tests; decoders use the same helpers. */
   int
   db2_management_token_authority_record_validate(const kb_mgmt_token_authority_record_t *record);
   int db2_management_identity_authority_record_validate(
       const kb_identity_token_authority_record_t *record);

   int db2_management_token_authority_open(db2_management_token_authority_ctx_t *ctx,
                                           const char *conninfo, char *errbuf, size_t errlen);
   void db2_management_token_authority_close(db2_management_token_authority_ctx_t *ctx);

   /* Admission commits before returning any envelope. A replay is returned as
    * OK with newly_admitted=0 and must not proceed to private-key use. */
   db2_management_token_authority_result_t
   db2_management_token_authority_admit(db2_management_token_authority_ctx_t *ctx,
                                        const char correlation_id[65], const char jti[65],
                                        kb_mgmt_token_authority_record_t *out);

   /* Identity-token admission (per-user remote_writes §4). Same contract as the
    * management admit: it commits before returning, a replay comes back OK with
    * newly_admitted=0 and must not proceed to private-key use, and a lost COMMIT
    * acknowledgement is terminal rather than retried. `jti` is the 64-hex
    * namespace handle; the record carries the token's own jti claim. */
   db2_management_token_authority_result_t
   db2_management_identity_authority_admit(db2_management_token_authority_ctx_t *ctx,
                                           const char correlation_id[65], const char jti[65],
                                           kb_identity_token_authority_record_t *out);

   /* Resolve a lost identity admission COMMIT without private-key use. Returns
    * ABSENT when nothing was admitted, which is a normal answer here. */
   db2_management_token_authority_result_t
   db2_management_identity_authority_readback(db2_management_token_authority_ctx_t *ctx,
                                              const char correlation_id[65], const char jti[65],
                                              kb_identity_token_authority_record_t *out);

   /* Open the REPEATABLE READ transaction held across private-key use. Closed by
    * db2_management_identity_authority_finalize or _abort. */
   db2_management_token_authority_result_t
   db2_management_identity_authority_use_begin(db2_management_token_authority_ctx_t *ctx,
                                               const char correlation_id[65], const char jti[65],
                                               kb_identity_token_authority_record_t *out);

   /* Re-verify and commit the identity use transaction. Refuses a transaction
    * opened for a different token kind: finalize keys only on correlation_id/jti
    * but runs a per-kind SQL function, so the kind guard is what stops a
    * mismatched call from running the wrong authority function against a live
    * signing transaction. */
   db2_management_token_authority_result_t
   db2_management_identity_authority_finalize(db2_management_token_authority_ctx_t *ctx);

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

   db2_management_token_authority_result_t
   db2_management_token_authority_kind(db2_management_token_authority_ctx_t *ctx,
                                       const char correlation_id[65], const char jti[65],
                                       db2_management_token_intent_kind_t *kind);
   db2_management_token_authority_result_t db2_management_token_read_claim(
       db2_management_token_authority_ctx_t *ctx, const char correlation_id[65], const char jti[65],
       const char lease_owner[65], kb_mgmt_token_authority_record_t *out);
   db2_management_token_authority_result_t
   db2_management_token_read_finalize(db2_management_token_authority_ctx_t *ctx,
                                      const char correlation_id[65], const char jti[65],
                                      const char lease_owner[65], const char *jwt);
   db2_management_token_authority_result_t
   db2_management_token_read_readback(db2_management_token_authority_ctx_t *ctx,
                                      const char correlation_id[65], const char jti[65],
                                      kb_mgmt_token_authority_output_t *out);

#ifdef __cplusplus
}
#endif

#endif
