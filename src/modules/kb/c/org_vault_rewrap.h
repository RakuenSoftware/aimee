#ifndef AIMEE_KB_STORE_ORG_VAULT_REWRAP_H
#define AIMEE_KB_STORE_ORG_VAULT_REWRAP_H

#include "vault_crypto.h"
#include "vault_reseal_receipt.h"

#include <stddef.h>
#include <stdint.h>

#define KB_STORE_VAULT_REWRAP_PAGE_MAX 128

typedef enum
{
   KB_STORE_VAULT_REWRAP_PREPARING,
   KB_STORE_VAULT_REWRAP_CUSTODY_PREPARED,
   KB_STORE_VAULT_REWRAP_WRAPS_STAGED,
   KB_STORE_VAULT_REWRAP_RESEAL_COMMITTING,
   KB_STORE_VAULT_REWRAP_RESEALED,
   KB_STORE_VAULT_REWRAP_PROMOTED,
   KB_STORE_VAULT_REWRAP_COMPLETED,
   KB_STORE_VAULT_REWRAP_ABORTED,
   KB_STORE_VAULT_REWRAP_RECOVERY_REQUIRED
} kb_store_vault_rewrap_state_t;

typedef enum
{
   KB_STORE_VAULT_REWRAP_OK = 0,
   KB_STORE_VAULT_REWRAP_NOT_FOUND,
   KB_STORE_VAULT_REWRAP_BUSY,
   KB_STORE_VAULT_REWRAP_CONFLICT,
   KB_STORE_VAULT_REWRAP_INVALID,
   KB_STORE_VAULT_REWRAP_TRANSIENT,
   KB_STORE_VAULT_REWRAP_INTEGRITY,
   KB_STORE_VAULT_REWRAP_ERROR
} kb_store_vault_rewrap_result_t;

typedef struct
{
   uint8_t operation_id[16];
   kb_store_vault_rewrap_state_t state;
   int64_t seal_epoch, fencing_token, old_generation, new_generation;
   int has_receipt, has_inventory, has_stage;
   uint8_t receipt[VAULT_RESEAL_RECEIPT_V1_LEN], receipt_digest[32];
   int64_t secret_count, check_count;
   uint8_t inventory_digest[32], stage_digest[32];
   char failure_class[65];
   int has_failure_from_state;
   kb_store_vault_rewrap_state_t failure_from_state;
} kb_store_vault_rewrap_snapshot_t;

typedef struct
{
   int64_t source_id, version;
   char principal[641], agent[257], cred[257];
   uint8_t source_digest[32], wrapped_dek[VAULT_WRAPPED_DEK_LEN];
} kb_store_vault_rewrap_secret_t;

typedef struct
{
   char principal[641];
   uint8_t source_digest[32], kek_check[VAULT_WRAPPED_DEK_LEN];
   size_t kek_check_len;
} kb_store_vault_rewrap_check_t;

typedef struct kb_store_vault_rewrap_tx kb_store_vault_rewrap_tx_t;

typedef struct
{
   uint8_t bytes[640];
   size_t len;
} kb_store_vault_rewrap_cursor_t;

typedef struct
{
   int64_t secret_count, check_count;
   uint8_t receipt_digest[32], inventory_digest[32], stage_digest[32];
} kb_store_vault_rewrap_verify_summary_t;

typedef struct
{
   int64_t secret_count, check_count;
   uint8_t inventory_digest[32];
} kb_store_vault_rewrap_inventory_summary_t;

typedef struct
{
   int64_t (*deadline_ms)(uint32_t per_call_ms);
   int (*operation_id_to_hex)(const uint8_t operation_id[VAULT_RESEAL_OPERATION_ID_LEN],
                              char out[VAULT_RESEAL_OPERATION_HEX_LEN + 1]);
   int (*operation_id_from_hex)(const char *hex,
                                uint8_t operation_id[VAULT_RESEAL_OPERATION_ID_LEN]);
   int (*receipt_decode)(const uint8_t *wire, size_t wire_len,
                         vault_tpm2_reseal_receipt_t *receipt);
   int (*receipt_digest)(const uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN], uint8_t digest[32]);
} kb_store_vault_reseal_provider_t;

void aimee_kb_store_register_vault_reseal_provider(
    const kb_store_vault_reseal_provider_t *provider);
int64_t kb_store_vault_reseal_deadline_ms(uint32_t per_call_ms);
int kb_store_vault_reseal_operation_id_to_hex(const uint8_t operation_id[16], char out[33]);
int kb_store_vault_reseal_operation_id_from_hex(const char *hex, uint8_t operation_id[16]);
int kb_store_vault_reseal_receipt_decode(const uint8_t *wire, size_t wire_len,
                                         vault_tpm2_reseal_receipt_t *receipt);
int kb_store_vault_reseal_receipt_digest(const uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN],
                                         uint8_t digest[32]);

void kb_store_vault_rewrap_snapshot_clear(kb_store_vault_rewrap_snapshot_t *snapshot);
void kb_store_vault_rewrap_secret_clear(kb_store_vault_rewrap_secret_t *rows, size_t count);
void kb_store_vault_rewrap_check_clear(kb_store_vault_rewrap_check_t *rows, size_t count);
void kb_store_vault_rewrap_cursor_clear(kb_store_vault_rewrap_cursor_t *cursor);
void kb_store_vault_rewrap_verify_summary_clear(kb_store_vault_rewrap_verify_summary_t *summary);
kb_store_vault_rewrap_result_t
kb_store_vault_rewrap_snapshot(const uint8_t operation_id[16],
                               kb_store_vault_rewrap_snapshot_t *out);

kb_store_vault_rewrap_result_t kb_store_vault_rewrap_tx_begin(kb_store_vault_rewrap_tx_t **out);
kb_store_vault_rewrap_result_t kb_store_vault_rewrap_tx_commit(kb_store_vault_rewrap_tx_t **tx);
void kb_store_vault_rewrap_tx_rollback(kb_store_vault_rewrap_tx_t **tx);

kb_store_vault_rewrap_result_t
kb_store_vault_rewrap_begin(kb_store_vault_rewrap_tx_t *tx, const char *actor,
                            const char *request_id, const uint8_t operation_id[16],
                            int64_t old_generation, int64_t new_generation, int64_t *seal_epoch,
                            int64_t *fence, kb_store_vault_rewrap_state_t *state);
kb_store_vault_rewrap_result_t
kb_store_vault_rewrap_record_prepared(kb_store_vault_rewrap_tx_t *tx,
                                      const uint8_t operation_id[16], int64_t fence,
                                      int64_t old_generation, int64_t new_generation,
                                      const uint8_t receipt[VAULT_RESEAL_RECEIPT_V1_LEN]);
kb_store_vault_rewrap_result_t kb_store_vault_rewrap_source_secret_page(
    kb_store_vault_rewrap_tx_t *tx, const uint8_t operation_id[16], int64_t fence, int64_t after,
    int limit, kb_store_vault_rewrap_secret_t *rows, size_t capacity, size_t *count);
kb_store_vault_rewrap_result_t kb_store_vault_rewrap_source_check_page(
    kb_store_vault_rewrap_tx_t *tx, const uint8_t operation_id[16], int64_t fence,
    const kb_store_vault_rewrap_cursor_t *after, int limit, kb_store_vault_rewrap_check_t *rows,
    size_t capacity, size_t *count, kb_store_vault_rewrap_cursor_t *next);
kb_store_vault_rewrap_result_t
kb_store_vault_rewrap_stage_dek(kb_store_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                                int64_t fence, const kb_store_vault_rewrap_secret_t *source,
                                const uint8_t new_wrapped_dek[VAULT_WRAPPED_DEK_LEN]);
kb_store_vault_rewrap_result_t
kb_store_vault_rewrap_stage_check(kb_store_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                                  int64_t fence, const kb_store_vault_rewrap_check_t *source,
                                  const uint8_t *new_check, size_t new_check_len);
kb_store_vault_rewrap_result_t
kb_store_vault_rewrap_inventory_summary(kb_store_vault_rewrap_tx_t *tx,
                                        const uint8_t operation_id[16], int64_t fence,
                                        kb_store_vault_rewrap_inventory_summary_t *out);
kb_store_vault_rewrap_result_t
kb_store_vault_rewrap_stage_finish(kb_store_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                                   int64_t fence,
                                   const kb_store_vault_rewrap_inventory_summary_t *expected);
kb_store_vault_rewrap_result_t kb_store_vault_rewrap_mark_committing(kb_store_vault_rewrap_tx_t *tx,
                                                                     const uint8_t operation_id[16],
                                                                     int64_t fence);
kb_store_vault_rewrap_result_t
kb_store_vault_rewrap_mark_resealed(kb_store_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                                    int64_t fence, const uint8_t receipt_digest[32]);
kb_store_vault_rewrap_result_t kb_store_vault_rewrap_promote(kb_store_vault_rewrap_tx_t *tx,
                                                             const uint8_t operation_id[16],
                                                             int64_t fence);
kb_store_vault_rewrap_result_t
kb_store_vault_rewrap_complete(kb_store_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                               int64_t fence, const uint8_t receipt_digest[32],
                               const uint8_t inventory_digest[32], const uint8_t stage_digest[32]);
kb_store_vault_rewrap_result_t kb_store_vault_rewrap_abort(kb_store_vault_rewrap_tx_t *tx,
                                                           const uint8_t operation_id[16],
                                                           int64_t fence,
                                                           const char *failure_class);
kb_store_vault_rewrap_result_t
kb_store_vault_rewrap_recovery_required(kb_store_vault_rewrap_tx_t *tx,
                                        const uint8_t operation_id[16], int64_t fence,
                                        const char *failure_class);

kb_store_vault_rewrap_result_t
kb_store_vault_rewrap_verify_summary(kb_store_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                                     int64_t fence, kb_store_vault_rewrap_verify_summary_t *out);
kb_store_vault_rewrap_result_t kb_store_vault_rewrap_verify_secret_page(
    kb_store_vault_rewrap_tx_t *tx, const uint8_t operation_id[16], int64_t fence, int64_t after,
    int limit, kb_store_vault_rewrap_secret_t *rows, size_t capacity, size_t *count);
kb_store_vault_rewrap_result_t kb_store_vault_rewrap_verify_check_page(
    kb_store_vault_rewrap_tx_t *tx, const uint8_t operation_id[16], int64_t fence,
    const kb_store_vault_rewrap_cursor_t *after, int limit, kb_store_vault_rewrap_check_t *rows,
    size_t capacity, size_t *count, kb_store_vault_rewrap_cursor_t *next);
kb_store_vault_rewrap_result_t
kb_store_vault_rewrap_verify_crypto_ack(kb_store_vault_rewrap_tx_t *tx,
                                        const uint8_t operation_id[16], int64_t fence);

/* Frozen injection seam for D2b's exhaustive fake-DB state-machine tests. */
typedef struct
{
   kb_store_vault_rewrap_result_t (*tx_begin)(kb_store_vault_rewrap_tx_t **);
   kb_store_vault_rewrap_result_t (*tx_commit)(kb_store_vault_rewrap_tx_t **);
   void (*tx_rollback)(kb_store_vault_rewrap_tx_t **);
   kb_store_vault_rewrap_result_t (*snapshot)(const uint8_t[16],
                                              kb_store_vault_rewrap_snapshot_t *);
   kb_store_vault_rewrap_result_t (*begin)(kb_store_vault_rewrap_tx_t *, const char *, const char *,
                                           const uint8_t[16], int64_t, int64_t, int64_t *,
                                           int64_t *, kb_store_vault_rewrap_state_t *);
   kb_store_vault_rewrap_result_t (*record_prepared)(kb_store_vault_rewrap_tx_t *,
                                                     const uint8_t[16], int64_t, int64_t, int64_t,
                                                     const uint8_t[VAULT_RESEAL_RECEIPT_V1_LEN]);
   kb_store_vault_rewrap_result_t (*source_secret_page)(kb_store_vault_rewrap_tx_t *,
                                                        const uint8_t[16], int64_t, int64_t, int,
                                                        kb_store_vault_rewrap_secret_t *, size_t,
                                                        size_t *);
   kb_store_vault_rewrap_result_t (*source_check_page)(kb_store_vault_rewrap_tx_t *,
                                                       const uint8_t[16], int64_t,
                                                       const kb_store_vault_rewrap_cursor_t *, int,
                                                       kb_store_vault_rewrap_check_t *, size_t,
                                                       size_t *, kb_store_vault_rewrap_cursor_t *);
   kb_store_vault_rewrap_result_t (*stage_dek)(kb_store_vault_rewrap_tx_t *, const uint8_t[16],
                                               int64_t, const kb_store_vault_rewrap_secret_t *,
                                               const uint8_t[VAULT_WRAPPED_DEK_LEN]);
   kb_store_vault_rewrap_result_t (*stage_check)(kb_store_vault_rewrap_tx_t *, const uint8_t[16],
                                                 int64_t, const kb_store_vault_rewrap_check_t *,
                                                 const uint8_t *, size_t);
   kb_store_vault_rewrap_result_t (*inventory_summary)(kb_store_vault_rewrap_tx_t *,
                                                       const uint8_t[16], int64_t,
                                                       kb_store_vault_rewrap_inventory_summary_t *);
   kb_store_vault_rewrap_result_t (*stage_finish)(
       kb_store_vault_rewrap_tx_t *, const uint8_t[16], int64_t,
       const kb_store_vault_rewrap_inventory_summary_t *);
   kb_store_vault_rewrap_result_t (*mark_committing)(kb_store_vault_rewrap_tx_t *,
                                                     const uint8_t[16], int64_t);
   kb_store_vault_rewrap_result_t (*mark_resealed)(kb_store_vault_rewrap_tx_t *, const uint8_t[16],
                                                   int64_t, const uint8_t[32]);
   kb_store_vault_rewrap_result_t (*promote)(kb_store_vault_rewrap_tx_t *, const uint8_t[16],
                                             int64_t);
   kb_store_vault_rewrap_result_t (*abort)(kb_store_vault_rewrap_tx_t *, const uint8_t[16], int64_t,
                                           const char *);
   kb_store_vault_rewrap_result_t (*recovery_required)(kb_store_vault_rewrap_tx_t *,
                                                       const uint8_t[16], int64_t, const char *);
   kb_store_vault_rewrap_result_t (*verify_summary)(kb_store_vault_rewrap_tx_t *, const uint8_t[16],
                                                    int64_t,
                                                    kb_store_vault_rewrap_verify_summary_t *);
   kb_store_vault_rewrap_result_t (*verify_secret_page)(kb_store_vault_rewrap_tx_t *,
                                                        const uint8_t[16], int64_t, int64_t, int,
                                                        kb_store_vault_rewrap_secret_t *, size_t,
                                                        size_t *);
   kb_store_vault_rewrap_result_t (*verify_check_page)(kb_store_vault_rewrap_tx_t *,
                                                       const uint8_t[16], int64_t,
                                                       const kb_store_vault_rewrap_cursor_t *, int,
                                                       kb_store_vault_rewrap_check_t *, size_t,
                                                       size_t *, kb_store_vault_rewrap_cursor_t *);
   kb_store_vault_rewrap_result_t (*verify_crypto_ack)(kb_store_vault_rewrap_tx_t *,
                                                       const uint8_t[16], int64_t);
   kb_store_vault_rewrap_result_t (*complete)(kb_store_vault_rewrap_tx_t *, const uint8_t[16],
                                              int64_t, const uint8_t[32], const uint8_t[32],
                                              const uint8_t[32]);
} kb_store_vault_rewrap_ops_t;

extern const kb_store_vault_rewrap_ops_t kb_store_vault_rewrap_default_ops;

/* Stable SQLSTATE-only mapping shared by every wrapper operation. */
kb_store_vault_rewrap_result_t kb_store_vault_rewrap_classify_sqlstate(const char *sqlstate);

#endif
