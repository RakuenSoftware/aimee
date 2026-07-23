#ifndef AIMEE_DB2_VAULT_OPERATOR_REWRAP_RUNTIME_H
#define AIMEE_DB2_VAULT_OPERATOR_REWRAP_RUNTIME_H

#include "org_vault_rewrap.h"
#include "vault_operator_status_runtime.h"

/* The frozen D2 seam has no context argument.  D3b therefore binds exactly one
 * already-open dedicated operator runtime for the daemon lifetime. */
int db2_vault_operator_rewrap_bind(db2_vault_operator_runtime_t *runtime);
void db2_vault_operator_rewrap_unbind(db2_vault_operator_runtime_t *runtime);

/* Recover a connection whose last statement result was lost.  Returns one
 * only after a new PostgreSQL session has re-established the fixed login and
 * SET ROLE authority, zero when there is no uncertain result, and minus one
 * when recovery itself is unavailable. */
int db2_vault_operator_rewrap_recover_uncertain(void);

extern const db2_vault_rewrap_ops_t db2_vault_operator_rewrap_ops;

typedef struct
{
   uint8_t operation_id[16];
   uint8_t request_id[16];
   db2_vault_rewrap_state_t state;
   int64_t seal_epoch, fencing_token, old_generation, new_generation;
} db2_vault_operator_rewrap_binding_t;

typedef struct
{
   db2_vault_operator_rewrap_binding_t binding;
   uint8_t receipt[VAULT_RESEAL_RECEIPT_V1_LEN];
   uint8_t receipt_digest[32], inventory_digest[32], stage_digest[32];
   int64_t secret_count, check_count;
} db2_vault_operator_completed_t;

typedef struct
{
   int64_t opened_epoch, opened_fence;
   uint8_t event_id[32], row_hash[32];
} db2_vault_operator_open_result_t;
typedef struct
{
   db2_vault_operator_open_result_t opened;
   int completed_open;
   int operation_present;
   uint8_t operation_id[16], request_id[16];
   int64_t operation_fence;
} db2_vault_operator_open_event_t;

int db2_vault_operator_dispatch(const uint8_t request_id[16],
                                db2_vault_operator_rewrap_binding_t *out, int *found);
int db2_vault_operator_reserve(const uint8_t request_id[16], const uint8_t candidate_op[16],
                               int64_t old_generation, int64_t new_generation,
                               db2_vault_operator_rewrap_binding_t *out, int *created);
int db2_vault_operator_active(db2_vault_operator_rewrap_binding_t *out, int *found);
int db2_vault_operator_completed(const uint8_t request_id[16], const uint8_t operation_id[16],
                                 db2_vault_operator_completed_t *out);
int db2_vault_operator_completed_active(const uint8_t operation_id[16],
                                        db2_vault_operator_completed_t *out);
int db2_vault_operator_current_check_page(const db2_vault_rewrap_cursor_t *after, int limit,
                                          db2_vault_rewrap_check_t *rows, size_t capacity,
                                          size_t *count, db2_vault_rewrap_cursor_t *next,
                                          int64_t *total_count);
int db2_vault_operator_open_completed(const db2_vault_operator_completed_t *completed,
                                      db2_vault_operator_open_result_t *out);
int db2_vault_operator_open_idle(const uint8_t request_id[16], int64_t epoch, int64_t fence,
                                 int64_t marker, db2_vault_operator_open_result_t *out);
int db2_vault_operator_open_event(const uint8_t event_id[32], db2_vault_operator_open_event_t *out);

#endif
