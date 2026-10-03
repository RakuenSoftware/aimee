#ifndef AIMEE_KB_STORE_MANAGEMENT_TOKEN_AUTHORITY_H
#define AIMEE_KB_STORE_MANAGEMENT_TOKEN_AUTHORITY_H

#include "kb_mgmt_token_authority.h"
#include "kb_mgmt_token_authority_ipc.h"

#include <stddef.h>

typedef enum
{
   KB_STORE_MANAGEMENT_TOKEN_AUTHORITY_OK = 0,
   KB_STORE_MANAGEMENT_TOKEN_AUTHORITY_DENIED,
   KB_STORE_MANAGEMENT_TOKEN_AUTHORITY_CONFLICT,
   KB_STORE_MANAGEMENT_TOKEN_AUTHORITY_EXPIRED,
   KB_STORE_MANAGEMENT_TOKEN_AUTHORITY_SEALED,
   KB_STORE_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY,
   KB_STORE_MANAGEMENT_TOKEN_AUTHORITY_ABSENT,
   KB_STORE_MANAGEMENT_TOKEN_AUTHORITY_UNAVAILABLE,
   KB_STORE_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS
} kb_store_management_token_authority_result_t;

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
} kb_store_management_token_authority_ctx_t;

typedef enum
{
   KB_STORE_MANAGEMENT_TOKEN_INTENT_ACTION = 1,
   KB_STORE_MANAGEMENT_TOKEN_INTENT_READ = 2,
   /* Data-plane identity token (per-user remote_writes §4). Resolved from the
    * same (correlation_id, jti) namespace as the other two, so the hardened IPC
    * seam carries no new request type. */
   KB_STORE_MANAGEMENT_TOKEN_INTENT_IDENTITY = 3
} kb_store_management_token_intent_kind_t;

typedef int (*kb_store_mgmt_token_record_valid_fn)(const kb_mgmt_token_authority_record_t *record);
typedef int (*kb_store_identity_token_record_valid_fn)(
    const kb_identity_token_authority_record_t *record);

#ifdef __cplusplus
extern "C"
{
#endif

   /* Internal declaration of the paired authority-record host contract exported
    * publicly through <aimee/kb/host_contracts.h>. */
   void aimee_kb_store_register_token_record_validators(
       kb_store_mgmt_token_record_valid_fn management,
       kb_store_identity_token_record_valid_fn identity);

   /* Fail closed unless the corresponding host validator is registered and
    * returns the contract's exact success value. Kept visible for focused
    * boundary tests; decoders use the same helpers. */
   int kb_store_management_token_authority_record_validate(
       const kb_mgmt_token_authority_record_t *record);
   int kb_store_management_identity_authority_record_validate(
       const kb_identity_token_authority_record_t *record);

   int kb_store_management_token_authority_open(kb_store_management_token_authority_ctx_t *ctx,
                                                const char *conninfo, char *errbuf, size_t errlen);
   void kb_store_management_token_authority_close(kb_store_management_token_authority_ctx_t *ctx);

   /* Admission commits before returning any envelope. A replay is returned as
    * OK with newly_admitted=0 and must not proceed to private-key use. */
   kb_store_management_token_authority_result_t
   kb_store_management_token_authority_admit(kb_store_management_token_authority_ctx_t *ctx,
                                             const char correlation_id[65], const char jti[65],
                                             kb_mgmt_token_authority_record_t *out);

   /* Identity-token admission (per-user remote_writes §4). Same contract as the
    * management admit: it commits before returning, a replay comes back OK with
    * newly_admitted=0 and must not proceed to private-key use, and a lost COMMIT
    * acknowledgement is terminal rather than retried. `jti` is the 64-hex
    * namespace handle; the record carries the token's own jti claim. */
   kb_store_management_token_authority_result_t
   kb_store_management_identity_authority_admit(kb_store_management_token_authority_ctx_t *ctx,
                                                const char correlation_id[65], const char jti[65],
                                                kb_identity_token_authority_record_t *out);

   /* Resolve a lost identity admission COMMIT without private-key use. Returns
    * ABSENT when nothing was admitted, which is a normal answer here. */
   kb_store_management_token_authority_result_t kb_store_management_identity_authority_readback(
       kb_store_management_token_authority_ctx_t *ctx, const char correlation_id[65],
       const char jti[65], kb_identity_token_authority_record_t *out);

   /* Open the REPEATABLE READ transaction held across private-key use. Closed by
    * kb_store_management_identity_authority_finalize or _abort. */
   kb_store_management_token_authority_result_t kb_store_management_identity_authority_use_begin(
       kb_store_management_token_authority_ctx_t *ctx, const char correlation_id[65],
       const char jti[65], kb_identity_token_authority_record_t *out);

   /* Re-verify and commit the identity use transaction. Refuses a transaction
    * opened for a different token kind: finalize keys only on correlation_id/jti
    * but runs a per-kind SQL function, so the kind guard is what stops a
    * mismatched call from running the wrong authority function against a live
    * signing transaction. */
   kb_store_management_token_authority_result_t
   kb_store_management_identity_authority_finalize(kb_store_management_token_authority_ctx_t *ctx);

   /* Resolve a lost admission COMMIT acknowledgement without private use. */
   kb_store_management_token_authority_result_t
   kb_store_management_token_authority_readback(kb_store_management_token_authority_ctx_t *ctx,
                                                const char correlation_id[65], const char jti[65],
                                                kb_mgmt_token_authority_record_t *out);

   /* Begin the fresh REPEATABLE READ use transaction. On OK the transaction
    * and facade-acquired row locks remain held until finalize or abort. */
   kb_store_management_token_authority_result_t
   kb_store_management_token_authority_use_begin(kb_store_management_token_authority_ctx_t *ctx,
                                                 const char correlation_id[65], const char jti[65],
                                                 kb_mgmt_token_authority_record_t *out);

   /* Recheck the locked tuple at a fresh database time and commit. JWT bytes
    * must not be released until this returns OK. */
   kb_store_management_token_authority_result_t
   kb_store_management_token_authority_finalize(kb_store_management_token_authority_ctx_t *ctx);
   void kb_store_management_token_authority_abort(kb_store_management_token_authority_ctx_t *ctx);

   kb_store_management_token_authority_result_t
   kb_store_management_token_authority_kind(kb_store_management_token_authority_ctx_t *ctx,
                                            const char correlation_id[65], const char jti[65],
                                            kb_store_management_token_intent_kind_t *kind);
   kb_store_management_token_authority_result_t kb_store_management_token_read_claim(
       kb_store_management_token_authority_ctx_t *ctx, const char correlation_id[65],
       const char jti[65], const char lease_owner[65], kb_mgmt_token_authority_record_t *out);
   kb_store_management_token_authority_result_t
   kb_store_management_token_read_finalize(kb_store_management_token_authority_ctx_t *ctx,
                                           const char correlation_id[65], const char jti[65],
                                           const char lease_owner[65], const char *jwt);
   kb_store_management_token_authority_result_t
   kb_store_management_token_read_readback(kb_store_management_token_authority_ctx_t *ctx,
                                           const char correlation_id[65], const char jti[65],
                                           kb_mgmt_token_authority_output_t *out);

#ifdef __cplusplus
}
#endif

#endif
