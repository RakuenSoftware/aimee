#include "db2/db2_pool.h"
#include "db2/db_postgres.h"
#include "db2/org_vault_rewrap.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct aimee_pg_stmt
{
   int step;
};

static int g_conn, g_returns, g_discards;
static const char *g_fail_command;
static const char *g_fail_state;
static int g_step_fail_at;
static const char *g_stmt_state = "XX000";
static aimee_pg_prepare_error_t g_prepare_failure = AIMEE_PG_PREPARE_OK;

int db2_pool_active(void)
{
   return 1;
}
void *db2_pool_lease(int timeout_ms)
{
   (void)timeout_ms;
   return &g_conn;
}
void db2_pool_return(void *conn)
{
   assert(conn == &g_conn);
   g_returns++;
}
void db2_pool_discard(void *conn)
{
   assert(conn == &g_conn);
   g_discards++;
}

int aimee_pg_exec_sqlstate(void *conn, const char *sql, char state[6], char *err, size_t errlen)
{
   (void)conn;
   (void)err;
   (void)errlen;
   state[0] = '\0';
   if (g_fail_command && strstr(sql, g_fail_command))
   {
      snprintf(state, 6, "%s", g_fail_state);
      return -1;
   }
   return 0;
}

static struct aimee_pg_stmt g_stmt;

aimee_pg_stmt_t *aimee_pg_prepare_ex(void *c, const char *s, aimee_pg_prepare_error_t *kind,
                                     char *e, size_t n)
{
   (void)c;
   (void)s;
   (void)e;
   (void)n;
   if (g_prepare_failure != AIMEE_PG_PREPARE_OK)
   {
      if (kind)
         *kind = g_prepare_failure;
      return NULL;
   }
   g_stmt.step = 0;
   if (kind)
      *kind = AIMEE_PG_PREPARE_OK;
   return &g_stmt;
}
aimee_pg_stmt_t *aimee_pg_prepare(void *c, const char *s, char *e, size_t n)
{
   return aimee_pg_prepare_ex(c, s, NULL, e, n);
}
void aimee_pg_finalize(aimee_pg_stmt_t *s)
{
   (void)s;
}
aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *s, char *e, size_t n)
{
   (void)e;
   (void)n;
   if (g_step_fail_at && s->step + 1 == g_step_fail_at)
   {
      s->step++;
      return AIMEE_PG_ERR;
   }
   return s->step++ == 0 ? AIMEE_PG_ROW : AIMEE_PG_DONE;
}
const char *aimee_pg_sqlstate(const aimee_pg_stmt_t *s)
{
   (void)s;
   return g_stmt_state;
}
int aimee_pg_bind_int(aimee_pg_stmt_t *s, const char *n, int v)
{
   (void)s;
   (void)n;
   (void)v;
   return 0;
}
int aimee_pg_bind_int64(aimee_pg_stmt_t *s, const char *n, int64_t v)
{
   (void)s;
   (void)n;
   (void)v;
   return 0;
}
int aimee_pg_bind_text(aimee_pg_stmt_t *s, const char *n, const char *v)
{
   (void)s;
   (void)n;
   (void)v;
   return 0;
}
int aimee_pg_bind_blob(aimee_pg_stmt_t *s, const char *n, const void *v, int z)
{
   (void)s;
   (void)n;
   (void)v;
   (void)z;
   return 0;
}
int aimee_pg_column_is_null(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 1;
}
int aimee_pg_column_bytes(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 0;
}
const void *aimee_pg_column_blob(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return NULL;
}
const char *aimee_pg_column_text(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return "promoted";
}
int64_t aimee_pg_column_int64(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 1;
}

static void test_sqlstate_mapping(void)
{
   assert(db2_vault_rewrap_classify_sqlstate("P7C01") == DB2_VAULT_REWRAP_CONFLICT);
   assert(db2_vault_rewrap_classify_sqlstate("P7I01") == DB2_VAULT_REWRAP_INTEGRITY);
   assert(db2_vault_rewrap_classify_sqlstate("40001") == DB2_VAULT_REWRAP_TRANSIENT);
   assert(db2_vault_rewrap_classify_sqlstate("40P01") == DB2_VAULT_REWRAP_TRANSIENT);
   assert(db2_vault_rewrap_classify_sqlstate("55000") == DB2_VAULT_REWRAP_BUSY);
   assert(db2_vault_rewrap_classify_sqlstate(NULL) == DB2_VAULT_REWRAP_ERROR);
}

static void *wrong_thread_commit(void *p)
{
   db2_vault_rewrap_tx_t **tx = p;
   assert(db2_vault_rewrap_tx_commit(tx) == DB2_VAULT_REWRAP_INVALID);
   return NULL;
}

static void test_public_handle_phases(void)
{
   db2_vault_rewrap_tx_t *tx = NULL;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   pthread_t other;
   assert(pthread_create(&other, NULL, wrong_thread_commit, &tx) == 0);
   assert(pthread_join(other, NULL) == 0);
   assert(tx != NULL);

   db2_vault_rewrap_verify_summary_t summary;
   assert(db2_vault_rewrap_verify_summary(tx, NULL, 1, &summary) == DB2_VAULT_REWRAP_INVALID);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID);
   assert(tx != NULL);
   db2_vault_rewrap_tx_rollback(&tx);
   assert(tx == NULL && g_returns == 1);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   g_fail_command = "COMMIT";
   g_fail_state = "40001";
   assert(db2_vault_rewrap_mark_committing(tx, (const uint8_t[16]){1}, 1) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_TRANSIENT);
   assert(tx == NULL && g_discards == 1);

   g_fail_state = "P7C01";
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_mark_committing(tx, (const uint8_t[16]){1}, 1) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_TRANSIENT);
   assert(tx == NULL && g_discards == 2);

   g_fail_command = "ROLLBACK";
   g_fail_state = "08006";
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   db2_vault_rewrap_tx_rollback(&tx);
   assert(tx == NULL && g_discards == 3);
}

static void test_transaction_kind_gates(void)
{
   uint8_t op[16] = {1};
   db2_vault_rewrap_tx_t *tx = NULL;
   g_fail_command = NULL;

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID && tx);
   db2_vault_rewrap_tx_rollback(&tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_mark_committing(tx, op, 1) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && !tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_promote(tx, op, 1) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_mark_committing(tx, op, 1) == DB2_VAULT_REWRAP_INVALID);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID && tx);
   db2_vault_rewrap_tx_rollback(&tx);

   db2_vault_rewrap_secret_t source = {.source_id = 1, .version = 1};
   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN] = {0};
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_stage_dek(tx, op, 1, &source, wrapped) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID && tx);
   db2_vault_rewrap_tx_rollback(&tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_stage_finish(tx, op, 1) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && !tx);
}

static void test_prepare_and_followup_failures(void)
{
   uint8_t op[16] = {1};
   db2_vault_rewrap_tx_t *tx = NULL;
   g_fail_command = NULL;

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   g_prepare_failure = AIMEE_PG_PREPARE_RESOURCE;
   assert(db2_vault_rewrap_mark_committing(tx, op, 1) == DB2_VAULT_REWRAP_ERROR);
   g_prepare_failure = AIMEE_PG_PREPARE_OK;
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID && tx);
   db2_vault_rewrap_tx_rollback(&tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   g_step_fail_at = 2;
   g_stmt_state = "P7C01";
   assert(db2_vault_rewrap_mark_committing(tx, op, 1) == DB2_VAULT_REWRAP_CONFLICT);
   g_step_fail_at = 0;
   g_stmt_state = "XX000";
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_INVALID && tx);
   db2_vault_rewrap_tx_rollback(&tx);
}

static void test_receipt_identity_binding(void)
{
   uint8_t op[16] = {1}, other_op[16] = {2};
   uint8_t wire[VAULT_RESEAL_RECEIPT_V1_LEN];
   vault_tpm2_reseal_receipt_t receipt;
   memset(&receipt, 0, sizeof(receipt));
   memcpy(receipt.operation_id, op, sizeof(op));
   receipt.old_generation = 7;
   receipt.new_generation = 8;
   assert(vault_reseal_receipt_encode(&receipt, wire) == 0);

   db2_vault_rewrap_tx_t *tx = NULL;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_record_prepared(tx, other_op, 1, 7, 8, wire) ==
          DB2_VAULT_REWRAP_INVALID);
   db2_vault_rewrap_tx_rollback(&tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_record_prepared(tx, op, 1, 6, 7, wire) == DB2_VAULT_REWRAP_INVALID);
   db2_vault_rewrap_tx_rollback(&tx);

   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_record_prepared(tx, op, 1, 7, 8, wire) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_tx_commit(&tx) == DB2_VAULT_REWRAP_OK && !tx);
}

static void test_page_cap_rejection(void)
{
   uint8_t op[16] = {1};
   size_t count = 99;
   db2_vault_rewrap_secret_t *rows = calloc(DB2_VAULT_REWRAP_PAGE_MAX + 1, sizeof(*rows));
   assert(rows);
   db2_vault_rewrap_tx_t *tx = NULL;
   assert(db2_vault_rewrap_tx_begin(&tx) == DB2_VAULT_REWRAP_OK);
   assert(db2_vault_rewrap_source_secret_page(tx, op, 1, 0, 1, rows, DB2_VAULT_REWRAP_PAGE_MAX + 1,
                                              &count) == DB2_VAULT_REWRAP_INVALID);
   assert(count == 0);
   db2_vault_rewrap_tx_rollback(&tx);
   free(rows);
}

static void test_output_clear_helpers(void)
{
   db2_vault_rewrap_secret_t secrets[2];
   db2_vault_rewrap_check_t checks[2];
   db2_vault_rewrap_cursor_t cursor;
   db2_vault_rewrap_verify_summary_t summary;
   memset(secrets, 0xa5, sizeof(secrets));
   memset(checks, 0xa5, sizeof(checks));
   memset(&cursor, 0xa5, sizeof(cursor));
   memset(&summary, 0xa5, sizeof(summary));
   db2_vault_rewrap_secret_clear(secrets, 2);
   db2_vault_rewrap_check_clear(checks, 2);
   db2_vault_rewrap_cursor_clear(&cursor);
   db2_vault_rewrap_verify_summary_clear(&summary);
   for (size_t i = 0; i < sizeof(secrets); i++)
      assert(((const unsigned char *)secrets)[i] == 0);
   for (size_t i = 0; i < sizeof(checks); i++)
      assert(((const unsigned char *)checks)[i] == 0);
   for (size_t i = 0; i < sizeof(cursor); i++)
      assert(((const unsigned char *)&cursor)[i] == 0);
   for (size_t i = 0; i < sizeof(summary); i++)
      assert(((const unsigned char *)&summary)[i] == 0);
}

int main(void)
{
   test_sqlstate_mapping();
   test_public_handle_phases();
   test_transaction_kind_gates();
   test_prepare_and_followup_failures();
   test_receipt_identity_binding();
   test_page_cap_rejection();
   test_output_clear_helpers();
   puts("org_vault_rewrap: public phase/SQLSTATE tests passed");
   return 0;
}
