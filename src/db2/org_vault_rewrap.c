#include "org_vault_rewrap.h"

#include "db2_internal.h"
#include "db2_pool.h"
#include "db_postgres.h"

#include <openssl/crypto.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define RW_ERR 256

static int utf8_valid(const uint8_t *s, size_t n);
static int cursor_cmp(const uint8_t *a, size_t an, const uint8_t *b, size_t bn);

struct db2_vault_rewrap_tx
{
   void *conn;
   pthread_t owner;
   enum
   {
      RW_PHASE_GENERAL = 1,
      RW_PHASE_SINGLE_DONE,
      RW_PHASE_STAGING,
      RW_PHASE_STAGING_DONE,
      RW_PHASE_PROMOTION_DONE,
      RW_PHASE_VERIFY_SECRETS,
      RW_PHASE_VERIFY_CHECKS,
      RW_PHASE_VERIFY_CONSUMED,
      RW_PHASE_CRYPTO_ACKED,
      RW_PHASE_COMPLETED,
      RW_PHASE_FAILED
   } phase;
   enum
   {
      RW_KIND_NONE = 0,
      RW_KIND_SINGLE,
      RW_KIND_STAGING,
      RW_KIND_PROMOTION,
      RW_KIND_VERIFY
   } kind;
   uint8_t operation_id[16];
   int64_t fence, expected_secrets, expected_checks, consumed_secrets, consumed_checks;
   int64_t last_secret_id;
   db2_vault_rewrap_cursor_t check_cursor;
   int secret_exhausted, check_exhausted;
   uint8_t receipt_digest[32], inventory_digest[32], stage_digest[32];
   db2_vault_rewrap_result_t prepare_error;
};

static int tx_owned(const db2_vault_rewrap_tx_t *tx)
{
   return tx && tx->conn && tx->phase != RW_PHASE_FAILED &&
          pthread_equal(tx->owner, pthread_self());
}

static db2_vault_rewrap_result_t tx_fail(db2_vault_rewrap_tx_t *tx, db2_vault_rewrap_result_t rc)
{
   if (tx && pthread_equal(tx->owner, pthread_self()))
      tx->phase = RW_PHASE_FAILED;
   return rc;
}

static db2_vault_rewrap_result_t claim_kind(db2_vault_rewrap_tx_t *tx, int kind)
{
   if (!tx_owned(tx))
      return DB2_VAULT_REWRAP_INVALID;
   if (tx->kind == RW_KIND_NONE && tx->phase == RW_PHASE_GENERAL)
   {
      tx->kind = kind;
      if (kind == RW_KIND_STAGING)
         tx->phase = RW_PHASE_STAGING;
      return DB2_VAULT_REWRAP_OK;
   }
   if (kind == RW_KIND_STAGING && tx->kind == RW_KIND_STAGING && tx->phase == RW_PHASE_STAGING)
      return DB2_VAULT_REWRAP_OK;
   return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
}

static db2_vault_rewrap_result_t prepare_or_bind_failure(db2_vault_rewrap_tx_t *tx, int had_stmt)
{
   return tx_fail(tx, had_stmt ? DB2_VAULT_REWRAP_INVALID : tx->prepare_error);
}

static int state_parse(const char *s, db2_vault_rewrap_state_t *out)
{
   static const char *const names[] = {
       "preparing", "custody_prepared", "wraps_staged", "reseal_committing", "resealed",
       "promoted",  "completed",        "aborted",      "recovery_required"};
   if (!s || !out)
      return -1;
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
      if (strcmp(s, names[i]) == 0)
      {
         *out = (db2_vault_rewrap_state_t)i;
         return 0;
      }
   return -1;
}

#define RW_STATE_BIT(state) (UINT32_C(1) << (state))
#define RW_STATES_ALL       ((UINT32_C(1) << (DB2_VAULT_REWRAP_RECOVERY_REQUIRED + 1)) - 1)

db2_vault_rewrap_result_t db2_vault_rewrap_classify_sqlstate(const char *s)
{
   if (!s || strlen(s) != 5)
      return DB2_VAULT_REWRAP_ERROR;
   if (strcmp(s, "55000") == 0)
      return DB2_VAULT_REWRAP_BUSY;
   if (strcmp(s, "P7C01") == 0 || strcmp(s, "23505") == 0)
      return DB2_VAULT_REWRAP_CONFLICT;
   if (strcmp(s, "P7I01") == 0 || strcmp(s, "P7B01") == 0)
      return DB2_VAULT_REWRAP_INTEGRITY;
   if (strcmp(s, "40001") == 0 || strcmp(s, "40P01") == 0)
      return DB2_VAULT_REWRAP_TRANSIENT;
   if (strcmp(s, "22023") == 0 || strcmp(s, "22003") == 0 || strcmp(s, "25001") == 0)
      return DB2_VAULT_REWRAP_INVALID;
   if (!strncmp(s, "08", 2) || !strncmp(s, "53", 2) || !strncmp(s, "57", 2))
      return DB2_VAULT_REWRAP_TRANSIENT;
   return DB2_VAULT_REWRAP_ERROR;
}

static db2_vault_rewrap_result_t step(aimee_pg_stmt_t *st, aimee_pg_step_t *value)
{
   char err[RW_ERR] = "";
   *value = aimee_pg_step(st, err, sizeof(err));
   return *value == AIMEE_PG_ERR ? db2_vault_rewrap_classify_sqlstate(aimee_pg_sqlstate(st))
                                 : DB2_VAULT_REWRAP_OK;
}

static db2_vault_rewrap_result_t expect_done(aimee_pg_stmt_t *st)
{
   aimee_pg_step_t value;
   db2_vault_rewrap_result_t rc = step(st, &value);
   return rc != DB2_VAULT_REWRAP_OK
              ? rc
              : (value == AIMEE_PG_DONE ? DB2_VAULT_REWRAP_OK : DB2_VAULT_REWRAP_INTEGRITY);
}

static int sqlstate_poisoned(const char *state)
{
   return state && (!strncmp(state, "08", 2) || !strncmp(state, "57", 2));
}

static int blob_exact(aimee_pg_stmt_t *st, int col, void *out, int n)
{
   if (aimee_pg_column_is_null(st, col) || aimee_pg_column_bytes(st, col) != n)
      return -1;
   const void *p = aimee_pg_column_blob(st, col);
   if (!p)
      return -1;
   memcpy(out, p, (size_t)n);
   return 0;
}

static int text_copy(aimee_pg_stmt_t *st, int col, char *out, size_t cap)
{
   const char *p = aimee_pg_column_text(st, col);
   if (!p || strlen(p) >= cap)
      return -1;
   memcpy(out, p, strlen(p) + 1);
   return 0;
}

static int failure_class_valid(const char *s)
{
   size_t n = s ? strlen(s) : 0;
   if (n == 0)
      return 1;
   if (n > 64 || s[0] < 'a' || s[0] > 'z')
      return 0;
   for (size_t i = 1; i < n; i++)
      if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
         return 0;
   return 1;
}

static int snapshot_shape_valid(const db2_vault_rewrap_snapshot_t *o)
{
   int no_failure = !o->failure_class[0] && !o->has_failure_from_state;
   int none = !o->has_receipt && !o->has_inventory && !o->has_stage && o->secret_count == 0 &&
              o->check_count == 0;
   int prepared = o->has_receipt && !o->has_inventory && !o->has_stage && o->secret_count == 0 &&
                  o->check_count == 0;
   int staged = o->has_receipt && o->has_inventory && o->has_stage;
   switch (o->state)
   {
   case DB2_VAULT_REWRAP_PREPARING:
      return no_failure && none;
   case DB2_VAULT_REWRAP_CUSTODY_PREPARED:
      return no_failure && prepared;
   case DB2_VAULT_REWRAP_WRAPS_STAGED:
   case DB2_VAULT_REWRAP_RESEAL_COMMITTING:
   case DB2_VAULT_REWRAP_RESEALED:
   case DB2_VAULT_REWRAP_PROMOTED:
   case DB2_VAULT_REWRAP_COMPLETED:
      return no_failure && staged;
   case DB2_VAULT_REWRAP_ABORTED:
      return o->failure_class[0] && !o->has_failure_from_state && (none || prepared || staged);
   case DB2_VAULT_REWRAP_RECOVERY_REQUIRED:
      if (!o->failure_class[0] || !o->has_failure_from_state)
         return 0;
      if (o->failure_from_state == DB2_VAULT_REWRAP_PREPARING)
         return none;
      if (o->failure_from_state == DB2_VAULT_REWRAP_CUSTODY_PREPARED)
         return prepared;
      return o->failure_from_state >= DB2_VAULT_REWRAP_WRAPS_STAGED &&
             o->failure_from_state <= DB2_VAULT_REWRAP_PROMOTED && staged;
   }
   return 0;
}

void db2_vault_rewrap_snapshot_clear(db2_vault_rewrap_snapshot_t *s)
{
   if (s)
      OPENSSL_cleanse(s, sizeof(*s));
}

void db2_vault_rewrap_secret_clear(db2_vault_rewrap_secret_t *rows, size_t count)
{
   if (rows && count <= DB2_VAULT_REWRAP_PAGE_MAX)
      OPENSSL_cleanse(rows, count * sizeof(*rows));
}

void db2_vault_rewrap_check_clear(db2_vault_rewrap_check_t *rows, size_t count)
{
   if (rows && count <= DB2_VAULT_REWRAP_PAGE_MAX)
      OPENSSL_cleanse(rows, count * sizeof(*rows));
}

void db2_vault_rewrap_cursor_clear(db2_vault_rewrap_cursor_t *cursor)
{
   if (cursor)
      OPENSSL_cleanse(cursor, sizeof(*cursor));
}

void db2_vault_rewrap_verify_summary_clear(db2_vault_rewrap_verify_summary_t *summary)
{
   if (summary)
      OPENSSL_cleanse(summary, sizeof(*summary));
}

static int snapshot_decode(aimee_pg_stmt_t *st, db2_vault_rewrap_snapshot_t *o)
{
   char ophex[33];
   if (text_copy(st, 0, ophex, sizeof(ophex)) != 0 ||
       vault_reseal_operation_id_from_hex(ophex, o->operation_id) != 0 ||
       state_parse(aimee_pg_column_text(st, 1), &o->state) != 0)
      return -1;
   if (aimee_pg_column_is_null(st, 2) || aimee_pg_column_is_null(st, 3) ||
       aimee_pg_column_is_null(st, 4) || aimee_pg_column_is_null(st, 5) ||
       aimee_pg_column_is_null(st, 8) || aimee_pg_column_is_null(st, 9))
      return -1;
   o->seal_epoch = aimee_pg_column_int64(st, 2);
   o->fencing_token = aimee_pg_column_int64(st, 3);
   o->old_generation = aimee_pg_column_int64(st, 4);
   o->new_generation = aimee_pg_column_int64(st, 5);
   if (o->seal_epoch < 1 || o->fencing_token < 1 || o->old_generation < 0 ||
       o->old_generation == INT64_MAX || o->new_generation != o->old_generation + 1)
      return -1;
   if (!aimee_pg_column_is_null(st, 6))
   {
      vault_tpm2_reseal_receipt_t receipt;
      uint8_t digest[32], op[16];
      if (blob_exact(st, 6, o->receipt, sizeof(o->receipt)) != 0 ||
          blob_exact(st, 7, o->receipt_digest, 32) != 0 ||
          vault_reseal_receipt_decode(o->receipt, sizeof(o->receipt), &receipt) != 0 ||
          vault_reseal_receipt_digest(o->receipt, digest) != 0 ||
          CRYPTO_memcmp(digest, o->receipt_digest, 32) != 0 ||
          vault_reseal_operation_id_from_hex(ophex, op) != 0 ||
          CRYPTO_memcmp(op, receipt.operation_id, 16) != 0 ||
          receipt.old_generation != (uint64_t)o->old_generation ||
          receipt.new_generation != (uint64_t)o->new_generation)
      {
         OPENSSL_cleanse(&receipt, sizeof(receipt));
         OPENSSL_cleanse(digest, sizeof(digest));
         OPENSSL_cleanse(op, sizeof(op));
         return -1;
      }
      OPENSSL_cleanse(&receipt, sizeof(receipt));
      OPENSSL_cleanse(digest, sizeof(digest));
      OPENSSL_cleanse(op, sizeof(op));
      o->has_receipt = 1;
   }
   else if (!aimee_pg_column_is_null(st, 7))
      return -1;
   o->secret_count = aimee_pg_column_int64(st, 8);
   o->check_count = aimee_pg_column_int64(st, 9);
   if (o->secret_count < 0 || o->check_count < 0)
      return -1;
   if (!aimee_pg_column_is_null(st, 10))
   {
      if (blob_exact(st, 10, o->inventory_digest, 32) != 0)
         return -1;
      o->has_inventory = 1;
   }
   if (!aimee_pg_column_is_null(st, 11))
   {
      if (blob_exact(st, 11, o->stage_digest, 32) != 0)
         return -1;
      o->has_stage = 1;
   }
   if (o->has_inventory != o->has_stage)
      return -1;
   if (!aimee_pg_column_is_null(st, 12) &&
       text_copy(st, 12, o->failure_class, sizeof(o->failure_class)) != 0)
      return -1;
   if (!aimee_pg_column_is_null(st, 13))
   {
      if (state_parse(aimee_pg_column_text(st, 13), &o->failure_from_state) != 0)
         return -1;
      o->has_failure_from_state = 1;
   }
   return failure_class_valid(o->failure_class) && snapshot_shape_valid(o) ? 0 : -1;
}

db2_vault_rewrap_result_t db2_vault_rewrap_snapshot(const uint8_t operation_id[16],
                                                    db2_vault_rewrap_snapshot_t *out)
{
   if (out)
      db2_vault_rewrap_snapshot_clear(out);
   if (!operation_id || !out)
      return DB2_VAULT_REWRAP_INVALID;
   char op[33], err[RW_ERR] = "";
   if (vault_reseal_operation_id_to_hex(operation_id, op) != 0)
      return DB2_VAULT_REWRAP_INVALID;
   if (!db2_pool_active())
      return DB2_VAULT_REWRAP_TRANSIENT;
   void *conn = db2_pool_lease(0);
   aimee_pg_prepare_error_t prepare_error = AIMEE_PG_PREPARE_OK;
   aimee_pg_stmt_t *st =
       conn ? aimee_pg_prepare_ex(conn, "SELECT * FROM org_vault_rewrap_snapshot(?1)",
                                  &prepare_error, err, sizeof(err))
            : NULL;
   if (!st || aimee_pg_bind_text(st, "?1", op) != 0)
   {
      aimee_pg_finalize(st);
      if (conn)
         db2_pool_return(conn);
      return conn ? (prepare_error == AIMEE_PG_PREPARE_RESOURCE ? DB2_VAULT_REWRAP_ERROR
                                                                : DB2_VAULT_REWRAP_INVALID)
                  : DB2_VAULT_REWRAP_TRANSIENT;
   }
   aimee_pg_step_t sr;
   db2_vault_rewrap_result_t rc = step(st, &sr);
   if (rc == DB2_VAULT_REWRAP_OK && sr == AIMEE_PG_DONE)
      rc = DB2_VAULT_REWRAP_NOT_FOUND;
   else if (rc == DB2_VAULT_REWRAP_OK && (sr != AIMEE_PG_ROW || snapshot_decode(st, out) != 0 ||
                                          CRYPTO_memcmp(out->operation_id, operation_id, 16) != 0))
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   else if (rc == DB2_VAULT_REWRAP_OK)
      rc = expect_done(st);
   int discard = sqlstate_poisoned(aimee_pg_sqlstate(st));
   aimee_pg_finalize(st);
   if (discard)
      db2_pool_discard(conn);
   else
      db2_pool_return(conn);
   if (rc != DB2_VAULT_REWRAP_OK)
      db2_vault_rewrap_snapshot_clear(out);
   return rc;
}

db2_vault_rewrap_result_t db2_vault_rewrap_tx_begin(db2_vault_rewrap_tx_t **out)
{
   if (!out || *out)
      return DB2_VAULT_REWRAP_INVALID;
   db2_vault_rewrap_tx_t *tx = calloc(1, sizeof(*tx));
   if (!tx)
      return DB2_VAULT_REWRAP_ERROR;
   if (!db2_pool_active() || !(tx->conn = db2_pool_lease(0)))
   {
      free(tx);
      return DB2_VAULT_REWRAP_TRANSIENT;
   }
   char state[6] = "", err[RW_ERR] = "";
   if (aimee_pg_exec_sqlstate(tx->conn, "BEGIN ISOLATION LEVEL SERIALIZABLE", state, err,
                              sizeof(err)) != 0)
   {
      db2_vault_rewrap_result_t rc = db2_vault_rewrap_classify_sqlstate(state);
      db2_pool_discard(tx->conn);
      OPENSSL_cleanse(tx, sizeof(*tx));
      free(tx);
      return rc == DB2_VAULT_REWRAP_ERROR ? DB2_VAULT_REWRAP_TRANSIENT : rc;
   }
   tx->owner = pthread_self();
   tx->phase = RW_PHASE_GENERAL;
   *out = tx;
   return DB2_VAULT_REWRAP_OK;
}

static db2_vault_rewrap_result_t tx_end(db2_vault_rewrap_tx_t **p, int commit)
{
   if (!p || !*p || !(*p)->conn || !pthread_equal((*p)->owner, pthread_self()))
      return DB2_VAULT_REWRAP_INVALID;
   db2_vault_rewrap_tx_t *tx = *p;
   if (commit && tx->phase != RW_PHASE_SINGLE_DONE && tx->phase != RW_PHASE_STAGING_DONE &&
       tx->phase != RW_PHASE_PROMOTION_DONE && tx->phase != RW_PHASE_COMPLETED)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   char state[6] = "", err[RW_ERR] = "";
   int erc =
       aimee_pg_exec_sqlstate(tx->conn, commit ? "COMMIT" : "ROLLBACK", state, err, sizeof(err));
   void *conn = tx->conn;
   db2_vault_rewrap_result_t rc =
       erc == 0 ? DB2_VAULT_REWRAP_OK
                : (commit ? DB2_VAULT_REWRAP_TRANSIENT : db2_vault_rewrap_classify_sqlstate(state));
   OPENSSL_cleanse(tx, sizeof(*tx));
   free(tx);
   *p = NULL;
   if (erc == 0)
      db2_pool_return(conn);
   else
      db2_pool_discard(conn);
   return rc == DB2_VAULT_REWRAP_ERROR ? DB2_VAULT_REWRAP_TRANSIENT : rc;
}

db2_vault_rewrap_result_t db2_vault_rewrap_tx_commit(db2_vault_rewrap_tx_t **tx)
{
   return tx_end(tx, 1);
}
void db2_vault_rewrap_tx_rollback(db2_vault_rewrap_tx_t **tx)
{
   if (tx && *tx)
      (void)tx_end(tx, 0);
}

static aimee_pg_stmt_t *prepare_op(db2_vault_rewrap_tx_t *tx, const char *sql,
                                   const uint8_t operation_id[16], int64_t fence)
{
   if (!tx_owned(tx))
      return NULL;
   tx->prepare_error = DB2_VAULT_REWRAP_INVALID;
   if ((tx->phase != RW_PHASE_GENERAL && tx->phase != RW_PHASE_STAGING) || !operation_id ||
       fence < 1)
   {
      tx->phase = RW_PHASE_FAILED;
      return NULL;
   }
   char op[33], err[RW_ERR] = "";
   if (vault_reseal_operation_id_to_hex(operation_id, op) != 0)
      return NULL;
   aimee_pg_prepare_error_t prepare_error = AIMEE_PG_PREPARE_OK;
   aimee_pg_stmt_t *st = aimee_pg_prepare_ex(tx->conn, sql, &prepare_error, err, sizeof(err));
   if (!st && prepare_error == AIMEE_PG_PREPARE_RESOURCE)
      tx->prepare_error = DB2_VAULT_REWRAP_ERROR;
   if (!st || aimee_pg_bind_text(st, "?1", op) != 0 || aimee_pg_bind_int64(st, "?2", fence) != 0)
   {
      aimee_pg_finalize(st);
      return NULL;
   }
   return st;
}

static aimee_pg_stmt_t *prepare_verify(db2_vault_rewrap_tx_t *tx, const char *sql,
                                       const uint8_t operation_id[16], int64_t fence)
{
   if (!tx_owned(tx) || !operation_id || fence < 1)
      return NULL;
   tx->prepare_error = DB2_VAULT_REWRAP_INVALID;
   char op[33], err[RW_ERR] = "";
   if (vault_reseal_operation_id_to_hex(operation_id, op) != 0)
      return NULL;
   aimee_pg_prepare_error_t prepare_error = AIMEE_PG_PREPARE_OK;
   aimee_pg_stmt_t *st = aimee_pg_prepare_ex(tx->conn, sql, &prepare_error, err, sizeof(err));
   if (!st && prepare_error == AIMEE_PG_PREPARE_RESOURCE)
      tx->prepare_error = DB2_VAULT_REWRAP_ERROR;
   if (!st || aimee_pg_bind_text(st, "?1", op) != 0 || aimee_pg_bind_int64(st, "?2", fence) != 0)
   {
      aimee_pg_finalize(st);
      return NULL;
   }
   return st;
}

static db2_vault_rewrap_result_t state_scalar(aimee_pg_stmt_t *st, uint32_t allowed_states,
                                              db2_vault_rewrap_state_t *state_out)
{
   if (!st)
      return DB2_VAULT_REWRAP_INVALID;
   aimee_pg_step_t sr;
   db2_vault_rewrap_result_t rc = step(st, &sr);
   if (rc == DB2_VAULT_REWRAP_OK && sr == AIMEE_PG_ROW)
   {
      db2_vault_rewrap_state_t state;
      if (state_parse(aimee_pg_column_text(st, 0), &state) != 0 ||
          !(allowed_states & RW_STATE_BIT(state)))
         rc = DB2_VAULT_REWRAP_INTEGRITY;
      else
      {
         rc = expect_done(st);
         if (rc == DB2_VAULT_REWRAP_OK && state_out)
            *state_out = state;
      }
   }
   else if (rc == DB2_VAULT_REWRAP_OK)
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   aimee_pg_finalize(st);
   return rc;
}

db2_vault_rewrap_result_t db2_vault_rewrap_begin(db2_vault_rewrap_tx_t *tx, const char *actor,
                                                 const char *request,
                                                 const uint8_t operation_id[16], int64_t oldg,
                                                 int64_t newg, int64_t *epoch, int64_t *fence,
                                                 db2_vault_rewrap_state_t *state)
{
   if (epoch)
      *epoch = 0;
   if (fence)
      *fence = 0;
   if (state)
      OPENSSL_cleanse(state, sizeof(*state));
   if (!tx_owned(tx) || tx->phase != RW_PHASE_GENERAL || !actor || !actor[0] || !request ||
       !request[0] || !operation_id || oldg < 0 || oldg == INT64_MAX || newg != oldg + 1 ||
       !epoch || !fence || !state)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   if (claim_kind(tx, RW_KIND_SINGLE) != DB2_VAULT_REWRAP_OK)
      return DB2_VAULT_REWRAP_INVALID;
   char op[33], err[RW_ERR] = "";
   if (vault_reseal_operation_id_to_hex(operation_id, op) != 0)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   aimee_pg_prepare_error_t prepare_error = AIMEE_PG_PREPARE_OK;
   aimee_pg_stmt_t *st =
       aimee_pg_prepare_ex(tx->conn, "SELECT * FROM org_vault_rewrap_begin(?1,?2,?3,?4,?5)",
                           &prepare_error, err, sizeof(err));
   if (!st || aimee_pg_bind_text(st, "?1", actor) != 0 ||
       aimee_pg_bind_text(st, "?2", request) != 0 || aimee_pg_bind_text(st, "?3", op) != 0 ||
       aimee_pg_bind_int64(st, "?4", oldg) != 0 || aimee_pg_bind_int64(st, "?5", newg) != 0)
   {
      aimee_pg_finalize(st);
      return tx_fail(tx, prepare_error == AIMEE_PG_PREPARE_RESOURCE ? DB2_VAULT_REWRAP_ERROR
                                                                    : DB2_VAULT_REWRAP_INVALID);
   }
   aimee_pg_step_t sr;
   db2_vault_rewrap_result_t rc = step(st, &sr);
   uint8_t returned_operation_id[16] = {0};
   if (rc == DB2_VAULT_REWRAP_OK && sr == AIMEE_PG_ROW &&
       vault_reseal_operation_id_from_hex(aimee_pg_column_text(st, 0), returned_operation_id) ==
           0 &&
       CRYPTO_memcmp(returned_operation_id, operation_id, 16) == 0 &&
       state_parse(aimee_pg_column_text(st, 1), state) == 0 &&
       (RW_STATES_ALL & RW_STATE_BIT(*state)))
   {
      *epoch = aimee_pg_column_int64(st, 2);
      *fence = aimee_pg_column_int64(st, 3);
      if (*epoch < 1 || *fence < 1)
         rc = DB2_VAULT_REWRAP_INTEGRITY;
      else
         rc = expect_done(st);
   }
   else if (rc == DB2_VAULT_REWRAP_OK)
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   OPENSSL_cleanse(returned_operation_id, sizeof(returned_operation_id));
   aimee_pg_finalize(st);
   if (rc != DB2_VAULT_REWRAP_OK)
   {
      *epoch = *fence = 0;
      OPENSSL_cleanse(state, sizeof(*state));
   }
   else
      tx->phase = RW_PHASE_SINGLE_DONE;
   return rc == DB2_VAULT_REWRAP_OK ? rc : tx_fail(tx, rc);
}

db2_vault_rewrap_result_t
db2_vault_rewrap_record_prepared(db2_vault_rewrap_tx_t *tx, const uint8_t opid[16], int64_t fence,
                                 int64_t old_generation, int64_t new_generation,
                                 const uint8_t receipt[VAULT_RESEAL_RECEIPT_V1_LEN])
{
   uint8_t digest[32];
   vault_tpm2_reseal_receipt_t decoded;
   memset(&decoded, 0, sizeof(decoded));
   if (!opid || !receipt || old_generation < 0 || old_generation == INT64_MAX ||
       new_generation != old_generation + 1 ||
       vault_reseal_receipt_decode(receipt, VAULT_RESEAL_RECEIPT_V1_LEN, &decoded) != 0 ||
       CRYPTO_memcmp(decoded.operation_id, opid, 16) != 0 ||
       decoded.old_generation != (uint64_t)old_generation ||
       decoded.new_generation != (uint64_t)new_generation ||
       vault_reseal_receipt_digest(receipt, digest) != 0)
   {
      OPENSSL_cleanse(&decoded, sizeof(decoded));
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   }
   OPENSSL_cleanse(&decoded, sizeof(decoded));
   if (claim_kind(tx, RW_KIND_SINGLE) != DB2_VAULT_REWRAP_OK)
   {
      OPENSSL_cleanse(digest, sizeof(digest));
      return DB2_VAULT_REWRAP_INVALID;
   }
   aimee_pg_stmt_t *st =
       prepare_op(tx, "SELECT org_vault_rewrap_record_prepared(?1,?2,?3,?4)", opid, fence);
   if (!st || aimee_pg_bind_blob(st, "?3", receipt, VAULT_RESEAL_RECEIPT_V1_LEN) != 0 ||
       aimee_pg_bind_blob(st, "?4", digest, 32) != 0)
   {
      int had_stmt = st != NULL;
      aimee_pg_finalize(st);
      OPENSSL_cleanse(digest, 32);
      return prepare_or_bind_failure(tx, had_stmt);
   }
   uint32_t allowed = RW_STATES_ALL & ~RW_STATE_BIT(DB2_VAULT_REWRAP_PREPARING);
   db2_vault_rewrap_result_t rc = state_scalar(st, allowed, NULL);
   OPENSSL_cleanse(digest, 32);
   if (rc == DB2_VAULT_REWRAP_OK)
      tx->phase = RW_PHASE_SINGLE_DONE;
   return rc == DB2_VAULT_REWRAP_OK ? rc : tx_fail(tx, rc);
}

db2_vault_rewrap_result_t db2_vault_rewrap_source_secret_page(db2_vault_rewrap_tx_t *tx,
                                                              const uint8_t opid[16], int64_t fence,
                                                              int64_t after, int limit,
                                                              db2_vault_rewrap_secret_t *rows,
                                                              size_t cap, size_t *count)
{
   if (rows)
      db2_vault_rewrap_secret_clear(
          rows, cap > DB2_VAULT_REWRAP_PAGE_MAX ? DB2_VAULT_REWRAP_PAGE_MAX : cap);
   if (count)
      *count = 0;
   if (!rows || !count || after < 0 || limit < 1 || limit > DB2_VAULT_REWRAP_PAGE_MAX ||
       cap < (size_t)limit || cap > DB2_VAULT_REWRAP_PAGE_MAX)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   if (tx_owned(tx) && tx->secret_exhausted)
      return tx_fail(tx, DB2_VAULT_REWRAP_INTEGRITY);
   if (claim_kind(tx, RW_KIND_STAGING) != DB2_VAULT_REWRAP_OK || after != tx->last_secret_id)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   aimee_pg_stmt_t *st =
       prepare_op(tx, "SELECT * FROM org_vault_rewrap_secret_page(?1,?2,?3,?4)", opid, fence);
   if (!st || aimee_pg_bind_int64(st, "?3", after) != 0 || aimee_pg_bind_int(st, "?4", limit) != 0)
   {
      int had_stmt = st != NULL;
      aimee_pg_finalize(st);
      return prepare_or_bind_failure(tx, had_stmt);
   }
   db2_vault_rewrap_result_t rc = DB2_VAULT_REWRAP_OK;
   aimee_pg_step_t sr;
   while ((rc = step(st, &sr)) == DB2_VAULT_REWRAP_OK && sr == AIMEE_PG_ROW)
   {
      if (*count >= (size_t)limit)
      {
         rc = DB2_VAULT_REWRAP_INTEGRITY;
         break;
      }
      db2_vault_rewrap_secret_t *r = &rows[(*count)++];
      r->source_id = aimee_pg_column_int64(st, 0);
      r->version = aimee_pg_column_int64(st, 4);
      if (r->source_id <= after || r->version < 1 ||
          text_copy(st, 1, r->principal, sizeof(r->principal)) != 0 ||
          text_copy(st, 2, r->agent, sizeof(r->agent)) != 0 ||
          text_copy(st, 3, r->cred, sizeof(r->cred)) != 0 ||
          blob_exact(st, 5, r->source_digest, 32) != 0 ||
          blob_exact(st, 6, r->wrapped_dek, VAULT_WRAPPED_DEK_LEN) != 0)
      {
         rc = DB2_VAULT_REWRAP_INTEGRITY;
         break;
      }
      after = r->source_id;
   }
   aimee_pg_finalize(st);
   if (rc != DB2_VAULT_REWRAP_OK)
   {
      db2_vault_rewrap_secret_clear(rows, cap);
      *count = 0;
      return tx_fail(tx, rc);
   }
   if (*count)
      tx->last_secret_id = rows[*count - 1].source_id;
   else
      tx->secret_exhausted = 1;
   return rc;
}

db2_vault_rewrap_result_t
db2_vault_rewrap_source_check_page(db2_vault_rewrap_tx_t *tx, const uint8_t opid[16], int64_t fence,
                                   const db2_vault_rewrap_cursor_t *after, int limit,
                                   db2_vault_rewrap_check_t *rows, size_t cap, size_t *count,
                                   db2_vault_rewrap_cursor_t *next)
{
   static const uint8_t empty = 0;
   if (rows)
      db2_vault_rewrap_check_clear(rows, cap > DB2_VAULT_REWRAP_PAGE_MAX ? DB2_VAULT_REWRAP_PAGE_MAX
                                                                         : cap);
   if (count)
      *count = 0;
   db2_vault_rewrap_cursor_t after_copy;
   int after_valid = after && after->len <= sizeof(after->bytes);
   if (after_valid)
      after_copy = *after;
   if (next)
      db2_vault_rewrap_cursor_clear(next);
   if (!rows || !count || !after_valid || !next || limit < 1 || limit > DB2_VAULT_REWRAP_PAGE_MAX ||
       cap < (size_t)limit || cap > DB2_VAULT_REWRAP_PAGE_MAX)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   after = &after_copy;
   if (tx_owned(tx) && tx->check_exhausted)
      return tx_fail(tx, DB2_VAULT_REWRAP_INTEGRITY);
   if (claim_kind(tx, RW_KIND_STAGING) != DB2_VAULT_REWRAP_OK ||
       after->len != tx->check_cursor.len ||
       CRYPTO_memcmp(after->bytes, tx->check_cursor.bytes, after->len) != 0)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   *next = *after;
   aimee_pg_stmt_t *st =
       prepare_op(tx, "SELECT * FROM org_vault_rewrap_check_page(?1,?2,?3,?4)", opid, fence);
   if (!st ||
       aimee_pg_bind_blob(st, "?3", after->len ? after->bytes : &empty, (int)after->len) != 0 ||
       aimee_pg_bind_int(st, "?4", limit) != 0)
   {
      int had_stmt = st != NULL;
      aimee_pg_finalize(st);
      db2_vault_rewrap_cursor_clear(next);
      return prepare_or_bind_failure(tx, had_stmt);
   }
   db2_vault_rewrap_result_t rc = DB2_VAULT_REWRAP_OK;
   aimee_pg_step_t sr;
   while ((rc = step(st, &sr)) == DB2_VAULT_REWRAP_OK && sr == AIMEE_PG_ROW)
   {
      if (*count >= (size_t)limit)
      {
         rc = DB2_VAULT_REWRAP_INTEGRITY;
         break;
      }
      db2_vault_rewrap_check_t *r = &rows[(*count)++];
      if (text_copy(st, 0, r->principal, sizeof(r->principal)) != 0 ||
          blob_exact(st, 1, r->source_digest, 32) != 0 || aimee_pg_column_is_null(st, 2) ||
          aimee_pg_column_is_null(st, 3))
      {
         rc = DB2_VAULT_REWRAP_INTEGRITY;
         break;
      }
      int n = aimee_pg_column_bytes(st, 2);
      if (n != 0 && n != VAULT_WRAPPED_DEK_LEN)
      {
         rc = DB2_VAULT_REWRAP_INTEGRITY;
         break;
      }
      if (n)
      {
         const void *p = aimee_pg_column_blob(st, 2);
         if (!p)
         {
            rc = DB2_VAULT_REWRAP_INTEGRITY;
            break;
         }
         memcpy(r->kek_check, p, (size_t)n);
      }
      r->kek_check_len = (size_t)n;
      int cn = aimee_pg_column_bytes(st, 3);
      const uint8_t *cursor = aimee_pg_column_blob(st, 3);
      size_t pn = strlen(r->principal);
      if (cn < 1 || cn > (int)sizeof(next->bytes) || !cursor || pn != (size_t)cn ||
          memcmp(r->principal, cursor, pn) != 0 || !utf8_valid(cursor, (size_t)cn) ||
          cursor_cmp(cursor, (size_t)cn, next->bytes, next->len) <= 0)
      {
         rc = DB2_VAULT_REWRAP_INTEGRITY;
         break;
      }
      memcpy(next->bytes, cursor, (size_t)cn);
      next->len = (size_t)cn;
   }
   aimee_pg_finalize(st);
   if (rc != DB2_VAULT_REWRAP_OK)
   {
      db2_vault_rewrap_check_clear(rows, cap);
      *count = 0;
      db2_vault_rewrap_cursor_clear(next);
      return tx_fail(tx, rc);
   }
   tx->check_cursor = *next;
   if (*count == 0)
      tx->check_exhausted = 1;
   return rc;
}

static db2_vault_rewrap_result_t void_scalar(aimee_pg_stmt_t *st)
{
   if (!st)
      return DB2_VAULT_REWRAP_INVALID;
   aimee_pg_step_t sr;
   db2_vault_rewrap_result_t rc = step(st, &sr);
   if (rc == DB2_VAULT_REWRAP_OK && sr != AIMEE_PG_ROW)
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   else if (rc == DB2_VAULT_REWRAP_OK)
      rc = expect_done(st);
   aimee_pg_finalize(st);
   return rc;
}

db2_vault_rewrap_result_t db2_vault_rewrap_stage_dek(db2_vault_rewrap_tx_t *tx,
                                                     const uint8_t opid[16], int64_t fence,
                                                     const db2_vault_rewrap_secret_t *s,
                                                     const uint8_t nw[VAULT_WRAPPED_DEK_LEN])
{
   if (!s || !nw || s->source_id < 1 || s->version < 1)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   if (claim_kind(tx, RW_KIND_STAGING) != DB2_VAULT_REWRAP_OK)
      return DB2_VAULT_REWRAP_INVALID;
   aimee_pg_stmt_t *st =
       prepare_op(tx, "SELECT org_vault_rewrap_stage_dek(?1,?2,?3,?4,?5,?6,?7,?8,?9)", opid, fence);
   if (!st || aimee_pg_bind_int64(st, "?3", s->source_id) != 0 ||
       aimee_pg_bind_text(st, "?4", s->principal) != 0 ||
       aimee_pg_bind_text(st, "?5", s->agent) != 0 || aimee_pg_bind_text(st, "?6", s->cred) != 0 ||
       aimee_pg_bind_int64(st, "?7", s->version) != 0 ||
       aimee_pg_bind_blob(st, "?8", s->source_digest, 32) != 0 ||
       aimee_pg_bind_blob(st, "?9", nw, VAULT_WRAPPED_DEK_LEN) != 0)
   {
      int had_stmt = st != NULL;
      aimee_pg_finalize(st);
      return prepare_or_bind_failure(tx, had_stmt);
   }
   db2_vault_rewrap_result_t rc = void_scalar(st);
   return rc == DB2_VAULT_REWRAP_OK ? rc : tx_fail(tx, rc);
}

db2_vault_rewrap_result_t db2_vault_rewrap_stage_check(db2_vault_rewrap_tx_t *tx,
                                                       const uint8_t opid[16], int64_t fence,
                                                       const db2_vault_rewrap_check_t *s,
                                                       const uint8_t *nw, size_t n)
{
   static const uint8_t empty = 0;
   if (!s || (!nw && n) || (n != 0 && n != VAULT_WRAPPED_DEK_LEN))
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   if (claim_kind(tx, RW_KIND_STAGING) != DB2_VAULT_REWRAP_OK)
      return DB2_VAULT_REWRAP_INVALID;
   aimee_pg_stmt_t *st =
       prepare_op(tx, "SELECT org_vault_rewrap_stage_check(?1,?2,?3,?4,?5)", opid, fence);
   if (!st || aimee_pg_bind_text(st, "?3", s->principal) != 0 ||
       aimee_pg_bind_blob(st, "?4", s->source_digest, 32) != 0 ||
       aimee_pg_bind_blob(st, "?5", n ? nw : &empty, (int)n) != 0)
   {
      int had_stmt = st != NULL;
      aimee_pg_finalize(st);
      return prepare_or_bind_failure(tx, had_stmt);
   }
   db2_vault_rewrap_result_t rc = void_scalar(st);
   return rc == DB2_VAULT_REWRAP_OK ? rc : tx_fail(tx, rc);
}

static db2_vault_rewrap_result_t simple_state(db2_vault_rewrap_tx_t *tx, const char *sql,
                                              const uint8_t opid[16], int64_t fence,
                                              const uint8_t *digest, const char *failure, int kind,
                                              int done_phase, uint32_t allowed_states)
{
   if (claim_kind(tx, kind) != DB2_VAULT_REWRAP_OK)
      return DB2_VAULT_REWRAP_INVALID;
   aimee_pg_stmt_t *st = prepare_op(tx, sql, opid, fence);
   if (!st)
      return tx_fail(tx, tx->prepare_error);
   if ((digest && aimee_pg_bind_blob(st, "?3", digest, 32) != 0) ||
       (failure && aimee_pg_bind_text(st, "?3", failure) != 0))
   {
      aimee_pg_finalize(st);
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   }
   db2_vault_rewrap_result_t rc = state_scalar(st, allowed_states, NULL);
   if (rc == DB2_VAULT_REWRAP_OK)
      tx->phase = done_phase;
   return rc == DB2_VAULT_REWRAP_OK ? rc : tx_fail(tx, rc);
}
db2_vault_rewrap_result_t
db2_vault_rewrap_inventory_summary(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                                   db2_vault_rewrap_inventory_summary_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!tx_owned(t) || t->kind != RW_KIND_STAGING || t->phase != RW_PHASE_STAGING || !out)
      return tx_fail(t, DB2_VAULT_REWRAP_INVALID);
   aimee_pg_stmt_t *st =
       prepare_op(t, "SELECT * FROM org_vault_rewrap_inventory_summary(?1,?2)", o, f);
   if (!st)
      return tx_fail(t, t->prepare_error);
   aimee_pg_step_t sr;
   db2_vault_rewrap_result_t rc = step(st, &sr);
   if (rc == DB2_VAULT_REWRAP_OK && sr == AIMEE_PG_ROW && !aimee_pg_column_is_null(st, 0) &&
       !aimee_pg_column_is_null(st, 1) && blob_exact(st, 2, out->inventory_digest, 32) == 0)
   {
      out->secret_count = aimee_pg_column_int64(st, 0);
      out->check_count = aimee_pg_column_int64(st, 1);
      rc = out->secret_count < 0 || out->check_count < 0 ? DB2_VAULT_REWRAP_INTEGRITY
                                                         : expect_done(st);
   }
   else if (rc == DB2_VAULT_REWRAP_OK)
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   aimee_pg_finalize(st);
   if (rc != DB2_VAULT_REWRAP_OK)
   {
      memset(out, 0, sizeof(*out));
      return tx_fail(t, rc);
   }
   return rc;
}

db2_vault_rewrap_result_t
db2_vault_rewrap_stage_finish(db2_vault_rewrap_tx_t *t, const uint8_t o[16], int64_t f,
                              const db2_vault_rewrap_inventory_summary_t *expected)
{
   if (!tx_owned(t) || t->kind != RW_KIND_STAGING || t->phase != RW_PHASE_STAGING ||
       !t->secret_exhausted || !t->check_exhausted || !expected || expected->secret_count < 0 ||
       expected->check_count < 0)
      return tx_fail(t, DB2_VAULT_REWRAP_INVALID);
   aimee_pg_stmt_t *st =
       prepare_op(t, "SELECT org_vault_rewrap_stage_finish(?1,?2,?3,?4,?5)", o, f);
   if (!st || aimee_pg_bind_int64(st, "?3", expected->secret_count) != 0 ||
       aimee_pg_bind_int64(st, "?4", expected->check_count) != 0 ||
       aimee_pg_bind_blob(st, "?5", expected->inventory_digest, 32) != 0)
   {
      int had_stmt = st != NULL;
      aimee_pg_finalize(st);
      return prepare_or_bind_failure(t, had_stmt);
   }
   db2_vault_rewrap_result_t rc =
       state_scalar(st,
                    RW_STATES_ALL & ~RW_STATE_BIT(DB2_VAULT_REWRAP_PREPARING) &
                        ~RW_STATE_BIT(DB2_VAULT_REWRAP_CUSTODY_PREPARED),
                    NULL);
   if (rc == DB2_VAULT_REWRAP_OK)
      t->phase = RW_PHASE_STAGING_DONE;
   return rc == DB2_VAULT_REWRAP_OK ? rc : tx_fail(t, rc);
}
db2_vault_rewrap_result_t db2_vault_rewrap_mark_committing(db2_vault_rewrap_tx_t *t,
                                                           const uint8_t o[16], int64_t f)
{
   return simple_state(
       t, "SELECT org_vault_rewrap_mark_committing(?1,?2)", o, f, NULL, NULL, RW_KIND_SINGLE,
       RW_PHASE_SINGLE_DONE,
       RW_STATE_BIT(DB2_VAULT_REWRAP_RESEAL_COMMITTING) | RW_STATE_BIT(DB2_VAULT_REWRAP_RESEALED) |
           RW_STATE_BIT(DB2_VAULT_REWRAP_PROMOTED) | RW_STATE_BIT(DB2_VAULT_REWRAP_COMPLETED) |
           RW_STATE_BIT(DB2_VAULT_REWRAP_RECOVERY_REQUIRED));
}
db2_vault_rewrap_result_t db2_vault_rewrap_mark_resealed(db2_vault_rewrap_tx_t *t,
                                                         const uint8_t o[16], int64_t f,
                                                         const uint8_t d[32])
{
   return d ? simple_state(t, "SELECT org_vault_rewrap_mark_resealed(?1,?2,?3)", o, f, d, NULL,
                           RW_KIND_SINGLE, RW_PHASE_SINGLE_DONE,
                           RW_STATE_BIT(DB2_VAULT_REWRAP_RESEALED) |
                               RW_STATE_BIT(DB2_VAULT_REWRAP_PROMOTED) |
                               RW_STATE_BIT(DB2_VAULT_REWRAP_COMPLETED) |
                               RW_STATE_BIT(DB2_VAULT_REWRAP_RECOVERY_REQUIRED))
            : tx_fail(t, DB2_VAULT_REWRAP_INVALID);
}
db2_vault_rewrap_result_t db2_vault_rewrap_promote(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                                   int64_t f)
{
   return simple_state(t, "SELECT org_vault_rewrap_promote(?1,?2)", o, f, NULL, NULL,
                       RW_KIND_PROMOTION, RW_PHASE_PROMOTION_DONE,
                       RW_STATE_BIT(DB2_VAULT_REWRAP_PROMOTED) |
                           RW_STATE_BIT(DB2_VAULT_REWRAP_COMPLETED) |
                           RW_STATE_BIT(DB2_VAULT_REWRAP_RECOVERY_REQUIRED));
}
db2_vault_rewrap_result_t db2_vault_rewrap_abort(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                                 int64_t f, const char *x)
{
   return x && x[0] ? simple_state(t, "SELECT org_vault_rewrap_abort(?1,?2,?3)", o, f, NULL, x,
                                   RW_KIND_SINGLE, RW_PHASE_SINGLE_DONE,
                                   RW_STATE_BIT(DB2_VAULT_REWRAP_ABORTED))
                    : tx_fail(t, DB2_VAULT_REWRAP_INVALID);
}
db2_vault_rewrap_result_t db2_vault_rewrap_recovery_required(db2_vault_rewrap_tx_t *t,
                                                             const uint8_t o[16], int64_t f,
                                                             const char *x)
{
   return x && x[0] ? simple_state(t, "SELECT org_vault_rewrap_recovery_required(?1,?2,?3)", o, f,
                                   NULL, x, RW_KIND_SINGLE, RW_PHASE_SINGLE_DONE,
                                   RW_STATE_BIT(DB2_VAULT_REWRAP_RECOVERY_REQUIRED))
                    : tx_fail(t, DB2_VAULT_REWRAP_INVALID);
}

db2_vault_rewrap_result_t db2_vault_rewrap_complete(db2_vault_rewrap_tx_t *t, const uint8_t o[16],
                                                    int64_t f, const uint8_t r[32],
                                                    const uint8_t i[32], const uint8_t s[32])
{
   if (!tx_owned(t))
      return DB2_VAULT_REWRAP_INVALID;
   if (t->phase != RW_PHASE_CRYPTO_ACKED || !o || !r || !i || !s || f != t->fence ||
       CRYPTO_memcmp(o, t->operation_id, 16) != 0 || CRYPTO_memcmp(r, t->receipt_digest, 32) != 0 ||
       CRYPTO_memcmp(i, t->inventory_digest, 32) != 0 || CRYPTO_memcmp(s, t->stage_digest, 32) != 0)
      return tx_fail(t, DB2_VAULT_REWRAP_INVALID);
   aimee_pg_stmt_t *st =
       prepare_verify(t, "SELECT org_vault_rewrap_complete(?1,?2,?3,?4,?5)", o, f);
   if (!st || aimee_pg_bind_blob(st, "?3", r, 32) != 0 ||
       aimee_pg_bind_blob(st, "?4", i, 32) != 0 || aimee_pg_bind_blob(st, "?5", s, 32) != 0)
   {
      int had_stmt = st != NULL;
      aimee_pg_finalize(st);
      return prepare_or_bind_failure(t, had_stmt);
   }
   db2_vault_rewrap_result_t rc = state_scalar(st, RW_STATE_BIT(DB2_VAULT_REWRAP_COMPLETED), NULL);
   if (rc == DB2_VAULT_REWRAP_OK)
      t->phase = RW_PHASE_COMPLETED;
   else
      tx_fail(t, rc);
   return rc;
}

db2_vault_rewrap_result_t db2_vault_rewrap_verify_summary(db2_vault_rewrap_tx_t *tx,
                                                          const uint8_t operation_id[16],
                                                          int64_t fence,
                                                          db2_vault_rewrap_verify_summary_t *out)
{
   if (out)
      db2_vault_rewrap_verify_summary_clear(out);
   if (!tx_owned(tx))
      return DB2_VAULT_REWRAP_INVALID;
   if (tx->phase != RW_PHASE_GENERAL || !operation_id || fence < 1 || !out)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   if (claim_kind(tx, RW_KIND_VERIFY) != DB2_VAULT_REWRAP_OK)
      return DB2_VAULT_REWRAP_INVALID;
   aimee_pg_stmt_t *st = prepare_verify(tx, "SELECT * FROM org_vault_rewrap_verify_summary(?1,?2)",
                                        operation_id, fence);
   if (!st)
      return tx_fail(tx, tx->prepare_error);
   aimee_pg_step_t sr;
   db2_vault_rewrap_result_t rc = step(st, &sr);
   if (rc == DB2_VAULT_REWRAP_OK && sr == AIMEE_PG_ROW && !aimee_pg_column_is_null(st, 0) &&
       !aimee_pg_column_is_null(st, 1) && blob_exact(st, 2, out->receipt_digest, 32) == 0 &&
       blob_exact(st, 3, out->inventory_digest, 32) == 0 &&
       blob_exact(st, 4, out->stage_digest, 32) == 0)
   {
      out->secret_count = aimee_pg_column_int64(st, 0);
      out->check_count = aimee_pg_column_int64(st, 1);
      if (out->secret_count < 0 || out->check_count < 0)
         rc = DB2_VAULT_REWRAP_INTEGRITY;
      else
         rc = expect_done(st);
   }
   else if (rc == DB2_VAULT_REWRAP_OK)
      rc = DB2_VAULT_REWRAP_INTEGRITY;
   aimee_pg_finalize(st);
   if (rc != DB2_VAULT_REWRAP_OK)
   {
      db2_vault_rewrap_verify_summary_clear(out);
      return tx_fail(tx, rc);
   }
   memcpy(tx->operation_id, operation_id, 16);
   tx->fence = fence;
   tx->expected_secrets = out->secret_count;
   tx->expected_checks = out->check_count;
   memcpy(tx->receipt_digest, out->receipt_digest, 32);
   memcpy(tx->inventory_digest, out->inventory_digest, 32);
   memcpy(tx->stage_digest, out->stage_digest, 32);
   tx->phase = RW_PHASE_VERIFY_SECRETS;
   return rc;
}

static int verify_identity(const db2_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                           int64_t fence)
{
   return operation_id && fence == tx->fence &&
          CRYPTO_memcmp(operation_id, tx->operation_id, 16) == 0;
}

db2_vault_rewrap_result_t
db2_vault_rewrap_verify_secret_page(db2_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                                    int64_t fence, int64_t after, int limit,
                                    db2_vault_rewrap_secret_t *rows, size_t cap, size_t *count)
{
   if (rows)
      db2_vault_rewrap_secret_clear(
          rows, cap > DB2_VAULT_REWRAP_PAGE_MAX ? DB2_VAULT_REWRAP_PAGE_MAX : cap);
   if (count)
      *count = 0;
   if (!tx_owned(tx))
      return DB2_VAULT_REWRAP_INVALID;
   if (tx->phase != RW_PHASE_VERIFY_SECRETS || !verify_identity(tx, operation_id, fence) || !rows ||
       !count || after < 0 || after != tx->last_secret_id || limit < 1 ||
       limit > DB2_VAULT_REWRAP_PAGE_MAX || cap < (size_t)limit)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   if (cap > DB2_VAULT_REWRAP_PAGE_MAX)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   aimee_pg_stmt_t *st = prepare_verify(
       tx, "SELECT * FROM org_vault_rewrap_verify_secret_page(?1,?2,?3,?4)", operation_id, fence);
   if (!st || aimee_pg_bind_int64(st, "?3", after) != 0 || aimee_pg_bind_int(st, "?4", limit) != 0)
   {
      int had_stmt = st != NULL;
      aimee_pg_finalize(st);
      return prepare_or_bind_failure(tx, had_stmt);
   }
   db2_vault_rewrap_result_t rc = DB2_VAULT_REWRAP_OK;
   aimee_pg_step_t sr;
   while ((rc = step(st, &sr)) == DB2_VAULT_REWRAP_OK && sr == AIMEE_PG_ROW)
   {
      if (*count >= (size_t)limit)
      {
         rc = DB2_VAULT_REWRAP_INTEGRITY;
         break;
      }
      db2_vault_rewrap_secret_t *r = &rows[(*count)++];
      r->source_id = aimee_pg_column_int64(st, 0);
      r->version = aimee_pg_column_int64(st, 4);
      if (r->source_id <= after || r->version < 1 ||
          text_copy(st, 1, r->principal, sizeof(r->principal)) != 0 ||
          text_copy(st, 2, r->agent, sizeof(r->agent)) != 0 ||
          text_copy(st, 3, r->cred, sizeof(r->cred)) != 0 ||
          blob_exact(st, 5, r->wrapped_dek, VAULT_WRAPPED_DEK_LEN) != 0)
      {
         rc = DB2_VAULT_REWRAP_INTEGRITY;
         break;
      }
      after = r->source_id;
   }
   aimee_pg_finalize(st);
   if (rc != DB2_VAULT_REWRAP_OK)
   {
      db2_vault_rewrap_secret_clear(rows, cap);
      *count = 0;
      return tx_fail(tx, rc);
   }
   if ((int64_t)*count > tx->expected_secrets - tx->consumed_secrets ||
       (*count > 0 && *count < (size_t)limit &&
        tx->consumed_secrets + (int64_t)*count != tx->expected_secrets) ||
       (*count == 0 && tx->consumed_secrets != tx->expected_secrets))
   {
      db2_vault_rewrap_secret_clear(rows, cap);
      *count = 0;
      return tx_fail(tx, DB2_VAULT_REWRAP_INTEGRITY);
   }
   tx->consumed_secrets += (int64_t)*count;
   if (*count)
      tx->last_secret_id = rows[*count - 1].source_id;
   if (*count == 0)
      tx->phase = RW_PHASE_VERIFY_CHECKS;
   return rc;
}

static int utf8_valid(const uint8_t *s, size_t n)
{
   size_t i = 0;
   while (i < n)
   {
      uint8_t c = s[i++];
      if (c == 0)
         return 0;
      if (c < 0x80)
         continue;
      unsigned need;
      uint32_t cp;
      if (c >= 0xc2 && c <= 0xdf)
         need = 1, cp = c & 0x1f;
      else if (c >= 0xe0 && c <= 0xef)
         need = 2, cp = c & 0x0f;
      else if (c >= 0xf0 && c <= 0xf4)
         need = 3, cp = c & 0x07;
      else
         return 0;
      if (n - i < need)
         return 0;
      for (unsigned j = 0; j < need; j++)
      {
         uint8_t d = s[i++];
         if ((d & 0xc0) != 0x80)
            return 0;
         cp = (cp << 6) | (d & 0x3f);
      }
      if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
          (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
         return 0;
   }
   return 1;
}

static int cursor_cmp(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
   size_t n = an < bn ? an : bn;
   int rc = n ? memcmp(a, b, n) : 0;
   return rc ? rc : (an > bn) - (an < bn);
}

db2_vault_rewrap_result_t
db2_vault_rewrap_verify_check_page(db2_vault_rewrap_tx_t *tx, const uint8_t operation_id[16],
                                   int64_t fence, const db2_vault_rewrap_cursor_t *after, int limit,
                                   db2_vault_rewrap_check_t *rows, size_t cap, size_t *count,
                                   db2_vault_rewrap_cursor_t *next)
{
   static const uint8_t empty = 0;
   if (rows)
      db2_vault_rewrap_check_clear(rows, cap > DB2_VAULT_REWRAP_PAGE_MAX ? DB2_VAULT_REWRAP_PAGE_MAX
                                                                         : cap);
   if (count)
      *count = 0;
   db2_vault_rewrap_cursor_t after_copy;
   int after_valid = after && after->len <= sizeof(after->bytes);
   if (after_valid)
      after_copy = *after;
   if (next)
      db2_vault_rewrap_cursor_clear(next);
   if (!tx_owned(tx))
      return DB2_VAULT_REWRAP_INVALID;
   if (tx->phase != RW_PHASE_VERIFY_CHECKS || !verify_identity(tx, operation_id, fence) ||
       !after_valid || after_copy.len != tx->check_cursor.len ||
       CRYPTO_memcmp(after_copy.bytes, tx->check_cursor.bytes, after_copy.len) != 0 || !rows ||
       !count || !next || limit < 1 || limit > DB2_VAULT_REWRAP_PAGE_MAX || cap < (size_t)limit)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   if (cap > DB2_VAULT_REWRAP_PAGE_MAX)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   after = &after_copy;
   *next = *after;
   aimee_pg_stmt_t *st = prepare_verify(
       tx, "SELECT * FROM org_vault_rewrap_verify_check_page(?1,?2,?3,?4)", operation_id, fence);
   if (!st ||
       aimee_pg_bind_blob(st, "?3", after->len ? after->bytes : &empty, (int)after->len) != 0 ||
       aimee_pg_bind_int(st, "?4", limit) != 0)
   {
      int had_stmt = st != NULL;
      aimee_pg_finalize(st);
      db2_vault_rewrap_cursor_clear(next);
      return prepare_or_bind_failure(tx, had_stmt);
   }
   db2_vault_rewrap_result_t rc = DB2_VAULT_REWRAP_OK;
   aimee_pg_step_t sr;
   while ((rc = step(st, &sr)) == DB2_VAULT_REWRAP_OK && sr == AIMEE_PG_ROW)
   {
      if (*count >= (size_t)limit)
      {
         rc = DB2_VAULT_REWRAP_INTEGRITY;
         break;
      }
      db2_vault_rewrap_check_t *r = &rows[(*count)++];
      if (text_copy(st, 0, r->principal, sizeof(r->principal)) != 0 ||
          aimee_pg_column_is_null(st, 1) || aimee_pg_column_is_null(st, 2))
      {
         rc = DB2_VAULT_REWRAP_INTEGRITY;
         break;
      }
      int n = aimee_pg_column_bytes(st, 1);
      if (n != 0 && n != VAULT_WRAPPED_DEK_LEN)
      {
         rc = DB2_VAULT_REWRAP_INTEGRITY;
         break;
      }
      if (n)
      {
         const void *p = aimee_pg_column_blob(st, 1);
         if (!p)
         {
            rc = DB2_VAULT_REWRAP_INTEGRITY;
            break;
         }
         memcpy(r->kek_check, p, (size_t)n);
      }
      r->kek_check_len = (size_t)n;
      int cn = aimee_pg_column_bytes(st, 2);
      const uint8_t *cursor = aimee_pg_column_blob(st, 2);
      size_t pn = strlen(r->principal);
      const db2_vault_rewrap_cursor_t *previous = *count == 1 ? after : next;
      if (cn < 1 || cn > (int)sizeof(next->bytes) || !cursor || pn != (size_t)cn ||
          memcmp(r->principal, cursor, pn) != 0 || !utf8_valid(cursor, (size_t)cn) ||
          cursor_cmp(cursor, (size_t)cn, previous->bytes, previous->len) <= 0)
      {
         rc = DB2_VAULT_REWRAP_INTEGRITY;
         break;
      }
      memcpy(next->bytes, cursor, (size_t)cn);
      next->len = (size_t)cn;
   }
   aimee_pg_finalize(st);
   if (rc != DB2_VAULT_REWRAP_OK)
   {
      db2_vault_rewrap_check_clear(rows, cap);
      *count = 0;
      memset(next, 0, sizeof(*next));
      return tx_fail(tx, rc);
   }
   if ((int64_t)*count > tx->expected_checks - tx->consumed_checks ||
       (*count > 0 && *count < (size_t)limit &&
        tx->consumed_checks + (int64_t)*count != tx->expected_checks) ||
       (*count == 0 && tx->consumed_checks != tx->expected_checks))
   {
      db2_vault_rewrap_check_clear(rows, cap);
      *count = 0;
      memset(next, 0, sizeof(*next));
      return tx_fail(tx, DB2_VAULT_REWRAP_INTEGRITY);
   }
   tx->consumed_checks += (int64_t)*count;
   tx->check_cursor = *next;
   if (*count == 0)
      tx->phase = RW_PHASE_VERIFY_CONSUMED;
   return rc;
}

db2_vault_rewrap_result_t db2_vault_rewrap_verify_crypto_ack(db2_vault_rewrap_tx_t *tx,
                                                             const uint8_t operation_id[16],
                                                             int64_t fence)
{
   if (!tx_owned(tx))
      return DB2_VAULT_REWRAP_INVALID;
   if (tx->phase != RW_PHASE_VERIFY_CONSUMED || !verify_identity(tx, operation_id, fence) ||
       tx->consumed_secrets != tx->expected_secrets || tx->consumed_checks != tx->expected_checks)
      return tx_fail(tx, DB2_VAULT_REWRAP_INVALID);
   tx->phase = RW_PHASE_CRYPTO_ACKED;
   return DB2_VAULT_REWRAP_OK;
}

const db2_vault_rewrap_ops_t db2_vault_rewrap_default_ops = {
    .tx_begin = db2_vault_rewrap_tx_begin,
    .tx_commit = db2_vault_rewrap_tx_commit,
    .tx_rollback = db2_vault_rewrap_tx_rollback,
    .snapshot = db2_vault_rewrap_snapshot,
    .begin = db2_vault_rewrap_begin,
    .record_prepared = db2_vault_rewrap_record_prepared,
    .source_secret_page = db2_vault_rewrap_source_secret_page,
    .source_check_page = db2_vault_rewrap_source_check_page,
    .stage_dek = db2_vault_rewrap_stage_dek,
    .stage_check = db2_vault_rewrap_stage_check,
    .inventory_summary = db2_vault_rewrap_inventory_summary,
    .stage_finish = db2_vault_rewrap_stage_finish,
    .mark_committing = db2_vault_rewrap_mark_committing,
    .mark_resealed = db2_vault_rewrap_mark_resealed,
    .promote = db2_vault_rewrap_promote,
    .abort = db2_vault_rewrap_abort,
    .recovery_required = db2_vault_rewrap_recovery_required,
    .verify_summary = db2_vault_rewrap_verify_summary,
    .verify_secret_page = db2_vault_rewrap_verify_secret_page,
    .verify_check_page = db2_vault_rewrap_verify_check_page,
    .verify_crypto_ack = db2_vault_rewrap_verify_crypto_ack,
    .complete = db2_vault_rewrap_complete,
};
