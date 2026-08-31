#ifndef AIMEE_DB2_ORG_VAULT_REWRAP_H
#define AIMEE_DB2_ORG_VAULT_REWRAP_H

#include "vault_crypto.h"
#include "vault_reseal_receipt.h"

#include <stddef.h>
#include <stdint.h>

#define DB2_VAULT_REWRAP_PAGE_MAX 128

typedef enum
{
   DB2_VAULT_REWRAP_PREPARING,
   DB2_VAULT_REWRAP_CUSTODY_PREPARED,
   DB2_VAULT_REWRAP_WRAPS_STAGED,
   DB2_VAULT_REWRAP_RESEAL_COMMITTING,
   DB2_VAULT_REWRAP_RESEALED,
   DB2_VAULT_REWRAP_PROMOTED,
   DB2_VAULT_REWRAP_COMPLETED,
   DB2_VAULT_REWRAP_ABORTED,
   DB2_VAULT_REWRAP_RECOVERY_REQUIRED
} db2_vault_rewrap_state_t;

typedef enum
{
   DB2_VAULT_REWRAP_OK = 0,
   DB2_VAULT_REWRAP_NOT_FOUND,
   DB2_VAULT_REWRAP_BUSY,
   DB2_VAULT_REWRAP_CONFLICT,
   DB2_VAULT_REWRAP_INVALID,
   DB2_VAULT_REWRAP_TRANSIENT,
   DB2_VAULT_REWRAP_INTEGRITY,
   DB2_VAULT_REWRAP_ERROR
} db2_vault_rewrap_result_t;

typedef struct
{
   uint8_t operation_id[16];
   db2_vault_rewrap_state_t state;
   int64_t seal_epoch, fencing_token, old_generation, new_generation;
   int has_receipt, has_inventory, has_stage;
   uint8_t receipt[VAULT_RESEAL_RECEIPT_V1_LEN], receipt_digest[32];
   int64_t secret_count, check_count;
   uint8_t inventory_digest[32], stage_digest[32];
   char failure_class[65];
   int has_failure_from_state;
   db2_vault_rewrap_state_t failure_from_state;
} db2_vault_rewrap_snapshot_t;

typedef struct
{
   int64_t source_id, version;
   char principal[641], agent[257], cred[257];
   uint8_t source_digest[32], wrapped_dek[VAULT_WRAPPED_DEK_LEN];
} db2_vault_rewrap_secret_t;

typedef struct
{
   char principal[641];
   uint8_t source_digest[32], kek_check[VAULT_WRAPPED_DEK_LEN];
   size_t kek_check_len;
} db2_vault_rewrap_check_t;

typedef struct db2_vault_rewrap_tx db2_vault_rewrap_tx_t;

typedef struct
{
   uint8_t bytes[640];
   size_t len;
} db2_vault_rewrap_cursor_t;

typedef struct
{
   int64_t secret_count, check_count;
   uint8_t receipt_digest[32], inventory_digest[32], stage_digest[32];
} db2_vault_rewrap_verify_summary_t;

typedef struct
{
   int64_t secret_count, check_count;
   uint8_t inventory_digest[32];
} db2_vault_rewrap_inventory_summary_t;

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
} db2_vault_reseal_provider_t;

void aimee_db2_register_vault_reseal_provider(const db2_vault_reseal_provider_t *provider);
int64_t db2_vault_reseal_deadline_ms(uint32_t per_call_ms);
int db2_vault_reseal_operation_id_to_hex(const uint8_t operation_id[16], char out[33]);
int db2_vault_reseal_operation_id_from_hex(const char *hex, uint8_t operation_id[16]);
int db2_vault_reseal_receipt_decode(const uint8_t *wire, size_t wire_len,
                                    vault_tpm2_reseal_receipt_t *receipt);
int db2_vault_reseal_receipt_digest(const uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN],
                                    uint8_t digest[32]);

void db2_vault_rewrap_snapshot_clear(db2_vault_rewrap_snapshot_t *snapshot);
void db2_vault_rewrap_secret_clear(db2_vault_rewrap_secret_t *rows, size_t count);
void db2_vault_rewrap_check_clear(db2_vault_rewrap_check_t *rows, size_t count);
void db2_vault_rewrap_cursor_clear(db2_vault_rewrap_cursor_t *cursor);
void db2_vault_rewrap_verify_summary_clear(db2_vault_rewrap_verify_summary_t *summary);
db2_vault_rewrap_result_t db2_vault_rewrap_snapshot(const uint8_t operation_id[16],
                                                    db2_vault_rewrap_snapshot_t *out);

db2_vault_rewrap_result_t db2_vault_rewrap_tx_begin(db2_vault_rewrap_tx_t **out);
db2_vault_rewrap_result_t db2_vault_rewrap_tx_commit(db2_vault_rewrap_tx_t **tx);
void db2_vault_rewrap_tx_rollback(db2_vault_rewrap_tx_t **tx);

db2_vault_rewrap_result_t db2_vault_rewrap_begin(db2_vault_rewrap_tx_t *tx, const char *actor,
                                                 const char *request_id,
                                                 const uint8_t operation_id[16],
                                                 int64_t old_generation, int64_t new_generation,
                                                 int64_t *seal_epoch, int64_t *fence,
                                                 db2_vault_rewrap_state_t *state);
db2_vault_rewrap_result_t
db2_vault_rewrap_record_prepared(db2_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                                 int64_t fence, int64_t old_generation, int64_t new_generation,
                                 const uint8_t receipt[VAULT_RESEAL_RECEIPT_V1_LEN]);
db2_vault_rewrap_result_t db2_vault_rewrap_source_secret_page(
    db2_vault_rewrap_tx_t *tx, const uint8_t operation_id[16], int64_t fence, int64_t after,
    int limit, db2_vault_rewrap_secret_t *rows, size_t capacity, size_t *count);
db2_vault_rewrap_result_t
db2_vault_rewrap_source_check_page(db2_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                                   int64_t fence, const db2_vault_rewrap_cursor_t *after, int limit,
                                   db2_vault_rewrap_check_t *rows, size_t capacity, size_t *count,
                                   db2_vault_rewrap_cursor_t *next);
db2_vault_rewrap_result_t
db2_vault_rewrap_stage_dek(db2_vault_rewrap_tx_t *tx, const uint8_t operation_id[16], int64_t fence,
                           const db2_vault_rewrap_secret_t *source,
                           const uint8_t new_wrapped_dek[VAULT_WRAPPED_DEK_LEN]);
db2_vault_rewrap_result_t
db2_vault_rewrap_stage_check(db2_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                             int64_t fence, const db2_vault_rewrap_check_t *source,
                             const uint8_t *new_check, size_t new_check_len);
db2_vault_rewrap_result_t
db2_vault_rewrap_inventory_summary(db2_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                                   int64_t fence, db2_vault_rewrap_inventory_summary_t *out);
db2_vault_rewrap_result_t
db2_vault_rewrap_stage_finish(db2_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                              int64_t fence, const db2_vault_rewrap_inventory_summary_t *expected);
db2_vault_rewrap_result_t db2_vault_rewrap_mark_committing(db2_vault_rewrap_tx_t *tx,
                                                           const uint8_t operation_id[16],
                                                           int64_t fence);
db2_vault_rewrap_result_t db2_vault_rewrap_mark_resealed(db2_vault_rewrap_tx_t *tx,
                                                         const uint8_t operation_id[16],
                                                         int64_t fence,
                                                         const uint8_t receipt_digest[32]);
db2_vault_rewrap_result_t db2_vault_rewrap_promote(db2_vault_rewrap_tx_t *tx,
                                                   const uint8_t operation_id[16], int64_t fence);
db2_vault_rewrap_result_t db2_vault_rewrap_complete(db2_vault_rewrap_tx_t *tx,
                                                    const uint8_t operation_id[16], int64_t fence,
                                                    const uint8_t receipt_digest[32],
                                                    const uint8_t inventory_digest[32],
                                                    const uint8_t stage_digest[32]);
db2_vault_rewrap_result_t db2_vault_rewrap_abort(db2_vault_rewrap_tx_t *tx,
                                                 const uint8_t operation_id[16], int64_t fence,
                                                 const char *failure_class);
db2_vault_rewrap_result_t db2_vault_rewrap_recovery_required(db2_vault_rewrap_tx_t *tx,
                                                             const uint8_t operation_id[16],
                                                             int64_t fence,
                                                             const char *failure_class);

db2_vault_rewrap_result_t db2_vault_rewrap_verify_summary(db2_vault_rewrap_tx_t *tx,
                                                          const uint8_t operation_id[16],
                                                          int64_t fence,
                                                          db2_vault_rewrap_verify_summary_t *out);
db2_vault_rewrap_result_t db2_vault_rewrap_verify_secret_page(
    db2_vault_rewrap_tx_t *tx, const uint8_t operation_id[16], int64_t fence, int64_t after,
    int limit, db2_vault_rewrap_secret_t *rows, size_t capacity, size_t *count);
db2_vault_rewrap_result_t
db2_vault_rewrap_verify_check_page(db2_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                                   int64_t fence, const db2_vault_rewrap_cursor_t *after, int limit,
                                   db2_vault_rewrap_check_t *rows, size_t capacity, size_t *count,
                                   db2_vault_rewrap_cursor_t *next);
db2_vault_rewrap_result_t db2_vault_rewrap_verify_crypto_ack(db2_vault_rewrap_tx_t *tx,
                                                             const uint8_t operation_id[16],
                                                             int64_t fence);

/* Frozen injection seam for D2b's exhaustive fake-DB state-machine tests. */
typedef struct
{
   db2_vault_rewrap_result_t (*tx_begin)(db2_vault_rewrap_tx_t **);
   db2_vault_rewrap_result_t (*tx_commit)(db2_vault_rewrap_tx_t **);
   void (*tx_rollback)(db2_vault_rewrap_tx_t **);
   db2_vault_rewrap_result_t (*snapshot)(const uint8_t[16], db2_vault_rewrap_snapshot_t *);
   db2_vault_rewrap_result_t (*begin)(db2_vault_rewrap_tx_t *, const char *, const char *,
                                      const uint8_t[16], int64_t, int64_t, int64_t *, int64_t *,
                                      db2_vault_rewrap_state_t *);
   db2_vault_rewrap_result_t (*record_prepared)(db2_vault_rewrap_tx_t *, const uint8_t[16], int64_t,
                                                int64_t, int64_t,
                                                const uint8_t[VAULT_RESEAL_RECEIPT_V1_LEN]);
   db2_vault_rewrap_result_t (*source_secret_page)(db2_vault_rewrap_tx_t *, const uint8_t[16],
                                                   int64_t, int64_t, int,
                                                   db2_vault_rewrap_secret_t *, size_t, size_t *);
   db2_vault_rewrap_result_t (*source_check_page)(db2_vault_rewrap_tx_t *, const uint8_t[16],
                                                  int64_t, const db2_vault_rewrap_cursor_t *, int,
                                                  db2_vault_rewrap_check_t *, size_t, size_t *,
                                                  db2_vault_rewrap_cursor_t *);
   db2_vault_rewrap_result_t (*stage_dek)(db2_vault_rewrap_tx_t *, const uint8_t[16], int64_t,
                                          const db2_vault_rewrap_secret_t *,
                                          const uint8_t[VAULT_WRAPPED_DEK_LEN]);
   db2_vault_rewrap_result_t (*stage_check)(db2_vault_rewrap_tx_t *, const uint8_t[16], int64_t,
                                            const db2_vault_rewrap_check_t *, const uint8_t *,
                                            size_t);
   db2_vault_rewrap_result_t (*inventory_summary)(db2_vault_rewrap_tx_t *, const uint8_t[16],
                                                  int64_t, db2_vault_rewrap_inventory_summary_t *);
   db2_vault_rewrap_result_t (*stage_finish)(db2_vault_rewrap_tx_t *, const uint8_t[16], int64_t,
                                             const db2_vault_rewrap_inventory_summary_t *);
   db2_vault_rewrap_result_t (*mark_committing)(db2_vault_rewrap_tx_t *, const uint8_t[16],
                                                int64_t);
   db2_vault_rewrap_result_t (*mark_resealed)(db2_vault_rewrap_tx_t *, const uint8_t[16], int64_t,
                                              const uint8_t[32]);
   db2_vault_rewrap_result_t (*promote)(db2_vault_rewrap_tx_t *, const uint8_t[16], int64_t);
   db2_vault_rewrap_result_t (*abort)(db2_vault_rewrap_tx_t *, const uint8_t[16], int64_t,
                                      const char *);
   db2_vault_rewrap_result_t (*recovery_required)(db2_vault_rewrap_tx_t *, const uint8_t[16],
                                                  int64_t, const char *);
   db2_vault_rewrap_result_t (*verify_summary)(db2_vault_rewrap_tx_t *, const uint8_t[16], int64_t,
                                               db2_vault_rewrap_verify_summary_t *);
   db2_vault_rewrap_result_t (*verify_secret_page)(db2_vault_rewrap_tx_t *, const uint8_t[16],
                                                   int64_t, int64_t, int,
                                                   db2_vault_rewrap_secret_t *, size_t, size_t *);
   db2_vault_rewrap_result_t (*verify_check_page)(db2_vault_rewrap_tx_t *, const uint8_t[16],
                                                  int64_t, const db2_vault_rewrap_cursor_t *, int,
                                                  db2_vault_rewrap_check_t *, size_t, size_t *,
                                                  db2_vault_rewrap_cursor_t *);
   db2_vault_rewrap_result_t (*verify_crypto_ack)(db2_vault_rewrap_tx_t *, const uint8_t[16],
                                                  int64_t);
   db2_vault_rewrap_result_t (*complete)(db2_vault_rewrap_tx_t *, const uint8_t[16], int64_t,
                                         const uint8_t[32], const uint8_t[32], const uint8_t[32]);
} db2_vault_rewrap_ops_t;

extern const db2_vault_rewrap_ops_t db2_vault_rewrap_default_ops;

/* Stable SQLSTATE-only mapping shared by every wrapper operation. */
db2_vault_rewrap_result_t db2_vault_rewrap_classify_sqlstate(const char *sqlstate);

#endif
